/*
 * fusiond v2 - merge every connected gamepad into ONE virtual controller
 *              with dynamic control socket + per-pad layout remapping.
 *
 * Part of the "Fusion Controller" Magisk module for rooted Android.
 * New in v2:
 *   - Unix domain socket at /data/adb/fusion.sock for runtime control
 *   - Dynamic HIDE ON/OFF (EVIOCGRAB, reversible without reboot)
 *   - Per-pad layout remapping (NINTENDO vs XBOX button order)
 *   - STATUS query returns JSON with current state
 *
 * Socket commands (text, one per line):
 *   HIDE ON              grab all physical pads (hide from Android)
 *   HIDE OFF             release all physical pads
 *   LAYOUT <idx> <mode>  mode = NINTENDO | XBOX | DEFAULT
 *   STATUS               returns JSON
 *   QUIT                 graceful shutdown
 *
 * Build (static, for Android arm64):
 *   aarch64-linux-musl-gcc -O2 -Wall -static -o fusiond fusiond_v2.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sched.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/uinput.h>

#ifndef EVIOCREVOKE
#define EVIOCREVOKE _IOW('E', 0x91, int)
#endif
#ifndef BTN_TRIGGER_HAPPY1
#define BTN_TRIGGER_HAPPY1 0x2c0
#endif
#ifndef BTN_TRIGGER_HAPPY24
#define BTN_TRIGGER_HAPPY24 (BTN_TRIGGER_HAPPY1 + 23)
#endif

#define VERSION     "2.3.0"
#define MAX_PADS    16
#define MAX_USER_MAP 256
#define FF_SLOTS    16
#define SOCK_PATH   "/data/adb/fusion.sock"
#define MAX_CLIENTS 4

static const char *FUSION_NAME = "Fusion Controller";
static const char *MAP_PATH    = "/data/adb/fusion.map";
static const char *FUSION_PHYS = "fusion-bridge";

static int hide_mode = 1;   /* v2.2.0: hide ON by default (--no-hide starts released) */
static int rumble_on = 1;
static int debug;
static volatile sig_atomic_t g_quit;
static volatile sig_atomic_t g_restart;
static volatile sig_atomic_t g_rescan;
static char **g_argv;
static unsigned long long g_act_counter;

#define BITS_PER_UL (sizeof(unsigned long) * 8)
static inline void bit_set(unsigned long *a, int b)   { a[b / BITS_PER_UL] |=  1UL << (b % BITS_PER_UL); }
static inline void bit_clear(unsigned long *a, int b) { a[b / BITS_PER_UL] &= ~(1UL << (b % BITS_PER_UL)); }
static inline int  bit_test(const unsigned long *a, int b) { return !!(a[b / BITS_PER_UL] & (1UL << (b % BITS_PER_UL))); }

struct axis_range { int min, max, neu; };   /* neu = rest value at connect */

enum layout_mode { LAYOUT_DEFAULT, LAYOUT_XBOX, LAYOUT_NINTENDO };

struct pad {
    int fd;
    char node[128];
    char name[128];
    int  hidden;
    int  ff_supported;
    unsigned long long act;
    struct axis_range ax[ABS_CNT];
    unsigned long axbits[ABS_CNT / BITS_PER_UL + 1];
    int out_code[ABS_CNT];   /* source axis -> fusion axis (-1 = dropped) */
    /* dynamic stick/trigger disambiguation state (see disambiguate) */
    unsigned char rx_seen, ry_seen;      /* RX/RY ever emitted events */
    unsigned char amb_z, amb_rz;         /* Z/RZ center-rest while RX/RY declared */
    unsigned char z_evt_seen, rz_evt_seen; /* ambiguous axis has emitted events */
    time_t z_last_evt, rz_last_evt;        /* last event time (quiet window) */
    unsigned char z_is_stick, rz_is_stick; /* authoritative reroute result */
    unsigned long keys[KEY_MAX / BITS_PER_UL + 1];
    enum layout_mode layout;
};

static struct pad pads[MAX_PADS];
static int npads;
static int pad_pfd_idx[MAX_PADS];   /* pfds[] index of each pad, snapshot at build */
static int ufd = -1;
static int sockfd = -1;
static int client_fds[MAX_CLIENTS];
static time_t client_since[MAX_CLIENTS];
static int client_uid[MAX_CLIENTS];   /* SO_PEERCRED of each client */
static int client_pid[MAX_CLIENTS];

/* ---------------------------------------------------------------- logging */
static void logmsg(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("[%ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ------------------------------------------------------- code translation */
struct cmap { unsigned short from, to; };

static const struct cmap def_map[] = {
    { BTN_TRIGGER, BTN_SOUTH  }, { BTN_THUMB,   BTN_EAST   },
    { BTN_THUMB2,  BTN_X      }, { BTN_TOP,     BTN_Y      },
    { BTN_TOP2,    BTN_TL     }, { BTN_PINKIE,  BTN_TR     },
    { BTN_BASE,    BTN_TL2    }, { BTN_BASE2,   BTN_TR2    },
    { BTN_BASE3,   BTN_SELECT }, { BTN_BASE4,   BTN_START  },
    { BTN_BASE5,   BTN_THUMBL }, { BTN_BASE6,   BTN_THUMBR },
};

static struct cmap user_map[MAX_USER_MAP];
static int n_user_map;

static unsigned short map_code(unsigned short from)
{
    int i;
    for (i = 0; i < n_user_map; i++)
        if (user_map[i].from == from) return user_map[i].to;
    for (i = 0; i < (int)(sizeof(def_map) / sizeof(def_map[0])); i++)
        if (def_map[i].from == from) return def_map[i].to;
    return from;
}

static void load_user_map(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[128];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int from, to;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%i %i", &from, &to) == 2) {
            if (n_user_map < MAX_USER_MAP) {
                user_map[n_user_map].from = (unsigned short)from;
                user_map[n_user_map].to   = (unsigned short)to;
                n_user_map++;
            }
        }
    }
    fclose(f);
    logmsg("loaded %d user code mappings from %s", n_user_map, path);
}

