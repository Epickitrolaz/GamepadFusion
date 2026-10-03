#!/system/bin/sh
# Fusion Controller - boot service
MODDIR=${0%/*}
LOG=/data/adb/fusion.log

echo "$(date) service.sh: loaded (moddir=$MODDIR)" >> "$LOG"

# wait until boot has finished
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 2
done
sleep 3

# exec bits are unreliable on some ROMs' module dirs - force them
chmod 0755 "$MODDIR" "$MODDIR/fusiond" "$MODDIR/fusiond-run.sh" 2>/dev/null
chmod 0755 /data/adb/fusiond /data/adb/fusionctl /data/adb/fusiond-run.sh 2>/dev/null

echo "$(date) service.sh: boot complete, spawning runner" >> "$LOG"

# run via sh so missing exec bits can never break the chain
# the monitor is supervised: if it ever dies it is respawned 2s later.
# touch /data/adb/fusion.monitor.disable + pkill fusion-monitor to stop it.
if command -v setsid >/dev/null 2>&1; then
  setsid sh "$MODDIR/fusiond-run.sh" >/dev/null 2>&1 &
  setsid sh -c "while :; do [ -f /data/adb/fusion.monitor.disable ] || sh '$MODDIR/fusion-monitor.sh'; sleep 2; done" >/dev/null 2>&1 &
else
  ( sh "$MODDIR/fusiond-run.sh" >/dev/null 2>&1 ) &
  ( while :; do [ -f /data/adb/fusion.monitor.disable ] || sh "$MODDIR/fusion-monitor.sh"; sleep 2; done ) &
fi
echo "$(date) service.sh: runner + monitor supervisor spawned" >> "$LOG"
