SKIPUNZIP=0

ui_print " "
ui_print "- Fusion Controller v2.3.0"

if [ "$ARCH" != "arm64" ]; then
  ui_print "! Unsupported architecture: $ARCH"
  abort
fi

# ------------------------------------------------------------- locate stage
# $MODDIR is NOT guaranteed to be set in the customize.sh environment by all
# Magisk versions. Locate a dir containing the daemon: prefer $MODDIR, then
# Magisk's default staging dir, then extract from the zip ourselves.
STAGE=""
if [ -n "$MODDIR" ] && [ -f "$MODDIR/fusiond" ]; then
  STAGE="$MODDIR"
elif [ -f /data/adb/modules_update/fusion_controller/fusiond ]; then
  STAGE=/data/adb/modules_update/fusion_controller
elif [ -f "$ZIPFILE" ]; then
  STAGE=/data/adb/.fusion_stage
  rm -rf "$STAGE"; mkdir -p "$STAGE"
  unzip -o "$ZIPFILE" fusiond fusionctl.sh fusiond-run.sh \
        fusion-monitor.sh service.sh module.prop -d "$STAGE" >/dev/null 2>&1
fi

if [ ! -f "$STAGE/fusiond" ]; then
  ui_print "! ERROR: could not locate/extract fusiond from: $ZIPFILE"
  abort
fi
ui_print "- daemon staged at: $STAGE"
chmod 0755 "$STAGE/fusiond" "$STAGE/fusiond-run.sh" \
           "$STAGE/fusionctl.sh" "$STAGE/fusion-monitor.sh" 2>/dev/null

# -------------------------------------------------------------- module dir
# Where service.sh will live after reboot. Normal Magisk flow has already
# extracted everything to the staging dir. If it is missing, build it.
MDIR="$STAGE"
if [ -n "$MODDIR" ]; then MDIR="$MODDIR"; fi
if [ ! -f "$MDIR/module.prop" ]; then
  MDIR=/data/adb/modules_update/fusion_controller
  mkdir -p "$MDIR"
  unzip -o "$ZIPFILE" -d "$MDIR" -x "META-INF/*" >/dev/null 2>&1
fi
# make sure the module dir has every runtime file (also covers partial extracts)
for f in fusiond fusiond-run.sh fusionctl.sh fusion-monitor.sh service.sh; do
  [ -f "$MDIR/$f" ] || cp -f "$STAGE/$f" "$MDIR/$f" 2>/dev/null
done
chmod 0755 "$MDIR/fusiond" "$MDIR/fusiond-run.sh" "$MDIR/service.sh" \
           "$MDIR/fusionctl.sh" "$MDIR/fusion-monitor.sh" 2>/dev/null
ui_print "- module dir: $MDIR"

# ----------------------------------------------------- copies to /data/adb
cp -f "$STAGE/fusiond" /data/adb/fusiond
chmod 0755 /data/adb/fusiond
cp -f "$STAGE/fusiond-run.sh" /data/adb/fusiond-run.sh
chmod 0755 /data/adb/fusiond-run.sh
cp -f "$STAGE/fusionctl.sh" /data/adb/fusionctl
chmod 0755 /data/adb/fusionctl
cp -f "$STAGE/fusion-monitor.sh" /data/adb/fusion-monitor.sh
chmod 0755 /data/adb/fusion-monitor.sh

if [ ! -f /data/adb/fusion-apps.conf ]; then
  printf '%s\n' \
    '# Fusion Controller - per-app rules (one per line)' \
    '#   default: hide mode ON - physical pads grabbed (Fusion pad only)' \
    '#   <package>=nohide   pads visible/released while this app is focused' \
    '#' \
    '# Old-style "<package>=hide" lines do nothing now (hide is the default).' \
    '# Find package names:  pm list packages | grep -iE "retro|eden|yuzu"' \
    '#' \
    '# Examples (uncomment and edit):' \
    '# com.android.launcher3=nohide' \
    '# com.android.systemui=nohide' \
    > /data/adb/fusion-apps.conf
fi

echo "$(date) customize.sh: installed v2.3.0" >> /data/adb/fusion.log

ui_print "- Testing binary..."
RC=0
"$STAGE/fusiond" --version >/dev/null 2>&1 || RC=$?
[ "$RC" = 0 ] && ui_print "  daemon: OK" || ui_print "! daemon: FAILED (rc=$RC)"

[ -e /dev/uinput ] && ui_print "- /dev/uinput: present" || ui_print "! /dev/uinput: MISSING"

ui_print " "
ui_print "After reboot:  /data/adb/fusionctl status"
ui_print " "