/* ------------------------------------------------ per-pad layout remapping */
static unsigned short remap_layout(struct pad *p, unsigned short code)
{
    if (p->layout == LAYOUT_NINTENDO) {
        /* Nintendo: B bottom, A right, Y left, X top
         * Standard (Xbox): A bottom, B right, X left, Y top */
        if (code == BTN_SOUTH) return BTN_EAST;   /* A -> B */
        if (code == BTN_EAST)  return BTN_SOUTH;  /* B -> A */
        if (code == BTN_WEST)  return BTN_NORTH;  /* X -> Y */
        if (code == BTN_NORTH) return BTN_WEST;   /* Y -> X */
    }
    /* LAYOUT_XBOX and LAYOUT_DEFAULT: identity (standard Xbox order) */
    return code;
}

/* ------------------------------------------------- fusion axis capabilities */
static const int fusion_axes[] = {
    ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ,
    ABS_HAT0X, ABS_HAT0Y, ABS_HAT1X, ABS_HAT1Y, ABS_HAT2X, ABS_HAT2Y,
};

static int fusion_has_axis(int code)
{
    unsigned i;
    for (i = 0; i < sizeof(fusion_axes) / sizeof(fusion_axes[0]); i++)
        if (fusion_axes[i] == code) return 1;
    return 0;
}

static void fusion_axis_range(int code, int *mn, int *mx)
{
    switch (code) {
    case ABS_X: case ABS_Y: case ABS_RX: case ABS_RY:
        *mn = -32768; *mx = 32767; break;
    case ABS_Z: case ABS_RZ:
        *mn = 0; *mx = 255; break;
    default:
        *mn = -1; *mx = 1; break;
    }
}

static int scale_value(int v, int smin, int smax, int sneu, int fmin, int fmax)
{
    double r;
    if (v < smin) v = smin;
    if (v > smax) v = smax;
    if (smax <= smin) return 0;              /* degenerate: fusion-neutral */
    if (v >= sneu) {
        if (smax <= sneu) return fmax;       /* rest == max edge */
        r = (double)(v - sneu) * fmax / (double)(smax - sneu);
    } else {
        if (sneu <= smin) return fmin;       /* rest == min edge */
        r = (double)(v - sneu) * (double)(0 - fmin) / (double)(sneu - smin);
    }
    if (r < fmin) r = fmin;
    if (r > fmax) r = fmax;
    return (int)(r + ((r < 0) ? -0.5 : 0.5));
}

/* --------------------------------------------------------------- emission */
static unsigned long last_union[KEY_MAX / BITS_PER_UL + 1];
static int last_axis[ABS_CNT];

static unsigned long long evtx_count, evtx_fail;

static void emit(struct input_event *ev)
{
    if (write(ufd, ev, sizeof(*ev)) < 0) {
        evtx_fail++;
        if (evtx_fail <= 5 || (evtx_fail % 100) == 0)
            logmsg("emit FAILED #%llu: %s", evtx_fail, strerror(errno));
        return;
    }
    evtx_count++;
}

static void emit3(int type, int code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    gettimeofday(&ev.time, NULL);
    ev.type = (uint16_t)type;
    ev.code = (uint16_t)code;
    ev.value = val;
    emit(&ev);
}

/* ----------------------------------------------------------- force feedback */
static int ff_pad_idx[FF_SLOTS];
static int ff_pad_id[FF_SLOTS];

static void handle_ff(void)
{
    struct input_event ev;
    ssize_t r;
    while ((r = read(ufd, &ev, sizeof ev)) == sizeof ev) {
        int i, newest = -1;
        unsigned long long newest_act = 0;
        if (ev.type != EV_FF || !rumble_on) continue;
        if (ev.code == FF_GAIN || ev.code == FF_AUTOCENTER) continue;
        for (i = 0; i < FF_SLOTS; i++) {
            if (ff_pad_id[i] == (int)ev.code) {
                int pi = ff_pad_idx[i];
                if (pi >= 0 && pi < npads && pads[pi].ff_supported) {
                    struct input_event fev = ev;
                    fev.code = (uint16_t)i;
                    write(pads[pi].fd, &fev, sizeof fev);
                }
                return;
            }
        }
        for (i = 0; i < npads; i++) {
            if (pads[i].ff_supported && pads[i].act > newest_act) {
                newest_act = pads[i].act;
                newest = i;
            }
        }
        if (newest >= 0) {
            struct ff_effect eff;
            int slot = -1;
            for (i = 0; i < FF_SLOTS; i++) {
                if (ff_pad_idx[i] < 0) { slot = i; break; }
            }
            if (slot < 0) slot = 0;
            memset(&eff, 0, sizeof eff);
            eff.type = FF_RUMBLE;
            eff.id = slot;
            eff.replay.length = 1000;
            eff.u.rumble.strong_magnitude = 0x8000;
            eff.u.rumble.weak_magnitude = 0x4000;
            if (ioctl(pads[newest].fd, EVIOCSFF, &eff) == 0) {
                struct input_event play;
                ff_pad_idx[slot] = newest;
                ff_pad_id[slot] = (int)ev.code;
                memset(&play, 0, sizeof play);
                play.type = EV_FF; play.code = (uint16_t)slot; play.value = 1;
                write(pads[newest].fd, &play, sizeof play);
            }
        }
    }
}

