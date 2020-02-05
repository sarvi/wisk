#!/bin/bash

# Remove aliases unless being sourced from interactive shell
[[ $0 =~ bash && $- =~ i ]] || unalias -a

# exit with indicated code or 0
# if second arg exists then first write to stderr
exit_with_code_msg() {
    local code
    code="${1:-0}"
    [[ -z "${2:-}" ]] || echo "$2" >&2

    # Remove exit trap
    trap - EXIT
    trap - ERR

    exit "$code"
}

errexit() {

    _errexit_err="$?"
    _errexit_err_code="${1:-1}"

    # set +x

    printf '\n\n###### ERREXIT ######\n\n' >&2
    echo "Error in ${BASH_SOURCE[1]}:${BASH_LINENO[0]}. '${BASH_COMMAND}' exited with status: '$_errexit_err', code: '$_errexit_err_code'" >&2
    # Print out the stack trace described by $function_stack
    if [ ${#FUNCNAME[@]} -ge 2 ]
    then
        echo "Call tree:" >&2
        for ((i=1;i<${#FUNCNAME[@]}-1;i++))
        do
            echo " $i: ${BASH_SOURCE[$i+1]}:${BASH_LINENO[$i]} ${FUNCNAME[$i]}(...)" >&2
        done
    fi

    if [[ ${_errexit_err_code} -eq 0 ]]
    then
        echo "errexit wants to return 0 - returning 1 for error instead" >&2
        _errexit_err_code=1
    fi
    echo "Exiting with status $_errexit_err_code" >&2
    exit_with_code_msg "$_errexit_err_code" "$(basename "$0") Failed"
}

setup_trap_for_err() {
    trap 'errexit' ERR

    set -o errtrace
}

reverse_array() {
    [[ $# -gt 0 ]] || return  # 0 elem edge case

    local -a array yarra
    local idx

    array=( "$@" )
    (( idx=${#array[@]}-1 )) || true
    yarra=( "${array[$idx]}" )  # yarra must be initialized w elem before referencing
    for (( idx=${#array[@]}-2 ; idx>=0 ; idx-- )); do
        yarra=( "${yarra[@]}" "${array[idx]}" )
    done

    # Echo to return values
    echo "${yarra[@]}"  # Intentionally Not >&2
}

is_in_array () {
    [[ $# -ge 2 ]] || return 1

    for v in "${@:2}"; do
        [[ "$v" == "$1" ]] && return 0
    done
    return 1
}

join() {
    # Join remaining params with single char
    [[ -n "${2:-}" ]] || return 1

    local IFS="$1"
    shift
    /bin/echo "$*"
}

join_str() {
    # Join remaining params with string
    [[ -n "${2:-}" ]] || return 1
    # Todo: Should make sure join_char is not in any param
    local join_char join_str
    join_char='|'
    join_str="$1"
    shift
    join "$join_char" "$@" | sed -e "s/$join_char/$join_str/g"
}

abspath() {
    [[ -n "${1:-}" ]] || return 1
    local path dir
    path="$1"

    while [[ -h "$path" ]]; do
        if [ -d "$path" ]; then
            path="$( cd -P "$path" && pwd )"
        else
            dir="$( cd -P "$( dirname "$path" )" && pwd )"
            path="$( readlink "$path" )"
            [[ "$path" != /* ]] && path="$dir/$path"
        fi
    done
    if [[ "$path" != /* ]]; then
        dir="$( cd -P "$( dirname "$path" )" && pwd )"
        path="$dir/$( basename "$path" )"
    fi
    # handle edge case of relative path to symlink to directory
    if [[ -h "$path" ]]; then
        abspath "$path"
    else
        echo "$path"  # Intentionally Not >&2
    fi
}

# shellcheck disable=SC2120
epoch() {
    local -a args=( "+%s" )

    [[ -z "${1:-}" ]] || args=( "${args[@]}" "$@" )
    date "${args[@]}"
}

start_epoch() {
    # shellcheck disable=SC2119
    [[ -n "${_START_EPOCH:-}" ]] || _START_EPOCH="$(( $( epoch ) - SECONDS ))"

    echo "$_START_EPOCH"  # Intentionally Not >&2
}

logdate() {
    # Gets the start of the script as string
    [[ -n "${_LOGDATE:-}" ]] || _LOGDATE="$(date "+%y%m%d%H%M%S" --date="@$(start_epoch)" 2> /dev/null )" || true
    [[ -n "${_LOGDATE:-}" ]] || _LOGDATE="$(date -r "$(start_epoch)" "+%y%m%d%H%M%S" 2> /dev/null )" || true
    [[ -n "${_LOGDATE:-}" ]] || _LOGDATE="$(start_epoch)"
    echo "$_LOGDATE"  # Intentionally Not >&2
}
# Reset _LOGDATE
[[ -z "${_LOGDATE:-}" ]] || unset _LOGDATE
logdate > /dev/null 2>&1 || true

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

# cd or pushd to directory or echo and exit 1
# will cd unless pushd is second arg
cdop_or_exit() {
    local dirpath cdop="cd"

    [[ -n "${1:-}" ]] || { echo "Missing arg: path or popd" >&2; return 1; }
    [[ -n "${2:-}" ]] && cdop="$2"

    if [ "$1" = popd ]
    then
        popd || exit_with_code_msg 1 "Failed to popd"

    else
        dirpath="$1"
        "$cdop" "$dirpath" || exit_with_code_msg 1 "Failed to $cdop to $dirpath"
    fi
}

is_bash_option_set() {
    [[ -n "${1:-}" ]] || return 1

    local is_set=false
    [[ $- =~ $1 ]] && is_set=true

    echo "$is_set"  # Intentionally Not >&2
}

opt_str() {
    [[ -n "${1:-}" ]] || return 1

    local sign="+"
    [[ $- =~ $1 ]] && sign="-"

    echo "$sign$1"  # Intentionally Not >&2
}