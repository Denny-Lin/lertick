#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Foreground native deployment. Never replace another root sched_ext scheduler.
set -eu
fail() { printf 'lertick: %s\n' "$*" >&2; exit 1; }
usage()
{
    cat <<'EOF'
Usage: ./deploy.sh COMMAND [OPTIONS]
       lertick COMMAND [OPTIONS]          (after installation)

  check       Check kernel interfaces and privileges; never attach a scheduler.
  verify      Load BPF for verifier acceptance, then close it without attaching.
  plan        Print a finite run command only; no build, sudo, load or attach.
  run         Foreground online trial, 30 seconds, learning all tasks by default.
  install     Build as your normal user, then install the loader and helper.
  uninstall   Remove installed files only when scheduler maintenance is safe.
  status      Read the current sched_ext state and scheduler name.
  help        Show this help.

plan/run options:
  --duration SECONDS        1..3600 (default 30)
  --mode MODE               fixed|shadow|online|ewma (default online)
  --sample-ms MILLISECONDS   100..10000 (default 1000)
  --stats-json FILE          New telemetry file, or - for stdout

Shadow mode is an ACTIVE fixed-slice scheduler, not a dry-run. Plan never
attaches; verify creates temporary BPF objects but does not change scheduling.
Native source builds may download pinned libbpf 1.7.0 as your normal user.
No system libraries, kernel packages, boot configuration or services are changed.
A compatible running sched_ext kernel is required; acquiring/booting one may
require a reboot. Another active scheduler must be stopped through its manager.
PREFIX defaults to /usr/local. DESTDIR only stages install/uninstall files;
staging never invokes sudo or runs the loader. No service is started or enabled.
EOF
}
case $0 in */*) script=$0 ;; *) script=$(command -v "$0") || fail 'cannot locate helper' ;; esac
tool_dir=$(CDPATH= cd -P "$(dirname "$script")" && pwd)
script=$tool_dir/$(basename "$script")
source_tree=no
if [ -f "$tool_dir/Makefile" ] && [ -f "$tool_dir/src/userspace/scx_lertick.c" ]; then
    source_tree=yes
    loader=$tool_dir/build/scx_lertick
    prefix=${PREFIX:-/usr/local}
else
    installed_prefix=$(CDPATH= cd -P "$tool_dir/.." && pwd)
    loader=$installed_prefix/sbin/scx_lertick
    prefix=${PREFIX:-$installed_prefix}
fi
destdir=${DESTDIR:-}
command=${1:-help}
if [ "$#" -gt 0 ]; then shift; fi
case $command in
    help|--help|-h) usage; exit 0 ;;
    check|verify|plan|run|install|uninstall|status|install-locked|uninstall-locked) ;;
    *) usage >&2; fail "unknown command: $command" ;;
esac
validate_path()
{
    case $2 in /*) ;; *) fail "$1 must be absolute" ;; esac
    case $2 in *[!/]*) ;; *) fail "$1 must not resolve to /" ;; esac
    case "$2/" in */../*|*/./*) fail "$1 must not contain . or .. components" ;; esac
}
validate_path PREFIX "$prefix"
if [ -n "$destdir" ]; then
    validate_path DESTDIR "$destdir"
    case $command in install|uninstall) ;; *) fail 'DESTDIR is only for install/uninstall staging' ;; esac
fi
stats_requested=no
validate_value()
{
    [ -n "$2" ] || fail "$1 needs a value"
    case $1 in
        --mode) case $2 in fixed|shadow|online|ewma) ;; *) fail 'invalid mode' ;; esac ;;
        --duration|--sample-ms)
            case $2 in *[!0-9]*) fail "$1 must be an integer" ;; esac
            if [ "$1" = --duration ]; then low=1; high=3600; else low=100; high=10000; fi
            [ "$2" -ge "$low" ] 2>/dev/null && [ "$2" -le "$high" ] 2>/dev/null || fail "$1 must be $low..$high" ;;
        --stats-json) stats_requested=yes ;;
    esac
}
validate_options()
{
    while [ "$#" -gt 0 ]; do
        case $1 in
            --duration|--mode|--sample-ms|--stats-json)
                [ "$#" -ge 2 ] || fail "$1 needs a value"
                validate_value "$1" "$2"; shift 2 ;;
            --duration=*|--mode=*|--sample-ms=*|--stats-json=*)
                validate_value "${1%%=*}" "${1#*=}"; shift ;;
            *) fail "unsupported option: $1 (fault injection and scheduler replacement are unavailable here)" ;;
        esac
    done
}
case $command in plan|run) validate_options "$@" ;; *) [ "$#" -eq 0 ] || fail "$command does not accept options" ;; esac
if [ "$command" = plan ]; then
    printf 'Run plan (not executed; requires a compatible idle sched_ext kernel):\n'
    # Single quotes make arbitrary telemetry names reviewable shell arguments.
    for word in sudo "$loader" --mode online --duration 30 --track-all --cpuperf auto "$@"; do
        escaped=$(printf '%s' "$word" | sed "s/'/'\\\\''/g")
        printf " '%s'" "$escaped"
    done
    printf '\nNo scheduler was loaded or attached.\n'
    exit 0
fi
[ "$(uname -s)" = Linux ] || fail 'native deployment requires Linux'
trusted_root_path()
{
    trust_remaining=${1#/}; trust_path=/
    while :; do
        [ ! -L "$trust_path" ] || fail "symlink in privileged installation path: $trust_path"
        if [ -e "$trust_path" ]; then
            trust_stat=$(stat -c '%u %a' "$trust_path") || fail "cannot inspect: $trust_path"
            set -- $trust_stat
            [ "$#" -eq 2 ] && [ "$1" = 0 ] || fail "installation path must be root-owned: $trust_path"
            case $2 in ''|*[!0-7]*) fail "invalid permissions: $trust_path" ;; esac
            [ "$((0$2 & 022))" -eq 0 ] || fail "installation path must not be group/other writable: $trust_path"
            if [ -n "$trust_remaining" ]; then [ -d "$trust_path" ] || fail "ancestor is not a directory: $trust_path"; fi
        fi
        [ -n "$trust_remaining" ] || break
        trust_component=${trust_remaining%%/*}
        case $trust_remaining in */*) trust_remaining=${trust_remaining#*/} ;; *) trust_remaining= ;; esac
        [ -n "$trust_component" ] || continue
        trust_path=${trust_path%/}/$trust_component
    done
}
trusted_installation() { trusted_root_path "$1/bin"; trusted_root_path "$1/sbin"; }
scheduler_ready()
{
    state=$(cat /sys/kernel/sched_ext/state 2>/dev/null) || fail 'sched_ext is unavailable; boot a compatible kernel (a reboot may be required)'
    [ "$state" = disabled ] || fail "NOT READY: sched_ext state=$state; stop the current scheduler through its own manager first"
}
show_status()
{
    if ! state=$(cat /sys/kernel/sched_ext/state 2>/dev/null); then
        printf 'sched_ext: unavailable in the running kernel\n'; return 1
    fi
    printf 'sched_ext: %s\n' "$state"
    if ops=$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null); then printf 'scheduler: %s\n' "$ops"; fi
}
if [ "$command" = status ]; then show_status; exit $?; fi
ensure_loader()
{
    if [ "$source_tree" = yes ] && [ "$(id -u)" -ne 0 ]; then
        "$tool_dir/scripts/native_build.sh" >&2
    fi
    [ -f "$loader" ] && [ -x "$loader" ] && [ ! -L "$loader" ] ||
        fail 'loader is unavailable; build as your normal user with scripts/native_build.sh first'
    if [ -z "$destdir" ]; then
        if [ "$source_tree" = no ] && [ "$(id -u)" -eq 0 ]; then trusted_root_path "$loader"; fi
        "$loader" --help >/dev/null || fail 'loader cannot run; rebuild on this Linux machine'
    fi
}
become_root()
{
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 || fail 'sudo is unavailable; build as your normal user before invoking this helper as root'
        exec sudo env PREFIX="$prefix" DESTDIR="$destdir" "$script" "$command" "$@"
    fi
}
case $command in
    install|uninstall)
        if [ -z "$destdir" ]; then
            # Removing installed files must still work after booting an unsupported kernel.
            if [ "$command" = uninstall ] && [ -x "$prefix/sbin/scx_lertick" ]; then
                loader=$prefix/sbin/scx_lertick
            else
                ensure_loader
            fi
            become_root
            trusted_installation "$prefix"
            trusted_root_path "$prefix/sbin/scx_lertick"
            "$loader" --maintenance -- "$script" "$command-locked" || fail 'maintenance failed or a scheduler/controller is active; installed files were kept'
            exit 0
        fi ;;
    install-locked|uninstall-locked)
        [ "$(id -u)" -eq 0 ] && [ -z "$destdir" ] || fail 'internal maintenance commands require root and no DESTDIR' ;;