/* --------------------------------------------------------- event processing */
static void release_pad_keys(int idx)
{
    struct pad *p = &pads[idx];
    unsigned i;
    for (i = 0; i < KEY_MAX; i++) {
        if (bit_test(p->keys, (int)i)) {
            int j, any = 0;
            bit_clear(p->keys, (int)i);
            for (j = 0; j < npads; j++)
                if (j != idx && bit_test(pads[j].keys, (int)i)) { any = 1; break; }
            if (!any && bit_test(last_union, (int)i)) {
                emit3(EV_KEY, (int)i, 0);
                bit_clear(last_union, (int)i);
            }
        }
    }
}

/* --- dynamic stick/trigger disambiguation -----------------------------------
 * Some pads (8BitDo in DInput/BT mode) DECLARE ABS_RX/ABS_RY but never emit
 * them, driving the right stick on ABS_Z/ABS_RZ. route_axes() then assumes
 * triggers. Discriminator: after the axis goes quiet for ~1s, read where it
 * PARKS - triggers park at min, sticks park mid-range. RX/RY activity vetoes
 * the stick interpretation (a real right stick lives there). */

static void disambiguate(struct pad *p, const struct input_event *ev)
{
    switch (ev->code) {
    case ABS_RX: p->rx_seen = 1; return;
    case ABS_RY: p->ry_seen = 1; return;
    case ABS_Z:
        if (p->amb_z)  { p->z_evt_seen = 1;  p->z_last_evt = time(NULL); }
        return;
    case ABS_RZ:
        if (p->amb_rz) { p->rz_evt_seen = 1; p->rz_last_evt = time(NULL); }
        return;
    default: return;
    }
}

/* pure decision core (unit-testable): classify from a parked value */
static void classify_axis(struct pad *p, int code, int observed)
{
    unsigned char *ambp, *stickp;
    int out, phantom, lo, hi, span;
    struct axis_range *a = &p->ax[code];

    if (code == ABS_Z) { ambp = &p->amb_z;  stickp = &p->z_is_stick;  out = ABS_RX; }
    else               { ambp = &p->amb_rz; stickp = &p->rz_is_stick; out = ABS_RY; }
    if (!*ambp || *stickp) return;

    lo = a->min; hi = a->max;
    span = hi - lo; if (span < 8) span = 8;

    if (observed <= lo + span / 8) {
        *ambp = 0;                    /* parks at min: real trigger */
        a->neu = lo;
        logmsg("%s: %s parks at min - confirmed trigger",
               p->name, code == ABS_Z ? "Z" : "RZ");
        return;
    }
    if (observed >= lo + (span * 3) / 8) {
        phantom = (code == ABS_Z) ? p->rx_seen : p->ry_seen;
        if (phantom) {
            *ambp = 0;                /* real stick lives on RX/RY: keep trigger */
            a->neu = lo;
            logmsg("%s: %s parks mid but %s active - keeping as trigger",
                   p->name, code == ABS_Z ? "Z" : "RZ", code == ABS_Z ? "RX" : "RY");
            return;
        }
        *ambp = 0;                    /* parks mid-range: it is the right stick */
        *stickp = 1;
        p->out_code[code] = out;
        p->out_code[out]  = -1;       /* silent phantom axis loses */
        if (last_axis[code] != 0) {   /* release any stale L2/R2 */
            emit3(EV_ABS, code, 0);
            emit3(EV_SYN, SYN_REPORT, 0);
            last_axis[code] = 0;
        }
        last_axis[out] = -999999;     /* force stick re-emit */
        logmsg("%s: %s parks mid-range, %s silent - rerouting %s->%s (right stick)",
               p->name, code == ABS_Z ? "Z" : "RZ", code == ABS_Z ? "RX" : "RY",
               code == ABS_Z ? "Z" : "RZ", code == ABS_Z ? "RX" : "RY");
    }
    /* between the bands: undecided - re-check on the next quiet window */
}

/* called from the main loop: classify ambiguous axes that went quiet */
static void classify_ambiguous(void)
{
    int i;
    time_t now = time(NULL);
    for (i = 0; i < npads; i++) {
        struct pad *p = &pads[i];
        struct input_absinfo ai;
        if (p->fd < 0) continue;
        if (p->amb_z && p->z_evt_seen && now - p->z_last_evt >= 1 &&
            ioctl(p->fd, EVIOCGABS(ABS_Z), &ai) == 0)
            classify_axis(p, ABS_Z, ai.value);
        if (p->amb_rz && p->rz_evt_seen && now - p->rz_last_evt >= 1 &&
            ioctl(p->fd, EVIOCGABS(ABS_RZ), &ai) == 0)
            classify_axis(p, ABS_RZ, ai.value);
    }
}

/* System keys that must survive fusion forwarding: keys below BTN_MISC are
 * otherwise dropped by the gamepad filter. KEY_BACK is the RP5's back button;
 * HOMEPAGE/HOME cover other vendors' home keys; volume in case a pad carries
 * the rocker. */
static int is_forwarded_key(unsigned short c)
{
    switch (c) {
    case KEY_BACK:
    case KEY_HOMEPAGE:
    case KEY_HOME:
    case KEY_MENU:
    case KEY_VOLUMEUP:
    case KEY_VOLUMEDOWN:
    case KEY_MUTE:
        return 1;
    default:
        return 0;
    }
}

