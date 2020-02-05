#!/bin/bash

# set -euo pipefail
set -o errexit
set -o nounset
set -o pipefail
set -x

# It is important to leave paths as input, DO NOT CONVERT
SCRIPT_DIR="$(dirname "${BASH_SOURCE[0]}")"
SCRIPT_NAME="$(basename "${BASH_SOURCE[0]}")"
SCRIPT="$SCRIPT_DIR/$SCRIPT_NAME"
WORKSPACE_DIR="$( dirname "$SCRIPT_DIR" )"

# Source common include
echo "Set config/wit_install_type.cfg"
rm -f "$WORKSPACE_DIR/config/wit_install_type.cfg"
ln -s "$WORKSPACE_DIR/config/wit_install_type_prod.cfg" "$WORKSPACE_DIR/config/wit_install_type.cfg"

# shellcheck source=./common_inc.sh
. "$SCRIPT_DIR/common_inc.sh"
# shellcheck source=./ws_init_inc.sh
. "$SCRIPT_DIR/ws_init_inc.sh"

echo "$SCRIPT $*"
echo "Workspace Dir: $WORKSPACE_DIR"
echo "HOSTNAME: $HOSTNAME"

if [[ -n "${1:-}" ]]
then
    VIRTENVPATH="$1"
    # Use path as passed in
    [[ -d "$VIRTENVPATH" ]] || mkdir -p "$VIRTENVPATH"
else
    VIRTENVPATH="$WORKSPACE_DIR/virtenv"
    mkdir -p "$VIRTENVPATH"
fi

# Setup virtenv for creating the docs
echo "VIRTENVPATH: $VIRTENVPATH"
"$SCRIPT_DIR/cli_pyenv_setup.sh" "$VIRTENVPATH"

safe_activate "$VIRTENVPATH"
client_doc_init "$WORKSPACE_DIR" "$VIRTENVPATH"
deactivate || true

if [[ -n "${2:-}" ]]
then
    # Lightweight virtualenv for cli
    CLI_VIRTENVPATH="$2"
    mkdir -p "$CLI_VIRTENVPATH"
    "$SCRIPT_DIR/cli_pyenv_setup.sh" "$CLI_VIRTENVPATH"
fi

echo "Client virtualenv initialization successful."
