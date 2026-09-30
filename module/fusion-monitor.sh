#!/system/bin/sh
# fusion-monitor: watches the foreground app and applies per-app Fusion rules.
#
# Config: /data/adb/fusion-apps.conf  (one rule per line)
#   <package>=hide      physical pads are grabbed while this app is focused
#
# v2.1.3: retry failed sends (socket may not exist yet at boot - the daemon
# races us), and re-assert the current state every 30s so a daemon restart
# (watchdog) can never leave pads unhidden/unhidden incorrectly.

LOG=/data/adb/fusion.log
CONF=/data/adb/fusion-apps.conf
SOCK=/data/adb/fusion.sock
INTERVAL=1

log() { echo "$(date) monitor: $1" >> "$LOG"; }

# every blocking call is bounded with timeout: dumpsys can hang during a busy
# boot, and nc can block if the daemon is wedged - either would freeze us.
send() { echo "$1" | timeout 3 nc -U "$SOCK" >/dev/null 2>&1; }

pkg_of() {
  timeout 2 dumpsys window 2>/dev/null | grep -m1 mCurrentFocus \
    | sed -n 's/.* u[0-9]* \([^ /}]*\).*/\1/p' | tr -d ' \r'
}

wants_hide() {
  [ -f "$CONF" ] || return 1
  awk -v p="$1" -F= '$1==p && $2 ~ /hide/ {f=1} END{exit f?0:1}' "$CONF"
}

log "started (config=$CONF) pid=$$"
trap 'log "monitor exited unexpectedly rc=$?"' EXIT

# wait up to 60s for the daemon's socket to appear
tries=0
while [ ! -S "$SOCK" ] && [ "$tries" -lt 60 ]; do
  sleep 1; tries=$((tries+1))
done
[ -S "$SOCK" ] || log "note: socket still missing after ${tries}s"

LAST_HIDE=""
LAST_PKG="__init__"
CYCLE=0
while :; do
  PKG=$(pkg_of)
  HIDE=no
  [ -n "$PKG" ] && wants_hide "$PKG" && HIDE=yes
  CMD="HIDE OFF"
  [ "$HIDE" = yes ] && CMD="HIDE ON"

  # log every focus change (deduped) - makes boot behavior visible in the log
  if [ "$PKG" != "$LAST_PKG" ]; then
    log "focus: ${PKG:-<none>} (hide=$HIDE)"
    LAST_PKG=$PKG
  fi

  if [ "$HIDE" != "$LAST_HIDE" ]; then
    # only mark the state as applied if the send actually succeeded
    if send "$CMD"; then
      LAST_HIDE=$HIDE
      log "focus=$PKG -> $CMD"
    else
      log "send failed (focus=$PKG) - retrying next cycle"
    fi
  elif [ $((CYCLE % 30)) = 0 ]; then
    # heartbeat proves the loop is turning + keepalive re-asserts state
    log "alive: focus=${PKG:-none} hide=$HIDE"
    send "$CMD" || log "keepalive send failed"
  fi
  CYCLE=$((CYCLE+1))
  sleep $INTERVAL
done