static void process_event(struct pad *p, int pidx, struct input_event *ev)
{
    unsigned short mapped;
    int i, fmin, fmax;

    p->act = ++g_act_counter;


    if (ev->type == EV_KEY &&
        ((ev->code >= BTN_MISC && ev->code < KEY_MAX) || is_forwarded_key(ev->code))) {
        mapped = map_code(ev->code);
        mapped = remap_layout(p, mapped);  /* apply per-pad layout */

        if (ev->value) {
            if (!bit_test(p->keys, mapped)) {
                bit_set(p->keys, mapped);
                if (!bit_test(last_union, mapped)) {
                    emit3(EV_KEY, mapped, 1);
                    bit_set(last_union, mapped);
                }
            }
        } else {
            if (bit_test(p->keys, mapped)) {
                bit_clear(p->keys, mapped);
                for (i = 0; i < npads; i++) {
                    if (i != pidx && bit_test(pads[i].keys, mapped)) {
                        emit3(EV_SYN, SYN_REPORT, 0);
                        return;
                    }
                }
                if (bit_test(last_union, mapped)) {
                    emit3(EV_KEY, mapped, 0);
                    bit_clear(last_union, mapped);
                }
            }
        }
        emit3(EV_SYN, SYN_REPORT, 0);
        return;
    }

    if (ev->type == EV_ABS) {
        int out;
        disambiguate(p, ev);
        out = p->out_code[ev->code];
        if (out >= 0 && fusion_has_axis(out)) {
            int scaled;
            fusion_axis_range(out, &fmin, &fmax);
            scaled = scale_value(ev->value, p->ax[ev->code].min, p->ax[ev->code].max, p->ax[ev->code].neu, fmin, fmax);
            if (scaled != last_axis[out]) {
                last_axis[out] = scaled;
                emit3(EV_ABS, out, scaled);
                emit3(EV_SYN, SYN_REPORT, 0);
            }
        }
        return;
    }

    if (ev->type == EV_SYN && ev->code == SYN_REPORT)
        emit3(EV_SYN, SYN_REPORT, 0);
}

/* ------------------------------------------------------------ pad detection */
static int is_gamepad(int fd)
{
    unsigned long bits[KEY_MAX / BITS_PER_UL + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    return bit_test(bits, BTN_GAMEPAD) || bit_test(bits, BTN_SOUTH) ||
           bit_test(bits, BTN_A) || bit_test(bits, BTN_TRIGGER);
}

static void route_axes(struct pad *p);

static void fill_axes(int fd, struct pad *p)
{
    int i;
    for (i = 0; i < ABS_CNT; i++) {
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(i), &ai) == 0) {
            p->ax[i].min = ai.minimum;
            p->ax[i].max = ai.maximum;
        } else {
            p->ax[i].min = 0;
            p->ax[i].max = 255;
        }
        /* Deterministic rest point. The kernel's ai.value is "last event
         * value": pads that connect during boot before their first report
         * read as 0 on every axis, which put 0..255 sticks at half scale
         * (left stick read 0.5/0.5) and broke every rest-relative heuristic.
         * Sticks and hats park mid-range, triggers park at min - both are
         * pure functions of (min, max), so capture nothing. */
        p->ax[i].neu = (p->ax[i].min + p->ax[i].max) / 2;
    }
    memset(p->axbits, 0, sizeof p->axbits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof p->axbits), p->axbits);
    route_axes(p);
    /* decided triggers rest at min (one-sided); ambiguous axes keep the
     * mid-range rest while undecided so the classifier can see movement */
    if (p->out_code[ABS_Z]  == ABS_Z && !p->amb_z)  p->ax[ABS_Z].neu  = p->ax[ABS_Z].min;
    if (p->out_code[ABS_RZ] == ABS_RZ && !p->amb_rz) p->ax[ABS_RZ].neu = p->ax[ABS_RZ].min;
}

/* --- axis routing -----------------------------------------------------------
 * HID gamepads without trigger usages report the RIGHT STICK on ABS_Z/ABS_RZ
 * (old DirectInput convention). Android's Generic.kl reads ABS_Z/ABS_RZ as
 * L2/R2, so unless the pad also has ABS_RX/ABS_RY, a center-resting Z/RZ is
 * rerouted to the right-stick slots. Otherwise Z/RZ stay triggers (one-sided,
 * scaled relative to their rest point). */
static void route_axes(struct pad *p)
{
    int i;
    int has_rstick = bit_test(p->axbits, ABS_RX) && bit_test(p->axbits, ABS_RY);

    for (i = 0; i < ABS_CNT; i++) {
        p->out_code[i] = -1;
        if (!bit_test(p->axbits, i)) continue;
        switch (i) {
        case ABS_X: case ABS_Y: case ABS_RX: case ABS_RY:
        case ABS_HAT0X: case ABS_HAT0Y:
        case ABS_HAT1X: case ABS_HAT1Y:
        case ABS_HAT2X: case ABS_HAT2Y:
            p->out_code[i] = i;
            break;
        case ABS_Z:
            if (p->z_is_stick) { p->out_code[i] = ABS_RX; break; }
            if (!has_rstick) { p->out_code[i] = ABS_RX; break; }  /* DInput: Z is stick X */
            p->out_code[i] = ABS_Z;      /* assume trigger; classifier decides */
            p->amb_z = 1;
            break;
        case ABS_RZ:
            if (p->rz_is_stick) { p->out_code[i] = ABS_RY; break; }
            if (!has_rstick) { p->out_code[i] = ABS_RY; break; }  /* DInput: RZ is stick Y */
            p->out_code[i] = ABS_RZ;
            p->amb_rz = 1;
            break;
        default:
            break;   /* unrouted axes are dropped */
        }
    }
}

/* -------------------------------------------------- dynamic hide/unhide */
/* Hide = EVIOCGRAB the pad: Android stops seeing it, we keep mirroring.
 * Physical pad remains visible to Android but produces no input. */
