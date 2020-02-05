#!/bin/bash

show_env() {
    local virtenvpath
    [[ -z "${1:-}" ]] || virtenvpath="$1"
    /bin/hostname
    /usr/bin/locale
    declare -p
    declare -F
    env | sort
}

safe_activate() {
    local virtenvpath u_str

    [[ -n "${1:-}" ]] || { echo "Missing mandatory arg: virtenvpath" >&2; return 1; }
    virtenvpath="$1"

    # Turn off -u (nounset) if set
    u_str="$( opt_str u )"; set +u

    # shellcheck disable=SC1090
    . "$virtenvpath/bin/activate"  # Source is too variable to point to

    # Restore original setting
    set "$u_str"
}

safe_deactivate() {
    # This prevents errors from unset vars.
    local u_str

    u_str="$( opt_str u )"; set +u
    deactivate

    set "$u_str"
}

# is only if config/wit_install_type.cfg contains install_type dev|test|stage|prod
# This must be caught in if then
is_server_workspace() {
    [[ -n "${1:-}" && -d "$1" ]] || return 1
    local workspace_dir
    workspace_dir="$1"

    if is_in_array "$(get_ws_install_type "$workspace_dir")"  "dev" "test" "stage" "prod"
    then
        # Verify host to be sure
        if is_in_array "$(hostname -s)" sjc-ww{d,s,p}l-flxcl{1..4}
        then
            return 0
        fi
    fi
    return 1
}

get_ws_install_type() {
    local workspace_dir current_cfg

    [[ -z "${1:-}" ]] && echo "ERROR: Must provide workspace_dir as argument" >&2 && return 1
    workspace_dir="$1"

    current_cfg="$workspace_dir/config/wit_install_type.cfg"

    local install_type
    if [[ -r "$current_cfg" ]]
    then
        # Really should strip all space and comments from file, then parse for install_type
        install_type="$(grep -h "^install_type =" "$current_cfg" | awk '{print $NF}')"
    elif [[ "$workspace_dir" =~ /auto/flexclone/wwwin-wit ]]
    then
        case "$(basename "$workspace_dir")" in
            wwwin-wittest-dev*)
                install_type="test"
                ;;
            wwwin-wit-dev*)
                install_type="dev"
                ;;
            wwwin-wit-stage*)
                install_type="stage"
                ;;
            *)
                install_type="prod"
                ;;
        esac
    else
        install_type="local"
        echo "Unknown install type - default to $install_type" >&2
    fi
    echo "$install_type"
}

doc_init() {
    local workspace_dir virtenvpath did_activate

    workspace_dir="$1"
    virtenvpath="$2"

    client_doc_init "$1" "$2"

    # Activate if not already activated
    if [[ -z "${VIRTUAL_ENV:-}" ]]
     then
        did_activate=1
        safe_activate "$virtenvpath"
    fi

    cdop_or_exit "$workspace_dir/doc" pushd
    make html
    cdop_or_exit popd

    if [[ ${did_activate:-0} -eq 1 ]]
    then
        safe_deactivate
    fi
}

client_doc_init() {
    local workspace_dir virtenvpath did_activate

    workspace_dir="$1"
    virtenvpath="$2"

    # Activate if not already activated
    if [[ -z "${VIRTUAL_ENV:-}" ]]
     then
        did_activate=1
        safe_activate "$virtenvpath"
    fi

    echo "Generate Code documentation" >&2
    cdop_or_exit "$workspace_dir/doc" pushd
    echo "DOC VIRTUAL_ENV=$VIRTUAL_ENV"
#    make clean
#    make man
    # make info
    cdop_or_exit popd

    if [[ ${did_activate:-0} -eq 1 ]]
    then
        safe_deactivate
    fi
}

# Prevent skipping of read of virtenv requirements
clear_virtualenv_signature() {
    local virtenvpath
    [[ -n "${1:-}" ]] || { echo "Missing mandatory arg: virtenvpath" >&2; return 1; }
    virtenvpath="$1"

    echo "rm -vf $virtenvpath/*.signature" >&2
    rm -vf "$virtenvpath/"*.signature
}


update_virtualenv() {
    [[ "$#" -eq 2 ]] || { echo "${FUNCNAME[0]} expected 2 params not $#: $*"; errexit 1; }

    local script_dir virtenvpath
    script_dir="$1"
    virtenvpath="$2"

    if [[ -n "${VIRTUAL_ENV:-}" ]]
    then
        echo "You can't upgrade the virtualenv while activated" >&2
        return 1
    fi
    show_env "$virtenvpath"

    local return_code=0
    "$script_dir/devpyenv_setup.sh" "$virtenvpath" || return_code="$?"
    if [[ "$return_code" -ne 0 ]]
    then
        printf 'Python environment setup failed with code: %d. Not proceeding.\n' "$return_code" >&2
    fi
    return ${return_code}
}

