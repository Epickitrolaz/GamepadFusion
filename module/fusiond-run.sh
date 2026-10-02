#!/system/bin/sh
# Fusion Controller - watchdog/supervisor for fusiond.
# Restarts the daemon if it dies, stops respawning when
# /data/adb/fusion.disable exists. Logs its own trail to /data/adb/fusion.log.

MODDIR=${0%/*}
LOG=/data/adb/fusion.log
CONF=/data/adb/fusion.conf
DISABLE=/data/adb/fusion.disable

# prefer the copy at /data/adb/fusiond (made by customize.sh); fall back
# to the copy inside the module dir
DAEMON=/data/adb/fusiond
[ -x "$DAEMON" ] || DAEMON="$MODDIR/fusiond"

log() { echo "$(date) runner: $1" >> "$LOG"; }

log "starting (daemon=$DAEMON, moddir=$MODDIR)"
[ -f "$CONF" ] || log "note: no config file at $CONF (defaults in use)"
[ -f "$DISABLE" ] && log "note: $DISABLE exists - not starting"

# rotate log if > 1 MB
if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 1048576 ]; then
  : > "$LOG"
fi

# optional config file, KEY=VALUE lines:
#   NAME=, HIDE=, RUMBLE=0, DEBUG=1, EXTRA_ARGS=
#   HIDE=0 starts with pads released (--no-hide); default/1 = hide ON
ARGS=""
if [ -f "$CONF" ]; then
  . "$CONF"
  [ -n "$NAME" ]      && ARGS="$ARGS --name=$NAME"
  [ "$HIDE" = "1" ]   && ARGS="$ARGS --hide"
  [ "$HIDE" = "0" ]   && ARGS="$ARGS --no-hide"
  [ "$RUMBLE" = "0" ] && ARGS="$ARGS --no-rumble"
  [ "$DEBUG" = "1" ]  && ARGS="$ARGS --debug"
  ARGS="$ARGS $EXTRA_ARGS"
  log "config loaded, args:$ARGS"
fi

if [ ! -x "$DAEMON" ]; then
  log "ERROR: daemon not found or not executable: $DAEMON"
  exit 1
fi

FAILS=0
while true; do
  [ -f "$DISABLE" ] && exit 0

  START=$(date +%s)
  "$DAEMON" $ARGS >> "$LOG" 2>&1
  RC=$?
  RUNTIME=$(( $(date +%s) - START ))

  [ -f "$DISABLE" ] && exit 0

  if [ "$RUNTIME" -ge 60 ]; then
    FAILS=0            # it ran fine for a while; crash was likely a fluke
  else
    FAILS=$((FAILS + 1))
    if [ "$FAILS" -ge 10 ]; then
      log "ERROR: daemon keeps failing (last rc=$RC) - giving up"
      exit 1
    fi
  fi

  log "daemon exited (rc=$RC, runtime=${RUNTIME}s), restarting in 3s"
  sleep 3
done