static void hide_pad(struct pad *p)
{
    int one = 1;
    if (p->hidden) return;
    /* exclusive grab: InputReader gets nothing, we keep reading + mirroring */
    if (ioctl(p->fd, EVIOCGRAB, &one) == 0) {
        p->hidden = 1;
        logmsg("grabbed %s (%s) - hidden from Android, mirrored via Fusion", p->name, p->node);
    } else {
        logmsg("WARNING: could not grab %s: %s", p->name, strerror(errno));
    }
}

static void unhide_pad(struct pad *p)
{
    char path[256];
    int fd;
    if (!p->hidden) return;
    /* closing the fd always releases the grab (ioctl ungrab is unreliable) */
    close(p->fd);
    snprintf(path, sizeof path, "/dev/input/%s", p->node);
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        logmsg("WARNING: could not reopen %s after release: %s", p->node, strerror(errno));
        p->fd = -1;   /* poll ignores negative fds */
        p->hidden = 0;
        return;
    }
    p->fd = fd;
    p->hidden = 0;
    fill_axes(fd, p);
    logmsg("released %s (%s) - visible to Android again", p->name, p->node);
}

static void apply_hide_mode(int enable)
{
    int i;
    hide_mode = enable;
    for (i = 0; i < npads; i++) {
        if (enable) hide_pad(&pads[i]);
        else unhide_pad(&pads[i]);
    }
    logmsg("hide_mode -> %d (%d pads)", hide_mode, npads);
}

/* --------------------------------------------------------- pad add/remove */
static void add_pad(const char *path, const char *node)
{
    struct pad *p = &pads[npads];
    char name[128] = "";
    char phys[128] = "";
    unsigned char ffb[(FF_MAX / 8) + 1] = {0};
    int fd;

    if (npads >= MAX_PADS) return;
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;

    if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0) name[0] = 0;
    name[sizeof name - 1] = 0;
    ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
    phys[sizeof phys - 1] = 0;

    if (strcmp(name, FUSION_NAME) == 0 ||
        (phys[0] && strcmp(phys, FUSION_PHYS) == 0)) {
        close(fd);
        return;
    }
    if (!is_gamepad(fd)) { close(fd); return; }

    memset(p, 0, sizeof *p);
    p->fd = fd;
    snprintf(p->node, sizeof p->node, "%s", node);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->layout = LAYOUT_DEFAULT;
    fill_axes(fd, p);

    p->ff_supported = 0;
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof ffb), ffb) >= 0) {
        if (ffb[FF_RUMBLE / 8] & (1 << (FF_RUMBLE % 8))) p->ff_supported = 1;
    }

    if (hide_mode) hide_pad(p);
    logmsg("added pad %d: \"%s\" (%s)%s%s", npads, p->name, p->node,
         p->ff_supported ? " [rumble]" : "",
         p->hidden ? " [hidden]" : "");
    npads++;
}

static void remove_pad(int idx)
{
    struct pad *p = &pads[idx];
    int i;
    logmsg("removing pad %d: \"%s\" (%s)", idx, p->name, p->node);
    release_pad_keys(idx);
    close(p->fd);
    for (i = 0; i < FF_SLOTS; i++) {
        if (ff_pad_idx[i] == idx) { ff_pad_idx[i] = -1; ff_pad_id[i] = -1; }
        else if (ff_pad_idx[i] > idx) ff_pad_idx[i]--;
    }
    memmove(&pads[idx], &pads[idx + 1], (size_t)(npads - idx - 1) * sizeof(struct pad));
    npads--;
    memset(&pads[npads], 0, sizeof(struct pad));
}

static void scan_devices(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        char path[256], node[128];
        int known = 0, i;
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        snprintf(node, sizeof node, "%.24s", e->d_name);
        snprintf(path, sizeof path, "/dev/input/%s", node);
        for (i = 0; i < npads; i++)
            if (strcmp(pads[i].node, node) == 0) { known = 1; break; }
        if (!known) add_pad(path, node);
    }
    closedir(d);
}

static void rescan_devices(void)
{
    int i;
    logmsg("rescanning all devices (current pads: %d)", npads);
    for (i = 0; i < npads; i++) {
        release_pad_keys(i);
        if (pads[i].fd >= 0) close(pads[i].fd);
    }
    for (i = 0; i < FF_SLOTS; i++) {
        ff_pad_idx[i] = -1;
        ff_pad_id[i] = -1;
    }
    npads = 0;
    memset(pads, 0, sizeof pads);
    memset(pad_pfd_idx, 0, sizeof pad_pfd_idx);

    if (ufd >= 0) {
        for (i = 0; i < ABS_CNT; i++) {
            if (fusion_has_axis(i)) emit3(EV_ABS, i, 0);
        }
        emit3(EV_SYN, SYN_REPORT, 0);
    }
    memset(last_axis, 0, sizeof last_axis);
    memset(last_union, 0, sizeof last_union);

    scan_devices();
    logmsg("rescan complete: %d pad(s) active", npads);
}

