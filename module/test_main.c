#define UNIT_TEST 1
#include "fusiond.c"

static void set_axis(struct pad *p, int code, int min, int max, int neu) {
    bit_set(p->axbits, code);
    p->ax[code].min = min; p->ax[code].max = max; p->ax[code].neu = neu;
}
int main(void) {
    int fails = 0;
    #define T(desc, got, want) do { if ((got) != (want)) { \
        printf("FAIL %-40s got=%d want=%d\n", desc, (got), (want)); fails++; } \
        else printf("ok   %s\n", desc); } while (0)

    /* pad1: DInput - X,Y sticks, Z/RZ CENTER-rest right stick, no RX/RY */
    struct pad p1; memset(&p1, 0, sizeof p1);
    set_axis(&p1, ABS_X, 0, 255, 128); set_axis(&p1, ABS_Y, 0, 255, 128);
    set_axis(&p1, ABS_Z, 0, 255, 128); set_axis(&p1, ABS_RZ, 0, 255, 128);
    route_axes(&p1);
    T("dinput Z -> RX",       p1.out_code[ABS_Z],  ABS_RX);
    T("dinput RZ -> RY",      p1.out_code[ABS_RZ], ABS_RY);
    T("dinput X stays X",     p1.out_code[ABS_X],  ABS_X);

    /* pad1b: signed DInput right stick (-128..127, rest 0) */
    struct pad p1b; memset(&p1b, 0, sizeof p1b);
    set_axis(&p1b, ABS_X, 0, 255, 128); set_axis(&p1b, ABS_Y, 0, 255, 128);
    set_axis(&p1b, ABS_Z, -128, 127, 0); set_axis(&p1b, ABS_RZ, -128, 127, 0);
    route_axes(&p1b);
    T("dinput-signed Z -> RX",  p1b.out_code[ABS_Z],  ABS_RX);
    T("dinput-signed RZ -> RY", p1b.out_code[ABS_RZ], ABS_RY);

    /* pad2: X360 - real right stick RX/RY + unipolar triggers Z/RZ */
    struct pad p2; memset(&p2, 0, sizeof p2);
    set_axis(&p2, ABS_X, -32768, 32767, 0); set_axis(&p2, ABS_Y, -32768, 32767, 0);
    set_axis(&p2, ABS_RX, -32768, 32767, 0); set_axis(&p2, ABS_RY, -32768, 32767, 0);
    set_axis(&p2, ABS_Z, 0, 255, 0); set_axis(&p2, ABS_RZ, 0, 255, 0);
    route_axes(&p2);
    T("x360 Z stays trigger",  p2.out_code[ABS_Z],  ABS_Z);
    T("x360 RZ stays trigger", p2.out_code[ABS_RZ], ABS_RZ);
    T("x360 RX stays RX",      p2.out_code[ABS_RX], ABS_RX);

    /* pad3: has RX/RY but Z center-rests (unusual) -> still a trigger */
    struct pad p3; memset(&p3, 0, sizeof p3);
    set_axis(&p3, ABS_X, -32768, 32767, 0); set_axis(&p3, ABS_Y, -32768, 32767, 0);
    set_axis(&p3, ABS_RX, -32768, 32767, 0); set_axis(&p3, ABS_RY, -32768, 32767, 0);
    set_axis(&p3, ABS_Z, -127, 127, 0); set_axis(&p3, ABS_RZ, -127, 127, 0);
    route_axes(&p3);
    T("c-r trigger w/ rstick -> Z",  p3.out_code[ABS_Z],  ABS_Z);
    T("c-r trigger w/ rstick -> RZ", p3.out_code[ABS_RZ], ABS_RZ);

    /* pad4: hats pass through */
    struct pad p4; memset(&p4, 0, sizeof p4);
    set_axis(&p4, ABS_HAT0X, -1, 1, 0); set_axis(&p4, ABS_HAT0Y, -1, 1, 0);
    route_axes(&p4);
    T("hat0x identity",  p4.out_code[ABS_HAT0X], ABS_HAT0X);
    T("unknown axis dropped", p4.out_code[ABS_THROTTLE], -1);

    /* ---- dynamic disambiguation: 8BitDo phantom-RX (DInput/BT) ---- */
    struct pad e1; memset(&e1, 0, sizeof e1);
    set_axis(&e1, ABS_X, 0, 255, 128); set_axis(&e1, ABS_Y, 0, 255, 128);
    set_axis(&e1, ABS_Z, 0, 255, 128); set_axis(&e1, ABS_RZ, 0, 255, 128);
    set_axis(&e1, ABS_RX, -32768, 32767, 0); set_axis(&e1, ABS_RY, -32768, 32767, 0);
    route_axes(&e1);
    T("phantom-rx: Z starts undecided trigger", e1.out_code[ABS_Z], ABS_Z);
    T("phantom-rx: ambiguity armed", e1.amb_z, 1);
    { struct input_event ev; memset(&ev, 0, sizeof ev); ev.type = EV_ABS; ev.code = ABS_Z; ev.value = 150;
      disambiguate(&e1, &ev);
      T("phantom-rx: Z event marked", e1.z_evt_seen, 1); }
    classify_axis(&e1, ABS_Z, 128);          /* parks mid-range */
    T("phantom-rx: parks mid -> rerouted RX", e1.out_code[ABS_Z], ABS_RX);
    T("phantom-rx: phantom RX route dropped", e1.out_code[ABS_RX], -1);
    T("phantom-rx: survives re-route (unhide)", (route_axes(&e1), e1.out_code[ABS_Z]), ABS_RX);

    /* real trigger parks at min */
    struct pad e4; memset(&e4, 0, sizeof e4);
    set_axis(&e4, ABS_Z, 0, 255, 128); set_axis(&e4, ABS_RX, -32768, 32767, 0); set_axis(&e4, ABS_RY, -32768, 32767, 0);
    route_axes(&e4);
    classify_axis(&e4, ABS_Z, 0);
    T("trigger: parks at min -> stays trigger", e4.out_code[ABS_Z], ABS_Z);
    T("trigger: ambiguity cleared", e4.amb_z, 0);
    T("trigger: rest moved to min", e4.ax[ABS_Z].neu, 0);

    /* rx veto: RX actively emitting + parks mid -> keep trigger */
    struct pad e5; memset(&e5, 0, sizeof e5);
    set_axis(&e5, ABS_Z, 0, 255, 128); set_axis(&e5, ABS_RX, -32768, 32767, 0); set_axis(&e5, ABS_RY, -32768, 32767, 0);
    route_axes(&e5);
    { struct input_event ev; memset(&ev, 0, sizeof ev); ev.type = EV_ABS; ev.code = ABS_RX; ev.value = 1000;
      disambiguate(&e5, &ev); }
    classify_axis(&e5, ABS_Z, 128);
    T("rx-active veto: Z stays trigger", e5.out_code[ABS_Z], ABS_Z);
    T("rx-active veto: rest at min", e5.ax[ABS_Z].neu, 0);

    /* undecided band: parks between min+span/8 and mid -> no decision yet */
    struct pad e6; memset(&e6, 0, sizeof e6);
    set_axis(&e6, ABS_Z, 0, 255, 128); set_axis(&e6, ABS_RX, -32768, 32767, 0); set_axis(&e6, ABS_RY, -32768, 32767, 0);
    route_axes(&e6);
    classify_axis(&e6, ABS_Z, 40);           /* between 31 and 95 */
    T("undecided band: no decision", e6.amb_z, 1);
    T("undecided band: stays trigger", e6.out_code[ABS_Z], ABS_Z);

    /* ---- system key passthrough (RP5 back button etc.) ---- */
    T("KEY_BACK whitelisted",     is_forwarded_key(KEY_BACK), 1);
    T("KEY_HOMEPAGE whitelisted", is_forwarded_key(KEY_HOMEPAGE), 1);
    T("KEY_MENU whitelisted",     is_forwarded_key(KEY_MENU), 1);
    T("random key not whitelisted", is_forwarded_key(KEY_A), 0);
    T("KEY_BACK maps identity",   map_code(KEY_BACK), KEY_BACK);
    T("KEY_HOMEPAGE maps identity", map_code(KEY_HOMEPAGE), KEY_HOMEPAGE);

    /* static DInput with boot-garbage neu: unconditional reroute */
    struct pad e7; memset(&e7, 0, sizeof e7);
    set_axis(&e7, ABS_X, 0, 255, 0); set_axis(&e7, ABS_Y, 0, 255, 0);
    set_axis(&e7, ABS_Z, 0, 255, 0); set_axis(&e7, ABS_RZ, 0, 255, 0);
    route_axes(&e7);
    T("dinput garbage-neu: Z -> RX", e7.out_code[ABS_Z], ABS_RX);
    T("dinput garbage-neu: RZ -> RY", e7.out_code[ABS_RZ], ABS_RY);

    printf(fails ? "== %d FAILURES ==\n" : "== all pass ==\n", fails);
    return fails != 0;
}
