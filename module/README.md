# Fusion Controller v2.0.0

**Merge every connected gamepad into ONE virtual controller on rooted Android.**

Built for devices like the Retroid Pocket 5, this Magisk module creates a "Fusion Controller" that mirrors inputs from all connected gamepads (built-in + Bluetooth/USB). Press A on any pad → the Fusion pad sends A.

## What's New in v2.0

### 🎮 Dynamic Hide Mode (Runtime Toggle)
- **v1:** Hide was boot-time only, one-way until reboot
- **v2:** Toggle hide on/off at runtime via socket control
- Use case: Enable hide for RetroArch (eliminates double-inputs), disable for other apps

### 🔄 Per-Pad Layout Remapping
- Swap A↔B and X↔Y for Nintendo-style controllers (8BitDo, Switch Pro, etc.)
- Each pad has independent layout: `NINTENDO | XBOX | DEFAULT`
- Applied in real-time, no reboot needed

### 🔌 Control Socket API
```sh
fusionctl hide on              # Hide physical pads (Fusion only)
fusionctl hide off             # Restore physical pads
fusionctl layout 0 NINTENDO    # Remap pad 0 to Nintendo layout
fusionctl status               # JSON: current state
```

## Installation

1. Flash `fusion_controller-v2.0.0.zip` in Magisk/KernelSU/APatch
2. Reboot
3. Verify: `su -c 'getevent -pl | grep -i fusion'`
4. Control: `su -c fusionctl status`

## Usage Examples

### Scenario 1: RetroArch (needs hide mode)
```sh
# RetroArch reads all pads → double-inputs
fusionctl hide on
# Now only Fusion Controller exists → bind RetroArch to it once
```

### Scenario 2: Multi-app with per-pad layout
```sh
fusionctl status              # See pad indices
# {"npads":2,"pads":[{"idx":0,"name":"8BitDo Ultimate 2C"},{"idx":1,"name":"Retroid Pocket Controller"}]}

fusionctl layout 0 NINTENDO   # Remap 8BitDo to Nintendo layout (B bottom, A right)
fusionctl layout 1 DEFAULT    # Retroid stays Xbox-style
```

### Scenario 3: Restore defaults
```sh
fusionctl hide off            # Physical pads visible again
fusionctl layout 0 DEFAULT    # Reset 8BitDo to standard Xbox mapping
```

## Configuration Files

### `/data/adb/fusion-apps.conf`
Per-app rules, checked every second by the foreground monitor:
```sh
# default: hide mode ON - physical pads grabbed (Fusion pad only)
# <package>=nohide   pads visible/released while this app is focused
com.android.launcher3=nohide
```
(Old-style `<package>=hide` lines are no-ops now - hide is the default.)

### `/data/adb/fusion.conf` (optional)
Boot-time defaults (overridden by runtime commands):
```sh
NAME=Fusion Controller
HIDE=1                    # 0 = start with pads released (--no-hide)
RUMBLE=1                  # 0 = disable rumble relay
DEBUG=1                   # verbose log
```

### `/data/adb/fusion.log`
Full boot + runtime trail. Check after issues:
```sh
su -c 'cat /data/adb/fusion.log'
```

## Socket Protocol (for app developers)

Connect to `/data/adb/fusion.sock` (Unix domain, SOCK_STREAM), send text commands:

| Command | Response | Effect |
|---------|----------|--------|
| `HIDE ON` | `OK HIDE ON` | Grab all physical pads via `EVIOCGRAB` |
| `HIDE OFF` | `OK HIDE OFF` | Release all physical pads |
| `LAYOUT <idx> <mode>` | `OK LAYOUT ...` | mode = `NINTENDO` \| `XBOX` \| `DEFAULT` |
| `STATUS` | `{...JSON...}` | Current state: hide_mode, pads array |
| `RESCAN` | `OK RESCAN <n> pads` | Rescan connected gamepads and reset axis disambiguation |
| `RESTART` | `OK RESTART` | Clean daemon restart and re-enumeration |
| `QUIT` | `OK QUIT` | Graceful daemon shutdown |

Example (shell):
```sh
echo "STATUS" | nc -U /data/adb/fusion.sock
```

Example (Python):
```python
import socket, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/data/adb/fusion.sock")
s.sendall(b"STATUS\n")
state = json.loads(s.recv(4096).decode())
print(f"Hide mode: {state['hide_mode']}, Pads: {state['npads']}")
```

## Troubleshooting

### "fusionctl: daemon socket not found"
```sh
ps -A | grep fusiond        # Should show one fusiond process
cat /data/adb/fusion.log    # Check for errors
```

### Double inputs in some apps
Pads are hidden by default since v2.2.0, so double inputs should be gone
everywhere. If an app needs the REAL pads (layout tools, key mappers), release
them while it is focused:

```sh
echo 'com.example.app=nohide' >> /data/adb/fusion-apps.conf
```

### Wrong button mapping (8BitDo, Nintendo controllers)
```sh
fusionctl status            # Find your pad's index
fusionctl layout 0 NINTENDO # Swap A↔B, X↔Y
```

### Wrong axis mapping / stick recognized as trigger
If an analog axis (such as a right stick on ABS_Z/ABS_RZ) gets mistakenly recognized as a trigger (L2/R2) due to resting position or connection quirks:
- Tap the **Fusion Restart** Quick Settings tile in the notification shade.
- Or run:
  ```sh
  su -c fusionctl restart
  ```
  This restarts the daemon and rescans all pads, resetting axis disambiguation so sticks are correctly mapped.
- Alternatively, run `su -c fusionctl rescan` to re-enumerate and reset disambiguation without restarting the daemon.

### Disable the module
```sh
su -c 'touch /data/adb/fusion.disable && pkill fusiond'
```

## Roadmap: Phase 2 & 3 (Next Updates)

- **Foreground monitor daemon** — auto-apply per-app settings from `/data/adb/fusion-apps.conf`:
  ```
  org.retroarch=HIDE_ON,LAYOUT_0_NINTENDO
  com.android.launcher3=HIDE_OFF
  ```
- **Quick Settings Tile + Settings APK** — GUI for live toggle + per-app config

## Technical Details

- Virtual pad: Xbox 360 USB (VID/PID 045e:028e) → Android applies built-in key layout
- Hide mechanism: `EVIOCGRAB` (reversible, unlike `EVIOCREVOKE`); ON by
  default since v2.2.0, per-app `=nohide` rules release pads while focused
- Every HIDE/LAYOUT/QUIT command is attributed in the log via
  `SO_PEERCRED` (uid + pid of the sending client)
- Socket: `/data/adb/fusion.sock` mode 0666 (any UID can control)
- Force feedback: Rumble routed to the most recently active physical pad
- SELinux: Permissive rules for `uhid_device`, `input_device` (see `sepolicy.rule`)

## License & Credits

Built on the JoyMerge architecture (uinput merging on Android). Fusion Controller extends it with dynamic control, per-pad remapping, and a protocol for external automation.

---

**Version:** 2.3.0  
**Tested on:** Retroid Pocket 5 (Android 13, Magisk 27.0)  
**Requirements:** Kernel with `CONFIG_INPUT_UINPUT=y`, root (Magisk/KernelSU/APatch)