esac
case $command in
    install|install-locked)
        ensure_loader
        target=$destdir$prefix
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        for path in "$target/bin/lertick" "$target/sbin/scx_lertick"; do
            if [ -L "$path" ] || { [ -e "$path" ] && [ ! -f "$path" ]; }; then fail "refusing non-regular install target: $path"; fi
        done
        umask 022
        mkdir -p "$target/bin" "$target/sbin"
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        loader_tmp=$(mktemp "$target/sbin/.scx_lertick.XXXXXX")
        helper_tmp=$(mktemp "$target/bin/.lertick.XXXXXX") || { rm -f "$loader_tmp"; exit 1; }
        trap 'rm -f "$loader_tmp" "$helper_tmp"' 0
        cp "$loader" "$loader_tmp"; cp "$script" "$helper_tmp"
        chmod 755 "$loader_tmp" "$helper_tmp"
        mv -f "$loader_tmp" "$target/sbin/scx_lertick"
        mv -f "$helper_tmp" "$target/bin/lertick"
        trap - 0
        printf 'Installed %s and %s. No service was started or enabled.\n' "$target/bin/lertick" "$target/sbin/scx_lertick"
        exit 0 ;;
    uninstall|uninstall-locked)
        target=$destdir$prefix
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        rm -f "$target/bin/lertick" "$target/sbin/scx_lertick"
        printf 'Removed helper and loader from %s; telemetry was kept.\n' "$target"
        exit 0 ;;
