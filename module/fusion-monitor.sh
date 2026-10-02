#!/system/bin/sh
# fusion-monitor: watches the foreground app and applies per-app Fusion rules.
#
# Config: /data/adb/fusion-apps.conf  (one rule per line)
#   <package>=nohide   physical pads are RELEASED while this app is focused
#
# v2.2.0: INVERTED default - hide mode is ON unless an app asks for OFF.
#   No rule (or no config, or an unreadable focus) = pads stay grabbed, so
#   games never see double inputs. Only listed nohide apps get pads visible.
#   Old-style <package>=hide lines are harmless no-ops now (hide is default).
#   Keepalive no longer blind-sends: it queries the daemon's STATUS first and
#   only sends when the daemon's actual state disagrees with our belief. If
#   STATUS is unreadable (daemon restarting) it falls back to a blind assert
#   so a restarted daemon still ends up in the right state.
#   v2.1.3: retry failed sends (socket may not exist yet at boot - the daemon
#   races us), and re-assert the current state periodically.

LOG=/data/adb/fusion.log
CONF=/data/adb/fusion-apps.conf
SOCK=/data/adb/fusion.sock
INTERVAL=1

log() { echo "$(date) monitor: $1" >> "$LOG"; }

# every blocking call is bounded with timeout: dumpsys can hang during a busy
# boot, and nc can block if the daemon is wedged - either would freeze us.
send() { echo "$1" | timeout 3 nc -U "$SOCK" >/dev/null 2>&1; }

pkg_of() {
  timeout 5 dumpsys window 2>/dev/null | grep -m1 -E 'mCurrentFocus|mFocusedApp' \
    | sed -n 's/.* u[0-9]* \([^ }]*\).*/\1/p' | tr -d ' \r'
}

# is this focus window an app? App windows carry "pkg/pkg.Activity" titles;
# system surfaces (notification shade, QS, keyboard, lockscreen, recents) do
# not. ROMs differ - some report "com.android.systemui/...", others just
# "StatusBar" or "NotificationShade" - so match those names as a fallback.

wants_show() {
  [ -f "$CONF" ] || return 1
  awk -v p="$1" -F= '$1==p && tolower($2) ~ /nohide|show/ {f=1} END{exit f?0:1}' "$CONF"
}

# the daemon's actual hide state: prints 0 or 1, nothing if unreachable
daemon_hide() {
  echo STATUS | timeout 3 nc -U "$SOCK" 2>/dev/null \
    | grep -o '"hide_mode":[01]' | cut -d: -f2
}

log "started (config=$CONF pid=$$)"
trap 'log "monitor exited unexpectedly rc=$?"' EXIT

# wait up to 60s for the daemon's socket to appear
tries=0
while [ ! -S "$SOCK" ] && [ "$tries" -lt 60 ]; do
  sleep 1; tries=$((tries+1))
done
[ -S "$SOCK" ] || log "note: socket still missing after ${tries}s"

LAST_HIDE=""
LAST_PKG="__init__"
LAST_APP=""
CYCLE=0
while :; do
  WIN=$(pkg_of)
  [ "$WIN" = null ] && WIN=""
  # A shade/QS pull-down, keyboard or lockscreen is not an app switch: keep
  # the previous app's rule so hide mode stays exactly as it was. Same for an
  # empty focus (dumpsys hiccup) - state persists instead of flip-flopping.
  case "$WIN" in
    ""|*StatusBar*|*NotificationShade*|*NavigationBar*|*Recents*|*InputMethod*|com.android.systemui*)
      PKG="$LAST_APP" ;;
    *)
      PKG="${WIN%%/*}"
      LAST_APP="$PKG" ;;
  esac
  # default: hidden. Only a listed nohide app releases the pads. An unknown
  # focus (dumpsys slow/dead) also stays hidden - safer for games.
  HIDE=yes
  [ -n "$PKG" ] && wants_show "$PKG" && HIDE=no
  CMD="HIDE ON"
  [ "$HIDE" = no ] && CMD="HIDE OFF"

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
    # heartbeat proves the loop is turning; reconcile instead of blind-assert
    # so we stop stomping the daemon when our belief already matches reality
    log "alive: focus=${PKG:-none} hide=$HIDE"
    WANT=1; [ "$HIDE" = no ] && WANT=0
    GOT=$(daemon_hide)
    if [ -n "$GOT" ] && [ "$GOT" != "$WANT" ]; then
      send "$CMD" || log "keepalive send failed"
    elif [ -z "$GOT" ]; then
      # STATUS unreadable (daemon restarting?) - blind assert as fallback
      send "$CMD" || log "keepalive send failed (daemon unreachable)"
    fi
  fi
  CYCLE=$((CYCLE+1))
  sleep $INTERVAL
done
