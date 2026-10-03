#!/system/bin/sh
# fusionctl - control the Fusion Controller daemon via socket
SOCK=/data/adb/fusion.sock

case "$1" in
  restart)
    if [ -S "$SOCK" ]; then
      RESP=$(echo "RESTART" | timeout 3 nc -U "$SOCK" 2>/dev/null)
      if [ -n "$RESP" ]; then
        echo "$RESP"
        exit 0
      fi
    fi
    # If daemon socket didn't respond or wasn't there, restart process
    pkill -9 fusiond 2>/dev/null
    rm -f "$SOCK"
    RUNNER=/data/adb/fusiond-run.sh
    [ -x "$RUNNER" ] || RUNNER=/data/adb/modules/fusion_controller/fusiond-run.sh
    DAEMON=/data/adb/fusiond
    [ -x "$DAEMON" ] || DAEMON=/data/adb/modules/fusion_controller/fusiond
    if [ -x "$RUNNER" ]; then
      nohup sh "$RUNNER" >/dev/null 2>&1 &
    elif [ -x "$DAEMON" ]; then
      nohup "$DAEMON" >/dev/null 2>&1 &
    else
      echo "ERROR: cannot find fusiond executable or runner"
      exit 1
    fi
    sleep 1
    if [ -S "$SOCK" ]; then
      echo "OK RESTART"
    else
      echo "OK RESTART (daemon spawned)"
    fi
    ;;
  *)
    if [ ! -S "$SOCK" ]; then
      echo "ERROR: daemon socket not found at $SOCK"
      echo "Is fusiond running? Check: ps -A | grep fusiond"
      exit 1
    fi

    case "$1" in
      hide)
        case "$2" in
          on|ON|1)   echo "HIDE ON" | timeout 3 nc -U "$SOCK" ;;
          off|OFF|0) echo "HIDE OFF" | timeout 3 nc -U "$SOCK" ;;
          *) echo "usage: fusionctl hide [on|off]"; exit 1 ;;
        esac
        ;;
      layout)
        if [ -z "$2" ] || [ -z "$3" ]; then
          echo "usage: fusionctl layout <pad_index> [NINTENDO|XBOX|DEFAULT]"
          exit 1
        fi
        echo "LAYOUT $2 $3" | timeout 3 nc -U "$SOCK"
        ;;
      status)
        echo "STATUS" | timeout 3 nc -U "$SOCK"
        ;;
      rescan)
        echo "RESCAN" | timeout 3 nc -U "$SOCK"
        ;;
      quit)
        echo "QUIT" | timeout 3 nc -U "$SOCK"
        ;;
      ping)
        echo "PING" | timeout 3 nc -U "$SOCK"
        ;;
      *)
        cat <<EOF
usage: fusionctl <command> [args]

Commands:
  hide [on|off]                   Hide/unhide physical controllers
  layout <idx> <mode>             Set layout for pad idx (NINTENDO|XBOX|DEFAULT)
  status                          Query current state (JSON)
  rescan                          Rescan all pads and reset axis disambiguation
  restart                         Restart the daemon and rescan pads
  quit                            Stop the daemon

Examples:
  fusionctl hide on               # Hide physical pads, Fusion only
  fusionctl hide off              # Restore physical pads
  fusionctl layout 0 NINTENDO     # Remap pad 0 (8BitDo) to Nintendo layout
  fusionctl restart               # Restart daemon & rescan pads (fixes axis issues)
  fusionctl rescan                # Rescan pads without restarting daemon
  fusionctl status                # Show current config
EOF
        exit 1
        ;;
    esac
    ;;
esac