/* --------------------------------------------------------- fusion creation */
static int fusion_create(void)
{
    struct uinput_user_dev uidev;
    unsigned int i;

    ufd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) {
        logmsg("FATAL: cannot open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    /* event types */
    ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    if (rumble_on) ioctl(ufd, UI_SET_EVBIT, EV_FF);

    /* buttons: canonical gamepad set */
    for (i = BTN_SOUTH; i <= BTN_THUMBR; i++) ioctl(ufd, UI_SET_KEYBIT, i);
    ioctl(ufd, UI_SET_KEYBIT, BTN_C);
    /* dpad as buttons */
    for (i = BTN_DPAD_UP; i <= BTN_DPAD_RIGHT; i++) ioctl(ufd, UI_SET_KEYBIT, i);
    /* old DInput family (translated) */
    for (i = BTN_TRIGGER; i <= BTN_BASE6; i++) ioctl(ufd, UI_SET_KEYBIT, i);
    /* HID trigger-happy family (forwarded as-is) */
    for (i = BTN_TRIGGER_HAPPY1; i <= BTN_TRIGGER_HAPPY24; i++)
        ioctl(ufd, UI_SET_KEYBIT, i);
    /* system keys forwarded through the pad (back / home / menu / volume) */
    ioctl(ufd, UI_SET_KEYBIT, KEY_BACK);
    ioctl(ufd, UI_SET_KEYBIT, KEY_HOMEPAGE);
    ioctl(ufd, UI_SET_KEYBIT, KEY_HOME);
    ioctl(ufd, UI_SET_KEYBIT, KEY_MENU);
    ioctl(ufd, UI_SET_KEYBIT, KEY_VOLUMEUP);
    ioctl(ufd, UI_SET_KEYBIT, KEY_VOLUMEDOWN);
    ioctl(ufd, UI_SET_KEYBIT, KEY_MUTE);

    /* axes */
    for (i = 0; i < sizeof(fusion_axes) / sizeof(fusion_axes[0]); i++)
        ioctl(ufd, UI_SET_ABSBIT, fusion_axes[i]);

    /* rumble */
    if (rumble_on) ioctl(ufd, UI_SET_FFBIT, FF_RUMBLE);

    ioctl(ufd, UI_SET_PHYS, FUSION_PHYS);

    memset(&uidev, 0, sizeof uidev);
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "%s", FUSION_NAME);
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x045e;
    uidev.id.product = 0x028e;
    uidev.id.version = 0x0110;
    uidev.ff_effects_max = rumble_on ? FF_SLOTS : 0;

    for (i = 0; i < sizeof(fusion_axes) / sizeof(fusion_axes[0]); i++) {
        int mn, mx;
        fusion_axis_range(fusion_axes[i], &mn, &mx);
        uidev.absmin[fusion_axes[i]] = mn;
        uidev.absmax[fusion_axes[i]] = mx;
    }

    if (write(ufd, &uidev, sizeof uidev) < 0) {
        logmsg("FATAL: uinput setup write failed: %s", strerror(errno));
        return -1;
    }

    if (ioctl(ufd, UI_DEV_CREATE) < 0) {
        logmsg("FATAL: UI_DEV_CREATE failed: %s", strerror(errno));
        return -1;
    }

    /* neutral initial state */
    for (i = 0; i < sizeof(fusion_axes) / sizeof(fusion_axes[0]); i++)
        emit3(EV_ABS, fusion_axes[i], 0);
    emit3(EV_SYN, SYN_REPORT, 0);

    logmsg("fusion device created: \"%s\" (VID/PID 045e:028e)", FUSION_NAME);
    return 0;
}

/* ------------------------------------------------------- control socket */
static int sock_init(void)
{
    struct sockaddr_un addr;
    int i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1; client_since[i] = 0;
        client_uid[i] = -1; client_pid[i] = -1;
    }

    sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0) {
        logmsg("socket() failed: %s", strerror(errno));
        return -1;
    }

    unlink(SOCK_PATH);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        logmsg("bind(%s) failed: %s", SOCK_PATH, strerror(errno));
        close(sockfd);
        sockfd = -1;
        return -1;
    }

    if (listen(sockfd, 4) < 0) {
        logmsg("listen() failed: %s", strerror(errno));
        close(sockfd);
        sockfd = -1;
        return -1;
    }

    chmod(SOCK_PATH, 0666);
    logmsg("control socket listening at %s", SOCK_PATH);
    return 0;
}

static void sock_accept(void)
{
    int cfd, i;
    struct sockaddr_un peer;
    socklen_t len = sizeof peer;

    cfd = accept(sockfd, (struct sockaddr *)&peer, &len);
    if (cfd < 0) return;

    fcntl(cfd, F_SETFL, O_NONBLOCK);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] < 0) {
            struct ucred cr;
            socklen_t crl = sizeof cr;
            client_fds[i] = cfd;
            client_since[i] = time(NULL);
            client_uid[i] = -1;
            client_pid[i] = -1;
            if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cr, &crl) == 0) {
                client_uid[i] = (int)cr.uid;
                client_pid[i] = (int)cr.pid;
            }
            logmsg("client %d connected (uid=%d pid=%d)", i, client_uid[i], client_pid[i]);
            return;
        }
    }
    close(cfd);
}