# Creates symlinks for install_type and initial data.
# Mandatory argument: install_type { local dev test stage prod }
# Optional: workspace_dir. Default is $WORKSPACE_DIR || parent dir of BASH_SOURCE[0]
set_ws_install_type() {

    local install_type ws_dir

    [[ -n "${1:-}" ]] || return 1
    install_type="$1"

    if [[ -n "${2:-}" ]]
    then
        ws_dir="$2"
    elif [[ -n "${WORKSPACE_DIR:-}" && -d "$WORKSPACE_DIR" ]]
    then
        ws_dir="$WORKSPACE_DIR"
    else
        ws_dir="$( dirname "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" )"
    fi

    if ! is_in_array "$install_type" "local" "dev" "test" "stage" "prod"
    then
        echo "Unknown install_type $install_type" >&2
        return 1
    fi

    echo "Set src/initial_data.json" >&2
    rm -v -f "$ws_dir/src/initial_data.json"
    ln -v -s "$ws_dir/config/initial_data_${install_type}.json" "$ws_dir/src/initial_data.json"

    echo "Set config/wit_install_type.cfg" >&2
    rm -vf "$ws_dir/config/wit_install_type.cfg"
    ln -v -s "wit_install_type_${install_type}.cfg" "$ws_dir/config/wit_install_type.cfg"
}

# tag_from_tagtype_commit
tag_from_commit() {
    local tagtype tag_template commit=HEAD
    local -i abbrev=4

    tagtype="$1"
    tag_template="WISK_${tagtype^^}"
    if [[ -n "${3:-}" ]]
    then
        abbrev="$2"
        commit="$3"
    elif [[ "${#2}" -eq 1 ]]
    then
        abbrev="$2"
    else
        commit="$2"
    fi

    if [[ ! "$commit" =~ $tag_template ]]
    then
        tag="$( git describe --tags --match "${tag_template}_[--9A-Z]*" --always "--abbrev=$abbrev" "$commit" )" || true
        if [[ -n "${tag:-}" && "$tag" =~ $tag_template ]]
        then
            echo "Replacing commit with equivalent server tag: $commit -> $tag" >&2
            echo "$tag"
            return
        else
            echo "Could not improve on commit $commit" >&2
        fi
    fi
    echo "$commit"
}

last_client_tag_from_commit() {

    tag_from_commit client 0 "${1:-HEAD}"
}


log_file_from_scriptname() {
    local scriptname logroot subname log_file
    scriptname="$1"
    logroot="$2"
    [[ -n "${3:-}" ]] && subname="$3" || subname="$( hostname -s )"

    short_name="${scriptname%.*}"
    group="${scriptname%%[-_.]*}"
    log_file="$logroot/$group/${short_name}_${subname}_$( logdate ).log"

    echo "$log_file"
}

rotate_files() {  # filename ext alt
    # rename filename to extension or to modified date
    # Example:
    # rotate_files pip_freeze txt orig
    # mv pip_freeze_orig.txt to pip_freeze_$(mod date).txt && mv pip_freeze.txt pip_freeze_orig.txt
    local path ext alt

    path="$1"
    ext="$2"
    if [[ -n "${3:-}" ]]
    then
        alt="$3"
        if [[ -e "${path}_${alt}.$ext" ]]
        then
            mv -v "${path}_${alt}.$ext" "${path}_$(date -r "${path}_${alt}.$ext" '+%s').$ext"
        fi
        if [[ -e "${path}.$ext" ]]
        then
            mv -v "${path}.$ext" "${path}_${alt}.$ext"
        fi
    else
        if [[ -e "${path}.$ext" ]]
        then
            mv -v "${path}.$ext" "${path}.$(date -r "${path}.$ext" '+%s').$ext"
        fi
    fi
}

log_virtenv_state() {
    [[ "$#" -eq 3 ]] || { echo "${FUNCNAME[0]} expected 3 params not $#: $*"; errexit 1; }

    local virtenvpath install_type workspace_dir
    virtenvpath="$1"
    install_type="$2"
    workspace_dir="$3"

    local log_path
    if [[ "$install_type" == "local" ]]
    then
        log_path="$workspace_dir/logs/workspace_init/pip_freeze"

    else
        log_path="/auto/wit-log/logs/$install_type/deploy/pip_freeze_$(basename "$workspace_dir")"
    fi

    if [[ -x "$virtenvpath/bin/pip3" ]]
    then
        rotate_files "$log_path" "txt" "last"

        "$virtenvpath/bin/pip3" freeze > "${log_path}.txt"
    fi
}
