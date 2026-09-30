#!/system/bin/sh
# fusionctl - control the Fusion Controller daemon via socket
SOCK=/data/adb/fusion.sock

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
  quit                            Stop the daemon

Examples:
  fusionctl hide on               # Hide physical pads, Fusion only
  fusionctl hide off              # Restore physical pads
  fusionctl layout 0 NINTENDO     # Remap pad 0 (8BitDo) to Nintendo layout
  fusionctl status                # Show current config
EOF
    exit 1
    ;;
esac