static void sock_handle_client(int slot)
{
    char buf[512], *line, *saveptr;
    ssize_t r;
    int cfd = client_fds[slot];

    r = read(cfd, buf, sizeof buf - 1);
    if (r <= 0) {
        close(cfd);
        client_fds[slot] = -1;
        client_uid[slot] = -1;
        client_pid[slot] = -1;
        if (debug) logmsg("client %d disconnected", slot);
        return;
    }
    buf[r] = 0;

    for (line = strtok_r(buf, "\r\n", &saveptr); line; line = strtok_r(NULL, "\r\n", &saveptr)) {
        char cmd[64], arg1[64], arg2[64];
        int n = sscanf(line, "%63s %63s %63s", cmd, arg1, arg2);

        if (n == 2 && strcmp(cmd, "HIDE") == 0) {
            logmsg("HIDE %s from uid=%d pid=%d", arg1, client_uid[slot], client_pid[slot]);
            if (strcmp(arg1, "ON") == 0) {
                apply_hide_mode(1);
                dprintf(cfd, "OK HIDE ON\n");
            } else if (strcmp(arg1, "OFF") == 0) {
                apply_hide_mode(0);
                dprintf(cfd, "OK HIDE OFF\n");
            } else {
                dprintf(cfd, "ERROR unknown HIDE mode\n");
            }
        }
        else if (n == 3 && strcmp(cmd, "LAYOUT") == 0) {
            int idx = atoi(arg1);
            enum layout_mode mode = LAYOUT_DEFAULT;
            if (strcmp(arg2, "NINTENDO") == 0) mode = LAYOUT_NINTENDO;
            else if (strcmp(arg2, "XBOX") == 0) mode = LAYOUT_XBOX;
            else if (strcmp(arg2, "DEFAULT") != 0) {
                dprintf(cfd, "ERROR unknown layout mode\n");
                continue;
            }
            if (idx >= 0 && idx < npads) {
                pads[idx].layout = mode;
                logmsg("pad %d layout -> %s (uid=%d pid=%d)", idx, arg2,
                       client_uid[slot], client_pid[slot]);
                dprintf(cfd, "OK LAYOUT %d %s\n", idx, arg2);
            } else {
                dprintf(cfd, "ERROR pad index out of range\n");
            }
        }
        else if (n == 1 && strcmp(cmd, "STATUS") == 0) {
            int i;
            dprintf(cfd, "{\"hide_mode\":%d,\"npads\":%d,\"evtx\":%llu,\"efail\":%llu,\"pads\":[",
                    hide_mode, npads, evtx_count, evtx_fail);
            for (i = 0; i < npads; i++) {
                const char *lstr = "DEFAULT";
                if (pads[i].layout == LAYOUT_NINTENDO) lstr = "NINTENDO";
                else if (pads[i].layout == LAYOUT_XBOX) lstr = "XBOX";
                char routes[80]; int r, rn = 0;
            routes[0] = 0;
            for (r = 0; r < ABS_CNT; r++)
                if (pads[i].out_code[r] >= 0 && pads[i].out_code[r] != r)
                    rn += snprintf(routes + rn, sizeof routes - rn, "%s%d>%d",
                                   rn ? "," : "", r, pads[i].out_code[r]);
            dprintf(cfd, "%s{\"idx\":%d,\"name\":\"%s\",\"node\":\"%s\",\"hidden\":%d,\"layout\":\"%s\",\"routes\":\"%s\"}",
                        i ? "," : "", i, pads[i].name, pads[i].node, pads[i].hidden, lstr, routes);
            }
            dprintf(cfd, "]}\n");
        }
        else if (n == 1 && strcmp(cmd, "PING") == 0) {
            emit3(EV_KEY, BTN_SOUTH, 1);
            emit3(EV_SYN, SYN_REPORT, 0);
            usleep(300000);
            emit3(EV_KEY, BTN_SOUTH, 0);
            emit3(EV_SYN, SYN_REPORT, 0);
            dprintf(cfd, "OK PING - synthetic A press sent to Fusion\n");
        }
        else if (n == 1 && strcmp(cmd, "RESCAN") == 0) {
            logmsg("RESCAN command received (uid=%d pid=%d)",
                   client_uid[slot], client_pid[slot]);
            rescan_devices();
            dprintf(cfd, "OK RESCAN %d pads\n", npads);
        }
        else if (n == 1 && strcmp(cmd, "RESTART") == 0) {
            logmsg("RESTART command received (uid=%d pid=%d)",
                   client_uid[slot], client_pid[slot]);
            g_restart = 1;
            g_quit = 1;
            dprintf(cfd, "OK RESTART\n");
        }
        else if (n == 1 && strcmp(cmd, "QUIT") == 0) {
            logmsg("QUIT command received (uid=%d pid=%d)",
                   client_uid[slot], client_pid[slot]);
            g_quit = 1;
            dprintf(cfd, "OK QUIT\n");
        }
        else {
            dprintf(cfd, "ERROR unknown command\n");
        }
    }

    /* one request/response per connection: close so clients like nc see EOF */
    close(cfd);
    client_fds[slot] = -1;
}

/* --------------------------------------------------------------- main loop */
static void log_self(void)
{
    char ctx[160] = "?";
    int fd = open("/proc/self/attr/current", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, ctx, sizeof ctx - 1);
        if (r > 0) {
            ctx[r] = 0;
            for (char *p = ctx; *p; p++) if (*p == '\n') { *p = 0; break; }
        }
        close(fd);
    }
    logmsg("running as uid=%d, context=%s", (int)getuid(), ctx);
}

static void on_term(int sig) { (void)sig; g_quit = 1; }
static void on_hup(int sig)  { (void)sig; g_rescan = 1; }

