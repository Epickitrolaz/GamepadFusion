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

    /* ---- ABS_BRAKE and ABS_GAS (modern Bluetooth & handheld triggers) ---- */
    struct pad pbg; memset(&pbg, 0, sizeof pbg);
    set_axis(&pbg, ABS_X, -32768, 32767, 0); set_axis(&pbg, ABS_Y, -32768, 32767, 0);
    set_axis(&pbg, ABS_RX, -32768, 32767, 0); set_axis(&pbg, ABS_RY, -32768, 32767, 0);
    set_axis(&pbg, ABS_BRAKE, 0, 255, 128); set_axis(&pbg, ABS_GAS, 0, 255, 128);
    route_axes(&pbg);
    T("brake -> Z", pbg.out_code[ABS_BRAKE], ABS_Z);
    T("gas -> RZ",  pbg.out_code[ABS_GAS],   ABS_RZ);
    T("brake neu reset to min", pbg.ax[ABS_BRAKE].neu, 0);
    T("gas neu reset to min",   pbg.ax[ABS_GAS].neu,   0);
    T("brake not ambiguous",    pbg.amb_z,  0);
    T("gas not ambiguous",      pbg.amb_rz, 0);
    T("pbg has analog lt",      pad_has_analog_lt(&pbg), 1);
    T("pbg has analog rt",      pad_has_analog_rt(&pbg), 1);

    /* pad with ABS_BRAKE and ABS_Z: ABS_BRAKE takes precedence as LT */
    struct pad pbz; memset(&pbz, 0, sizeof pbz);
    set_axis(&pbz, ABS_X, -32768, 32767, 0); set_axis(&pbz, ABS_Y, -32768, 32767, 0);
    set_axis(&pbz, ABS_RX, -32768, 32767, 0); set_axis(&pbz, ABS_RY, -32768, 32767, 0);
    set_axis(&pbz, ABS_Z, 0, 255, 128); set_axis(&pbz, ABS_BRAKE, 0, 255, 128);
    route_axes(&pbz);
    T("brake takes precedence over Z", pbz.out_code[ABS_BRAKE], ABS_Z);
    T("Z does not route to Z when brake present", (pbz.out_code[ABS_Z] != ABS_Z), 1);
    T("amb_z cleared by brake", pbz.amb_z, 0);

    /* virtual device declares standard Xbox 360 axes ABS_Z and ABS_RZ with range 0..255 */
    T("fusion has ABS_Z",  fusion_has_axis(ABS_Z),  1);
    T("fusion has ABS_RZ", fusion_has_axis(ABS_RZ), 1);
    T("fusion has no ABS_BRAKE", fusion_has_axis(ABS_BRAKE), 0);
    T("fusion has no ABS_GAS",   fusion_has_axis(ABS_GAS),   0);
    int f_mn = -1, f_mx = -1;
    fusion_axis_range(ABS_Z, &f_mn, &f_mx);
    T("fusion ABS_Z min", f_mn, 0);
    T("fusion ABS_Z max", f_mx, 255);
    f_mn = -1; f_mx = -1;
    fusion_axis_range(ABS_RZ, &f_mn, &f_mx);
    T("fusion ABS_RZ min", f_mn, 0);
    T("fusion ABS_RZ max", f_mx, 255);

    /* ---- digital trigger synthesis ---- */
    /* Digital pad (e.g. Switch Joy-Con / Pro controller with clicky triggers) */
    struct pad pdig; memset(&pdig, 0, sizeof pdig);
    set_axis(&pdig, ABS_X, -32768, 32767, 0); set_axis(&pdig, ABS_Y, -32768, 32767, 0);
    set_axis(&pdig, ABS_RX, -32768, 32767, 0); set_axis(&pdig, ABS_RY, -32768, 32767, 0);
    route_axes(&pdig);
    T("pdig has no analog lt", pad_has_analog_lt(&pdig), 0);
    T("pdig has no analog rt", pad_has_analog_rt(&pdig), 0);

    /* BTN_TL2 press synthesizes ABS_Z 255 */
    last_axis[ABS_Z] = 0;
    struct input_event ev_key;
    memset(&ev_key, 0, sizeof ev_key);
    ev_key.type = EV_KEY; ev_key.code = BTN_TL2; ev_key.value = 1;
    process_event(&pdig, 0, &ev_key);
    T("digital TL2 press -> ABS_Z 255",     last_axis[ABS_Z], 255);

    /* BTN_TL2 release resets ABS_Z to 0 */
    ev_key.value = 0;
    process_event(&pdig, 0, &ev_key);
    T("digital TL2 release -> ABS_Z 0",     last_axis[ABS_Z], 0);

    /* BTN_TR2 press synthesizes ABS_RZ 255 */
    last_axis[ABS_RZ] = 0;
    ev_key.code = BTN_TR2; ev_key.value = 1;
    process_event(&pdig, 0, &ev_key);
    T("digital TR2 press -> ABS_RZ 255",  last_axis[ABS_RZ], 255);

    /* BTN_TR2 release resets ABS_RZ to 0 */
    ev_key.value = 0;
    process_event(&pdig, 0, &ev_key);
    T("digital TR2 release -> ABS_RZ 0",  last_axis[ABS_RZ], 0);

    /* Analog pad (pbg): BTN_TL2 press does NOT synthesize 255 spike */
    last_axis[ABS_Z] = 0;
    ev_key.code = BTN_TL2; ev_key.value = 1;
    process_event(&pbg, 0, &ev_key);
    T("analog pad TL2 press no spike ABS_Z",     last_axis[ABS_Z], 0);
    ev_key.value = 0;
    process_event(&pbg, 0, &ev_key);

    /* Analog pad ABS_BRAKE motion event updates ABS_Z */
    struct input_event ev_abs;
    memset(&ev_abs, 0, sizeof ev_abs);
    ev_abs.type = EV_ABS; ev_abs.code = ABS_BRAKE; ev_abs.value = 128;
    process_event(&pbg, 0, &ev_abs);
    T("analog brake event -> ABS_Z scaled",     last_axis[ABS_Z], 128);

    /* Analog pad ABS_GAS motion event updates ABS_RZ */
    ev_abs.code = ABS_GAS; ev_abs.value = 200;
    process_event(&pbg, 0, &ev_abs);
    T("analog gas event -> ABS_RZ scaled",  last_axis[ABS_RZ], 200);

    /* rescan_devices: clears pads and resets state */
    npads = 1; pads[0].fd = -1;
    rescan_devices();
    T("rescan_devices: npads reset", npads, 0);

    /* ---- layout remapping tests ---- */
    struct pad pl; memset(&pl, 0, sizeof pl);
    pl.layout = LAYOUT_DEFAULT;
    T("layout default: A is A", remap_layout(&pl, BTN_SOUTH), BTN_SOUTH);
    T("layout default: B is B", remap_layout(&pl, BTN_EAST),  BTN_EAST);
    T("layout default: X is X", remap_layout(&pl, BTN_WEST),  BTN_WEST);
    T("layout default: Y is Y", remap_layout(&pl, BTN_NORTH), BTN_NORTH);

    pl.layout = LAYOUT_NINTENDO;
    T("layout nintendo: A -> B", remap_layout(&pl, BTN_SOUTH), BTN_EAST);
    T("layout nintendo: B -> A", remap_layout(&pl, BTN_EAST),  BTN_SOUTH);
    T("layout nintendo: X -> Y", remap_layout(&pl, BTN_WEST),  BTN_NORTH);
    T("layout nintendo: Y -> X", remap_layout(&pl, BTN_NORTH), BTN_WEST);

    pl.layout = LAYOUT_SWAP_AB;
    T("layout swap_ab: A -> B", remap_layout(&pl, BTN_SOUTH), BTN_EAST);
    T("layout swap_ab: B -> A", remap_layout(&pl, BTN_EAST),  BTN_SOUTH);
    T("layout swap_ab: X is X", remap_layout(&pl, BTN_WEST),  BTN_WEST);
    T("layout swap_ab: Y is Y", remap_layout(&pl, BTN_NORTH), BTN_NORTH);

    pl.layout = LAYOUT_SWAP_XY;
    T("layout swap_xy: A is A", remap_layout(&pl, BTN_SOUTH), BTN_SOUTH);
    T("layout swap_xy: B is B", remap_layout(&pl, BTN_EAST),  BTN_EAST);
    T("layout swap_xy: X -> Y", remap_layout(&pl, BTN_WEST),  BTN_NORTH);
    T("layout swap_xy: Y -> X", remap_layout(&pl, BTN_NORTH), BTN_WEST);

    /* custom button remapping */
    pl.layout = LAYOUT_CUSTOM;
    parse_custom_map(&pl, "304->310,305->311");
    T("custom map: A -> LB", remap_layout(&pl, BTN_SOUTH), BTN_TL);
    T("custom map: B -> RB", remap_layout(&pl, BTN_EAST),  BTN_TR);
    T("custom map: unmapped X is X", remap_layout(&pl, BTN_WEST), BTN_WEST);

    char custom_out[128];
    format_custom_map(&pl, custom_out, sizeof custom_out);
    T("format custom map non-empty", strlen(custom_out) > 0, 1);

    /* ---- persistence test: save & load layout across reboot ---- */
    LAYOUTS_PATH = "/tmp/fusion_test_layouts.conf";
    unlink(LAYOUTS_PATH);
    save_pad_layout("Xbox Wireless Controller", "NINTENDO");
    save_pad_layout("8BitDo Ultimate 2C", "CUSTOM:304->305,305->304");

    struct pad test_pad1; memset(&test_pad1, 0, sizeof test_pad1);
    snprintf(test_pad1.name, sizeof test_pad1.name, "Xbox Wireless Controller");
    load_pad_layout(&test_pad1);
    T("persistence: xbox loaded NINTENDO layout", test_pad1.layout, LAYOUT_NINTENDO);
    T("persistence: xbox remaps A -> B", remap_layout(&test_pad1, BTN_SOUTH), BTN_EAST);

    struct pad test_pad2; memset(&test_pad2, 0, sizeof test_pad2);
    snprintf(test_pad2.name, sizeof test_pad2.name, "8BitDo Ultimate 2C");
    load_pad_layout(&test_pad2);
    T("persistence: 8bitdo loaded CUSTOM layout", test_pad2.layout, LAYOUT_CUSTOM);
    T("persistence: 8bitdo custom map A -> B", remap_layout(&test_pad2, BTN_SOUTH), BTN_EAST);
    T("persistence: 8bitdo custom map B -> A", remap_layout(&test_pad2, BTN_EAST),  BTN_SOUTH);

    /* update pad1 layout and verify persistence update */
    save_pad_layout("Xbox Wireless Controller", "XBOX");
    load_pad_layout(&test_pad1);
    T("persistence: xbox updated to XBOX", test_pad1.layout, LAYOUT_XBOX);
    unlink(LAYOUTS_PATH);

    printf(fails ? "== %d FAILURES ==\n" : "== all pass ==\n", fails);
    return fails != 0;
}
