#!/bin/bash -x

# ToDo - Enable these.
set -o errexit
set -o nounset
set -o pipefail

SCRIPT_NAME="$(basename "${BASH_SOURCE[0]}")"
# SHORT_NAME="${scriptname%.*}"
if [[ "$SCRIPT_NAME" == devpyenv_setup.sh ]]
then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
else
    SCRIPT_DIR="$(dirname "${BASH_SOURCE[0]}" )"  # don't mess up /sw/packages
fi
SCRIPT="$SCRIPT_DIR/$SCRIPT_NAME"
WORKSPACE_DIR="$( dirname "$SCRIPT_DIR" )"
INSTANCE="$( basename "$WORKSPACE_DIR" )"
OSTYPE="$(uname)"

# shellcheck source=./common_inc.sh
. "$SCRIPT_DIR/common_inc.sh"
# shellcheck source=./ws_init_inc.sh
. "$SCRIPT_DIR/ws_init_inc.sh"

INSTALL_TYPE="$( get_ws_install_type "$WORKSPACE_DIR" )"
echo "INSTALL_TYPE: $INSTALL_TYPE" >&2

echo "$SCRIPT_NAME $*"
echo "Path: $PATH"
echo "Workspace Dir: $WORKSPACE_DIR"
echo "Instance: $INSTANCE"

# PROJECT="wisk"
[[ -n "${1:-}" ]] || { echo "Missing mandatory argument: virtenvpath"; exit 1; }
VIRTENVPATH="$1"

if [[ $OSTYPE = "Darwin" ]]
then
    MD5SUM="md5 -r"
    PYTHON="$(command -v python)"
    VIRTUALENV="$(command -v virtualenv)"
else
    MD5SUM="md5sum"

    # Linux Python
    # TODO: Refactor out so only in deploy_to_server
    if [[ -e /opt/eif/web/common/bin/python ]]
    then
        PYTHON=/opt/eif/web/common/bin/python
        VIRTUALENV=/opt/eif/web/common/bin/virtualenv
        [[ -x "$VIRTENVPATH/bin/python3" || -z "${LD_LIBRARY_PATH:-}" ]] || unset LD_LIBRARY_PATH
    else
        if [[ -e /sw/packages/python3/3.5.0/bin/virtualenv ]]
        then
            PYTHON='/sw/packages/python3/3.5.0/bin/python3'
            VIRTUALENV='/sw/packages/python3/3.5.0/bin/virtualenv'
            # Prepend /router/bin at front of path for virtualenv install/upgrade?
        else
            PYTHON='/sw/packages/python3/3.5.0/bin/python3'
            VIRTUALENV='/sw/packages/python3/3.5.0/bin/virtualenv'
        fi

        PATH="$(dirname "$PYTHON"):$PATH"
        PKG_CONFIG_PATH="$(dirname "$PYTHON")/lib/pkgconfig:/sw/packages/libffi/current/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
        LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/sw/packages/libffi/current/lib64"
        export PATH PKG_CONFIG_PATH LD_LIBRARY_PATH
    fi
fi

# Report configuration
echo "Using Python: $PYTHON"
echo "Using Virtualenv: $VIRTUALENV"
echo "Using PKG_CONFIG_PATH: ${PKG_CONFIG_PATH:-}"
echo "Using LD_LIBRARY_PATH: ${LD_LIBRARY_PATH:-}"
echo "VIRTUALENV_PATH=$VIRTENVPATH"

# Do signature checking to see if the virtualenv needs to be updated.
CALC_SIG=""
VIRTENV_SIGNATURE="$VIRTENVPATH/virtenv.signature"
echo "Signature File: $VIRTENV_SIGNATURE"
for req_file in "$WORKSPACE_DIR"/requirements.*.txt "$SCRIPT" "$SCRIPT_DIR/ws_init_inc.sh" "$SCRIPT_DIR/workspace_init.sh"
do
    REQ_SIG="$( ${MD5SUM} "$req_file" )"
    CALC_SIG="$CALC_SIG $REQ_SIG"
done

if [[ ! -e "$VIRTENV_SIGNATURE" ]]
then
    echo "New VirtualEnv Install(missing signature file)"
else
    CUR_SIG="$( cat "$VIRTENV_SIGNATURE" )"
    echo "Current Signature   : $CUR_SIG"
    echo "Calculated Signature: $CALC_SIG"
    if [[ "$CUR_SIG" = "$CALC_SIG" ]]
    then
        echo "Virtualenv up-to-date !!!"
        exit 0
    fi
    echo "VirtualEnv Upgrade(Signature out-of-date)"
    rm "$VIRTENV_SIGNATURE"
fi

log_virtenv_state "$VIRTENVPATH" "$INSTALL_TYPE" "$WORKSPACE_DIR" || true

rm -f "$WORKSPACE_DIR/pip.log"
# Make sure pip is up to date.  Prevents sporadic certificate errors
if ! "$VIRTENVPATH/bin/pip3" install --upgrade --log "$WORKSPACE_DIR/pip.log" pip; then
    echo "Failed to upgrade pip - re-initializing virtualenv"
    "$VIRTUALENV" --clear -p "$PYTHON" "$VIRTENVPATH"
    # Attempt upgrade a second time
    "$VIRTENVPATH/bin/pip3" install --upgrade --log "$WORKSPACE_DIR/pip.log" pip
fi

echo "Updating virtualenv at $VIRTENVPATH"
if [[ -f "$WORKSPACE_DIR/requirements.setup.txt" ]]
then
    "$VIRTENVPATH/bin/pip3" install --upgrade --log "$WORKSPACE_DIR/pip.log" -r "$WORKSPACE_DIR/requirements.setup.txt"
fi

if [[ "$SCRIPT_NAME" == cli_pyenv_setup.sh && "$(basename "$VIRTENVPATH")" = *CLIENT* ]]
then
    "$VIRTENVPATH/bin/pip3" install --upgrade --log "$WORKSPACE_DIR/pip.log" -r "$WORKSPACE_DIR/requirements.cli.txt"
    echo "Virtualenv to deploy for CLI configured successfully !!!"
    exit
fi

"$VIRTENVPATH/bin/pip3" install --log "$WORKSPACE_DIR/pip.log" -r "$WORKSPACE_DIR/requirements.orig.txt"
if [[ "$SCRIPT_NAME" == devpyenv_setup.sh && -r "$WORKSPACE_DIR/requirements.${OSTYPE}.txt" ]]
then
    "$VIRTENVPATH/bin/pip3" install --upgrade --log "$WORKSPACE_DIR/pip.log" -r "$WORKSPACE_DIR/requirements.${OSTYPE}.txt"
fi

log_virtenv_state "$VIRTENVPATH" "$INSTALL_TYPE" "$WORKSPACE_DIR" || true

echo "$CALC_SIG" > "$VIRTENV_SIGNATURE"

echo "Virtualenv configuration successful !!!"