static int main_loop(void)
{
    int inot = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    char buf[8192];
    int i;

    if (inot >= 0)
        inotify_add_watch(inot, "/dev/input",
                          IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);

    scan_devices();

    while (!g_quit) {
        struct pollfd pfds[MAX_PADS + MAX_CLIENTS + 3];
        int n = 0, rc;

        if (g_rescan) {
            g_rescan = 0;
            rescan_devices();
        }

        pfds[n].fd = ufd;   pfds[n].events = POLLIN; pfds[n].revents = 0; n++;
        if (inot >= 0) { pfds[n].fd = inot; pfds[n].events = POLLIN; pfds[n].revents = 0; n++; }
        if (sockfd >= 0) { pfds[n].fd = sockfd; pfds[n].events = POLLIN; pfds[n].revents = 0; n++; }

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                pfds[n].fd = client_fds[i]; pfds[n].events = POLLIN; pfds[n].revents = 0; n++;
            }
        }

        for (i = 0; i < npads; i++) {
            pfds[n].fd = pads[i].fd; pfds[n].events = POLLIN; pfds[n].revents = 0;
            pad_pfd_idx[i] = n;
            n++;
        }

        rc = poll(pfds, (nfds_t)n, 3000);
        if (g_quit) break;
        if (rc < 0) {
            if (errno == EINTR) {
                if (g_rescan) {
                    g_rescan = 0;
                    rescan_devices();
                }
                continue;
            }
            logmsg("poll: %s", strerror(errno));
            break;
        }
        if (g_rescan) {
            g_rescan = 0;
            rescan_devices();
        }
        classify_ambiguous();

        {
            /* wall-clock heartbeat + stale-client sweep every ~30s,
             * regardless of how busy the event loop is */
            static time_t last_beat;
            time_t now = time(NULL);
            if (now - last_beat >= 30) {
                int ncl = 0;
                last_beat = now;
                for (i = 0; i < MAX_CLIENTS; i++) if (client_fds[i] >= 0) ncl++;
                logmsg("loop: pads=%d clients=%d evtx=%llu", npads, ncl, evtx_count);
                for (i = 0; i < MAX_CLIENTS; i++)
                    if (client_fds[i] >= 0 && now - client_since[i] > 30) {
                        logmsg("client slot %d stale (uid=%d pid=%d) - closing",
                               i, client_uid[i], client_pid[i]);
                        close(client_fds[i]);
                        client_fds[i] = -1;
                    }
            }
        }

        /* fusion fd: force feedback */
        if (ufd >= 0 && (pfds[0].revents & (POLLIN | POLLERR)))
            handle_ff();

        /* control socket: accept new clients */
        if (sockfd >= 0) {
            int sock_slot = (inot >= 0 ? 2 : 1);
            if (pfds[sock_slot].revents & POLLIN)
                sock_accept();
        }

        /* client sockets */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                int slot_offset = (inot >= 0 ? 3 : 2) + (sockfd >= 0 ? 1 : 0);
                /* find the pollfd for this client (simple linear search) */
                int pi;
                for (pi = slot_offset; pi < n; pi++) {
                    if (pfds[pi].fd == client_fds[i] && (pfds[pi].revents & (POLLIN | POLLERR | POLLHUP))) {
                        sock_handle_client(i);
                        break;
                    }
                }
            }
        }

        /* pads - use the index snapshotted at build time (immune to
         * client connect/close churn inside this same poll iteration) */
        for (i = npads - 1; i >= 0; i--) {
            int pad_poll_idx = pad_pfd_idx[i];

            if (pad_poll_idx < 0 || pad_poll_idx >= n) continue;
            short rev = pfds[pad_poll_idx].revents;
            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                logmsg("pad %d (%s) poll flags 0x%hx - removing", i, pads[i].node, rev);
                remove_pad(i);
                break;  /* indexes shifted; next poll rebuilds */
            }
            if (rev & POLLIN) {
                struct input_event evs[32];
                ssize_t r;
                while ((r = read(pads[i].fd, evs, sizeof evs)) > 0) {
                    ssize_t off;
                    for (off = 0; off + (ssize_t)sizeof(struct input_event) <= r;
                         off += (ssize_t)sizeof(struct input_event))
                        process_event(&pads[i], i,
                                      (struct input_event *)((char *)evs + off));
                    if (r < (ssize_t)sizeof evs) break;
                }
            }
        }

        /* hotplug */
        if ((inot >= 0 && (pfds[1].revents & POLLIN)) || rc == 0) {
            if (inot >= 0)
                while (read(inot, buf, sizeof buf) > 0) ;
            scan_devices();
        }
    }
    return 0;
}

#ifndef UNIT_TEST
int main(int argc, char **argv)
{
    int i;
    struct sched_param sp;

    g_argv = argv;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            printf("fusiond %s\n", VERSION);
            return 0;
        }
        if (!strncmp(argv[i], "--name=", 7))       FUSION_NAME = argv[i] + 7;
        else if (!strcmp(argv[i], "--hide"))        hide_mode = 1;
        else if (!strcmp(argv[i], "--no-hide"))     hide_mode = 0;
        else if (!strcmp(argv[i], "--no-rumble"))   rumble_on = 0;
        else if (!strcmp(argv[i], "--debug"))       debug = 1;
        else if (!strncmp(argv[i], "--map=", 6))    MAP_PATH = argv[i] + 6;
        else {
            fprintf(stderr, "usage: fusiond [--name=NAME] [--hide] [--no-hide] [--no-rumble] "
                            "[--debug] [--map=FILE] [--version]\n");
            return 2;
        }
    }

    logmsg("fusiond v%s starting (name=\"%s\" hide=%d rumble=%d)",
         VERSION, FUSION_NAME, hide_mode, rumble_on);
    log_self();

    sp.sched_priority = 1;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        logmsg("note: SCHED_FIFO unavailable (%s)", strerror(errno));

    for (i = 0; i < FF_SLOTS; i++) { ff_pad_idx[i] = -1; ff_pad_id[i] = -1; }

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_hup);
    signal(SIGPIPE, SIG_IGN);

    load_user_map(MAP_PATH);

    if (fusion_create() < 0) return 1;
    if (sock_init() < 0) return 1;

    main_loop();

    if (ufd >= 0) { ioctl(ufd, UI_DEV_DESTROY); close(ufd); }
    if (sockfd >= 0) { close(sockfd); unlink(SOCK_PATH); }
    for (i = 0; i < npads; i++) close(pads[i].fd);
    for (i = 0; i < MAX_CLIENTS; i++) if (client_fds[i] >= 0) close(client_fds[i]);

    if (g_restart) {
        logmsg("fusiond restarting daemon (execv)...");
        execv("/proc/self/exe", g_argv);
        if (g_argv && g_argv[0]) execv(g_argv[0], g_argv);
        logmsg("FATAL: execv failed: %s", strerror(errno));
        return 1;
    }

    logmsg("fusiond exiting cleanly");
    return 0;
}
#endif