esac
scheduler_ready
ensure_loader
become_root "$@"
scheduler_ready
case $command in
    check) "$loader" --check >/dev/null; printf 'Lertick: basic kernel/permission checks passed; no scheduler attached.\n'; exit 0 ;;
    verify) "$loader" --verify >/dev/null; printf 'Lertick: BPF verifier accepted; no scheduler attached.\n'; exit 0 ;;
    run) ;;
esac
"$loader" --check >/dev/null || fail 'kernel/permission check failed; no trial started'
child=; pending_signal=; pending_status=0
finish_trial()
{
    trial_status=$1
    trap '' INT TERM HUP
    printf 'Lertick trial ended (status %s).\n' "$trial_status" >&2
    show_status >&2 || trial_status=1
    if [ "${state:-unavailable}" != disabled ]; then
        printf 'lertick: default scheduling could not be confirmed; no other scheduler will be aborted.\n' >&2
        if [ "$trial_status" -eq 0 ]; then trial_status=1; fi
    fi
    exit "$trial_status"
}
stop_trial()
{
    signal_status=$1
    if [ -z "$child" ]; then pending_signal=yes; pending_status=$signal_status; return; fi
    trap '' INT TERM HUP
    kill -TERM "$child" 2>/dev/null || :
    wait "$child" 2>/dev/null || :
    child=
    finish_trial "$signal_status"
}
trap 'stop_trial 130' INT
trap 'stop_trial 143' TERM
trap 'stop_trial 129' HUP
printf 'Lertick: starting a 30-second foreground trial (unless overridden); Ctrl-C stops this child.\n' >&2
[ -z "$pending_signal" ] || finish_trial "$pending_status"
if [ "$stats_requested" = yes ]; then
    "$loader" --mode online --duration 30 --track-all --cpuperf auto "$@" &
else
    "$loader" --mode online --duration 30 --track-all --cpuperf auto "$@" >/dev/null &
fi
child=$!
[ -z "$pending_signal" ] || stop_trial "$pending_status"
if wait "$child"; then trial_status=0; else trial_status=$?; fi
child=
finish_trial "$trial_status"
