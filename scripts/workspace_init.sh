#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SCRIPT_NAME="$(basename "${BASH_SOURCE[0]}")"
SCRIPT="$SCRIPT_DIR/$SCRIPT_NAME"
WORKSPACE_DIR="$( dirname "$SCRIPT_DIR" )"
INSTANCE="$( basename "$WORKSPACE_DIR" )"
SCRIPT_SHORT_NAME="${SCRIPT_NAME%.*}"

# Source common include
# shellcheck source=./common_inc.sh
. "$SCRIPT_DIR/common_inc.sh"
# shellcheck source=./ws_init_inc.sh
. "$SCRIPT_DIR/ws_init_inc.sh"


# Create local client logdirs
init_log_dirs() {
    local log_dir
    log_dir="${1:-}"
    [[ -n "${1:-}" ]] || log_dir="$WORKSPACE_DIR/logs"

    local umask0 archive_dir program_group
    umask0="$( umask -p )"  # Prints statement that can be invoked
    umask 0000 || true

    mkdir -p "$log_dir"

    archive_dir="$(dirname "$log_dir")/archive"
    mkdir -p "$archive_dir"

    for program_group in "${CLIENT_PROGRAM_GROUPS[@]}"
    do
        mkdir -v -p "$log_dir/$program_group"
        mkdir -v -p "$log_dir/${program_group}_error"
    done

    chmod -R a+rwX "$log_dir" "$archive_dir" || true

    $umask0 || true
}


set -x
set -o nounset
set -o pipefail
set -o errexit
setup_trap_for_err

CLIENT_PROGRAM_GROUPS=( 'wisk' 'wisktrack' )
init_log_dirs "$WORKSPACE_DIR/logs"

[[ -n "${USER:-}" ]] || USER="$(id -u -n)"
[[ -n "${HOSTNAME:-}" ]] || HOSTNAME="$(hostname -s)"

# LOG_PATH="/auto/wisk-log/logs/local/$SCRIPT_SHORT_NAME/${SCRIPT_SHORT_NAME}_${USER}_${HOSTNAME}_${INSTANCE}_$$.log"
LOG_PATH="$WORKSPACE_DIR/logs/$SCRIPT_SHORT_NAME/${SCRIPT_SHORT_NAME}_${USER}_${HOSTNAME}_${INSTANCE}_$$.log"
echo "Logging to $LOG_PATH" >&2
tee2log "$LOG_PATH"


echo "$SCRIPT $*"
echo "Workspace Dir: $WORKSPACE_DIR"
echo "Instance: $INSTANCE"
echo "HOSTNAME: $HOSTNAME"

set_ws_install_type "local" "$WORKSPACE_DIR"

if [[ -n "${1:-}" ]]; then
    VIRTENVPATH="$1"
    mkdir -p "$VIRTENVPATH" || exit_with_code_msg 1 "Failed to create directory $VIRTENVPATH"
    VIRTENVPATH="$(cd "$VIRTENVPATH" && pwd -P )"
else
    VIRTENVROOT="$HOME/virtenv"    # default
    if [[ ! -e "$VIRTENVROOT" && -x /router/bin/servinfo ]]; then
        ME="$( whoami )"
        SITE="$( /router/bin/servinfo --site )"
        WKSPACE="/ws/${ME}-$SITE"
        if [ -e "$WKSPACE" ]
        then
            mkdir -p "$WKSPACE/virtenv" || exit_with_code_msg 1 "Failed to create $WKSPACE/virtenv"
            ln -s "$WKSPACE/virtenv" "$VIRTENVROOT"
        fi
    fi

    VIRTENVPATH="$VIRTENVROOT/wisk"
fi

echo "VIRTENVPATH $VIRTENVPATH"

### Make sure VIRTUAL_ENV not activated
[[ -z "${VIRTUAL_ENV:-}" ]] || exit_with_code_msg 1 "Failed.  Deactivate virtualenv before initializing workspace." >&2

# Update the virtualenv
if ! update_virtualenv "$SCRIPT_DIR" "$VIRTENVPATH"
then
    exit_with_code_msg 1 "Failed to update virtenv.  Terminating"
    exit 1
fi

echo "Logging to $LOG_PATH" >&2

[[ -z "${SKIP_DOC_INIT:-}" ]] || exit_with_code_msg 0 "$SCRIPT_NAME Successful (skipped doc init)."

safe_activate "$VIRTENVPATH"
doc_init "$WORKSPACE_DIR" "$VIRTENVPATH"

echo "Logged to $LOG_PATH" >&2

exit_with_code_msg 0 "Workspace initialization successful"
