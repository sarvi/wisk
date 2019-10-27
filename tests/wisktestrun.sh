SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SCRIPT_NAME="$(basename "${BASH_SOURCE[0]}")"
SCRIPT="$SCRIPT_DIR/$SCRIPT_NAME"
WORKSPACE_DIR="$( dirname "$SCRIPT_DIR" )"
SCRIPT_SHORT_NAME="${SCRIPT_NAME%.*}"


tee2log() {
    local log_path

    [[ -n "${1:-}" ]] || { echo "Must supply log path" >&2; return 1; }
    log_path="$1"

    if [[ ! -f "$log_path" ]]; then
        mkdir -p "$( dirname "$log_path" )"
    fi

    # tee to log
    echo "exec > >( tee -a $log_path ) 2>&1" >&2
    exec > >( tee -a "$log_path" ) 2>&1 || true
}

tee2log program_uut.log


# LD_PRELOAD="$WORKSPACE_DIR/src/lib/libwisktrack.so"
# LD_LIBRARY_PATH="$WORKSPACE_DIR/src/lib64:$WORKSPACE_DIR/src/lib"
LD_LIBRARY_PATH="$WORKSPACE_DIR/src/lib32:$WORKSPACE_DIR/src/lib64"
LD_PRELOAD="libwisktrack.so"
WISK_TRACKER_PIPE="/tmp/wisk_tracker.pipe"
WISK_TRACKER_UUID="XXXXXXXX-XXXXXXXX-XXXXXXXX"

echo "Command: $*"
rm -f $WISK_TRACKER_PIPE
mkfifo $WISK_TRACKER_PIPE
LD_PRELOAD="$LD_PRELOAD" WISK_TRACKER_PIPE="$WISK_TRACKER_PIPE" WISK_TRACKER_UUID="$WISK_TRACKER_UUID=" $*

