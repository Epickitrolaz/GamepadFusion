# GamepadFusion

A Magisk module that merges **all connected gamepads** on an Android handheld
into one virtual controller — the **Fusion Controller** — plus a small
companion app for per-app control.

Press "A" on *any* connected pad, and every app sees "A" pressed on the Fusion
Controller. The virtual pad always enumerates as a standard Xbox-layout
controller, so emulators, launchers and games only ever need one profile.

Built and tested on a **Retroid Pocket 5** (internal pad + 8BitDo Ultimate 2C).

> [!WARNING]
> **This project is experimental software.** It installs a system-level daemon
> that grabs raw input devices — expect bugs. You flash/install it entirely at
> your own risk. **I am not responsible for any damage, data loss, or
> bootloops.** Keep a bootloop protector (Magisk's built-in one) enabled, and
> know how to recover your device before installing.

## How it works

`fusiond` (a static C daemon started at boot by the module) opens every
`/dev/input/event*` node that looks like a gamepad, maintains a real-time union
of all their button/axis states, and replays that state into a single
`uinput` device named **Fusion Controller** (VID/PID `045e:028e`, Xbox One S
identity — the most widely recognized pad in the Android ecosystem).

## Features

- **Merge everything**: internal + USB + Bluetooth pads fused into one device
- **Standard Xbox identity** — every app sees the same controller, no per-app
  controller profiles or double inputs
- **Hide mode, ON by default** — physical pads are grabbed (EVIOCGRAB) at all
  times so apps that read every device directly only see the Fusion pad;
  apps that need the real pads get them released while focused via
  `<package>=nohide` rules (`fusion-monitor.sh` + `/data/adb/fusion-apps.conf`)
- **Per-pad layout remapping** — Nintendo-layout pads get their A/B, X/Y
  swapped to standard Xbox order in real time
- **System key passthrough** — Back / Home / Menu / Volume keys survive the
  merge (the RP5's back button works)
- **Rumble merge** — force feedback from any pad is replayed to all pads
- **Motion controls work** — gyro/accelerometer from the physical pads is
  passed through untouched, so motion aiming and gyro-based emulators keep
  working with the Fusion pad
- **Control socket** — live reconfiguration over a Unix socket, with the
  `fusionctl` CLI and the FusionControl app as frontends

## Install

1. Flash the module zip from the [latest release](https://github.com/Epickitrolaz/GamepadFusion/releases) in Magisk and reboot.
2. (Optional) Install `FusionControl.apk` from Releases — toggle hide mode,
   per-pad layouts, restart/rescan, and view connected pads from a normal app, no terminal.
   It also ships **Quick Settings tiles**:
   - **Fusion Hide**: toggle hide mode (grab physical pads / Fusion-only) straight from the shade.
   - **Fusion Restart**: restart the daemon and rescan pads with one tap to fix axes incorrectly recognized as triggers.

CLI quick reference (run as root):

```sh
fusionctl status                 # pads + fusion device state
fusionctl hide 1                 # hide physical pads (grab) — for RetroArch etc.
fusionctl layout 0 NINTENDO      # remap pad 0 to Nintendo layout
fusionctl restart                # restart daemon & rescan pads (fixes axis/trigger issues)
fusionctl rescan                 # rescan pads and reset disambiguation without restart
```

Details, config files and troubleshooting: [`module/README.md`](module/README.md)

## Building from source

**Module** — needs an aarch64 musl cross toolchain (musl.cc) and `zip`:

```sh
TC=/path/to/aarch64-linux-musl-cross ./module/build.sh
# -> module/fusion_controller-vX.Y.Z.zip
```

**App** — needs an Android SDK (platform 34, build-tools 34) and JDK 17+:

```sh
ANDROID_SDK=/path/to/sdk JAVA_HOME=/path/to/jdk ./app/build_apk.sh
# -> app/FusionControl.apk
```

## Repo layout

```
├── module/          Magisk module: fusiond.c daemon, installer, control CLI
│   └── fusiond.c    the daemon (single-file C, no dependencies, static-linked)
├── app/             FusionControl companion app (no-Gradle build: aapt2/javac/d8)
│   └── icon/        icon SVG sources (adaptive bg/fg layers; PNGs rendered with resvg)
├── module/README.md detailed usage, socket protocol, troubleshooting
└── LICENSE          MIT
```

## Credits

Inspired by [JoyMerge](https://github.com/quatrixone/JoyMerge) (Joy-Con merging
via uinput on Android). Related quirk thread on Retroid controller device IDs.

## License

MIT — see [LICENSE](LICENSE).
