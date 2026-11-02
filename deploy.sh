#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Native foreground/service deployment. Never stop a foreign scheduler.
set -eu
fail() { printf 'lertick: %s\n' "$*" >&2; exit 1; }
usage()
{
    cat <<'HELP'
Usage: ./deploy.sh COMMAND [OPTIONS]
       lertick COMMAND [OPTIONS]          (after installation)

  check       Basic kernel/permission checks; never attach.
  verify      Verify BPF without attaching/pinning; safe while a scheduler runs.
  plan        Print a run command only; never build, sudo, load or attach.
  run         Foreground online scheduler until Ctrl-C (default duration 0).
  trial       Foreground online trial (default 30 seconds).
  install     Build as normal user; install files + service without enabling it.
  start       Start the installed systemd service until explicitly stopped.
  stop        Stop only this service and confirm Linux default scheduling.
  restart     Restart this service; never stop another scheduler.
  enable      Start now and enable this service at boot.
  disable     Stop now and disable this service at boot.
  update      Build/verify a candidate before stopping/replacing this service.
  rollback    Restore the previous complete release, preserving service intent.
  uninstall   Disable/stop this service and remove its installed files.
  status      Read scheduler, service and installed release status.
  version     Print the installed/built loader version.
  report      Print a local support report JSON; optional --include-logs.
  help        Show this help.

plan/run/trial options:
  --duration SECONDS        0..3600; 0 means until stopped
  --mode MODE               fixed|shadow|online|ewma (default online)
  --sample-ms MILLISECONDS   100..60000 (indefinite default 60000; finite 1000)
  --stats-json FILE          New telemetry file, or - for stdout

Indefinite runs use aggregate summaries; finite trials retain detailed samples.
Shadow is an ACTIVE fixed-slice scheduler. Verify loads temporary BPF objects;
plan only prints. The helper cannot replace another active scheduler.
Native builds may download pinned libbpf as your normal user, never as root.
Installed update shallow-clones https://github.com/Denny-Lin/lertick.git main
into a private temporary checkout as your normal user, then builds/verifies it.
Source update uses this checkout; it does not git pull or change branches.
A compatible running sched_ext kernel is required; booting one may need reboot.
Systemd is required for service commands; otherwise use foreground run.
PREFIX defaults to /usr/local. DESTDIR supports install/uninstall staging only,
with no sudo, BPF execution or service operations. Install never enables service.
HELP
}
case $0 in */*) script=$0 ;; *) script=$(command -v "$0") || fail 'cannot locate helper' ;; esac
tool_dir=$(CDPATH= cd -P "$(dirname "$script")" && pwd)
script=$tool_dir/$(basename "$script")
source_tree=no
if [ -f "$tool_dir/Makefile" ] && [ -f "$tool_dir/src/userspace/scx_lertick.c" ]; then
    source_tree=yes; loader=$tool_dir/build/scx_lertick; prefix=${PREFIX:-/usr/local}
    manager=$tool_dir/scripts/service_manager.sh; report=$tool_dir/scripts/collect_report.py
else
    installed_prefix=$(CDPATH= cd -P "$tool_dir/.." && pwd)
    prefix=${PREFIX:-$installed_prefix}; loader=$installed_prefix/sbin/scx_lertick
    manager=$installed_prefix/libexec/lertick/service_manager.sh; report=$installed_prefix/libexec/lertick/collect_report.py
fi
destdir=${DESTDIR:-}; command=${1:-help}
if [ "$#" -gt 0 ]; then shift; fi
case $command in
    help|--help|-h) usage; exit 0 ;;
    check|verify|plan|run|trial|install|uninstall|status|version|report|start|stop|restart|enable|disable|update|rollback) ;;
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
stats_requested=no; duration=0
[ "$command" != trial ] || duration=30
validate_value()
{
    [ -n "$2" ] || fail "$1 needs a value"
    case $1 in
        --mode) case $2 in fixed|shadow|online|ewma) ;; *) fail 'invalid mode' ;; esac ;;
        --duration|--sample-ms)
            case $2 in *[!0-9]*) fail "$1 must be an integer" ;; esac
            if [ "$1" = --duration ]; then low=0; high=3600; else low=100; high=60000; fi
            [ "$2" -ge "$low" ] 2>/dev/null && [ "$2" -le "$high" ] 2>/dev/null || fail "$1 must be $low..$high"
            if [ "$1" = --duration ]; then duration=$2; fi ;;
        --stats-json) stats_requested=yes ;;
    esac
}
validate_options()
{
    while [ "$#" -gt 0 ]; do
        case $1 in
            --duration|--mode|--sample-ms|--stats-json) [ "$#" -ge 2 ] || fail "$1 needs a value"; validate_value "$1" "$2"; shift 2 ;;
            --duration=*|--mode=*|--sample-ms=*|--stats-json=*) validate_value "${1%%=*}" "${1#*=}"; shift ;;
            *) fail "unsupported option: $1 (fault injection and scheduler replacement are unavailable here)" ;;
        esac
    done
}
case $command in
    plan|run|trial) validate_options "$@" ;;
    report) for arg in "$@"; do case $arg in --include-logs|--help) ;; *) fail "unsupported report option: $arg" ;; esac; done ;;
    *) [ "$#" -eq 0 ] || fail "$command does not accept options" ;;
esac
if [ "$command" = plan ]; then
    printf 'Run plan (not executed; requires a compatible idle sched_ext kernel):\n'
    if [ "$duration" -eq 0 ]; then set -- --summary-only --sample-ms 60000 "$@"; fi
    for word in sudo "$loader" --mode online --duration 0 --track-all --cpuperf auto "$@"; do
        escaped=$(printf '%s' "$word" | sed "s/'/'\\\\''/g"); printf " '%s'" "$escaped"
    done
    printf '\nNo scheduler was loaded or attached.\n'; exit 0
fi
if [ "$command" = report ]; then
    [ -f "$report" ] && [ ! -L "$report" ] || fail 'report tool is unavailable; install/update Lertick first'
    exec python3 "$report" --prefix "$prefix" "$@"
fi
[ "$(uname -s)" = Linux ] || fail 'native deployment requires Linux'
trusted_root_path()
{
    trust_remaining=${1#/}; trust_path=/
    while :; do
        [ ! -L "$trust_path" ] || fail "symlink in privileged path: $trust_path"
        if [ -e "$trust_path" ]; then
            trust_stat=$(stat -c '%u %a' "$trust_path") || fail "cannot inspect: $trust_path"
            set -- $trust_stat
            [ "$#" -eq 2 ] && [ "$1" = 0 ] || fail "path must be root-owned: $trust_path"
            case $2 in ''|*[!0-7]*) fail "invalid permissions: $trust_path" ;; esac
            [ "$((0$2 & 022))" -eq 0 ] || fail "path must not be group/other writable: $trust_path"
            if [ -n "$trust_remaining" ]; then [ -d "$trust_path" ] || fail "ancestor is not a directory: $trust_path"; fi
        fi
        [ -n "$trust_remaining" ] || break
        trust_component=${trust_remaining%%/*}
        case $trust_remaining in */*) trust_remaining=${trust_remaining#*/} ;; *) trust_remaining= ;; esac
        [ -n "$trust_component" ] || continue
        trust_path=${trust_path%/}/$trust_component
    done
}
scheduler_ready()
{
    state=$(cat /sys/kernel/sched_ext/state 2>/dev/null) || fail 'sched_ext unavailable; boot a compatible kernel (reboot may be required)'
    [ "$state" = disabled ] || fail "NOT READY: sched_ext state=$state; stop its scheduler through its own manager first"
}
show_status()
{
    if ! state=$(cat /sys/kernel/sched_ext/state 2>/dev/null); then printf 'sched_ext: unavailable\n'; return 1; fi
    printf 'sched_ext: %s\n' "$state"
    if ops=$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null); then printf 'scheduler: %s\n' "$ops"; fi
}
if [ "$command" = status ]; then
    status=0; show_status || status=$?
    if [ -x "$manager" ]; then "$manager" "$prefix" status; fi
    exit "$status"
fi
if [ "$command" = version ]; then [ -x "$loader" ] || fail 'loader unavailable; build/install first'; exec "$loader" --version; fi
ensure_loader()
{
    if [ "$source_tree" = yes ] && [ "$(id -u)" -ne 0 ]; then
        if [ "$command" = update ] || [ "$command" = verify ]; then "$tool_dir/scripts/native_build.sh" --allow-active >&2; else "$tool_dir/scripts/native_build.sh" >&2; fi
    fi
    [ -f "$loader" ] && [ -x "$loader" ] && [ ! -L "$loader" ] || fail 'loader unavailable; build as your normal user first'
    if [ -z "$destdir" ]; then
        if [ "$source_tree" = no ] && [ "$(id -u)" -eq 0 ]; then trusted_root_path "$loader"; fi
        "$loader" --help >/dev/null || fail 'loader cannot run; rebuild on this Linux machine'
    fi
}
become_root()
{
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 || fail 'sudo unavailable; build as normal user before running this helper as root'
        exec sudo env PREFIX="$prefix" DESTDIR="$destdir" "$script" "$command" "$@"
    fi
}
if [ "$command" = update ] && [ "$source_tree" = no ]; then
    [ "$(id -u)" -ne 0 ] || fail 'run installed update as your normal user; downloads/builds never run as root'
    update_checkout=$(mktemp -d "${TMPDIR:-/tmp}/lertick-update.XXXXXX")
    trap 'rm -rf "$update_checkout"' 0
    printf 'Lertick: downloading GitHub main to private checkout %s as your normal user.\n' "$update_checkout" >&2
    git clone --depth 1 --branch main https://github.com/Denny-Lin/lertick.git "$update_checkout/source"
    PREFIX="$prefix" "$update_checkout/source/deploy.sh" update
    exit $?
fi
case $command in
    install|update|uninstall|rollback|start|stop|restart|enable|disable)
        case $command in
            install|update)
                if [ "$source_tree" = yes ] && [ "$(id -u)" -eq 0 ]; then
                    # Reject untrusted prebuilt bundles before executing even --help.
                    # Normal users still build first and pass exact bytes to sudo staging.
                    for candidate_file in "$loader" "$script" "$manager" "$tool_dir/templates/lertick.service.in" "$report"; do
                        [ -f "$candidate_file" ] && [ ! -L "$candidate_file" ] || fail "missing/non-regular candidate file: $candidate_file"
                        trusted_root_path "$candidate_file"
                    done
                fi
                ;;
        esac
        if [ -n "$destdir" ]; then
            if [ "$command" = uninstall ]; then
                stage_target=$destdir$prefix
                for path in "$stage_target/bin/lertick" "$stage_target/sbin/scx_lertick" "$stage_target/libexec/lertick/service_manager.sh" "$stage_target/libexec/lertick/collect_report.py" "$destdir/etc/systemd/system/lertick.service"; do
                    if [ "$(id -u)" -eq 0 ]; then trusted_root_path "$path"; fi
                    [ ! -L "$path" ] || fail "symlink staging target: $path"; rm -f "$path"
                done
                exit 0
            fi
            ensure_loader
            exec "$manager" "$prefix" install "$tool_dir" "$destdir"
        fi
        case $command in
            install|update)
                [ "$source_tree" = yes ] || fail 'install/update requires a source checkout'
                ensure_loader
                if [ "$(id -u)" -ne 0 ]; then
                    command -v sudo >/dev/null 2>&1 || fail 'sudo is unavailable'
                    # Build/download stays unprivileged; root only stages/verifies exact copied bytes.
                    exec sudo "$manager" "$prefix" "$command" "$tool_dir"
                fi
                ;;
        esac
        become_root
        [ -x "$manager" ] && [ ! -L "$manager" ] || fail 'service manager unavailable; install/update first'
        if [ "$source_tree" = no ]; then trusted_root_path "$manager"; fi
        exec "$manager" "$prefix" "$command" "$tool_dir"
        ;;
esac
# Verifier-only load is deliberately permitted alongside an active scheduler.
if [ "$command" != verify ]; then scheduler_ready; fi
ensure_loader
become_root "$@"
if [ "$command" != verify ]; then scheduler_ready; fi
case $command in
    check) "$loader" --check >/dev/null; printf 'Lertick: basic checks passed; no scheduler attached.\n'; exit 0 ;;
    verify) "$loader" --verify >/dev/null; printf 'Lertick: BPF verifier accepted; no scheduler attached.\n'; exit 0 ;;
    run|trial) ;;
esac
"$loader" --check >/dev/null || fail 'kernel/permission check failed; no scheduler started'
child=; pending_signal=; pending_status=0
finish_run()
{
    run_status=$1; trap '' INT TERM HUP
    printf 'Lertick foreground run ended (status %s).\n' "$run_status" >&2
    show_status >&2 || run_status=1
    if [ "${state:-unavailable}" != disabled ]; then
        printf 'lertick: default scheduling could not be confirmed; no other scheduler will be aborted.\n' >&2
        [ "$run_status" -ne 0 ] || run_status=1
    fi
    exit "$run_status"
}
stop_run()
{
    signal_status=$1
    if [ -z "$child" ]; then pending_signal=yes; pending_status=$signal_status; return; fi
    trap '' INT TERM HUP
    kill -TERM "$child" 2>/dev/null || :; wait "$child" 2>/dev/null || :; child=
    finish_run "$signal_status"
}
trap 'stop_run 130' INT
trap 'stop_run 143' TERM
trap 'stop_run 129' HUP
base_duration=0; [ "$command" != trial ] || base_duration=30
if [ "$duration" -eq 0 ]; then
    set -- --summary-only --sample-ms 60000 "$@"
    printf 'Lertick: starting foreground scheduler until stopped; Ctrl-C detaches it.\n' >&2
else printf 'Lertick: starting finite foreground scheduler; Ctrl-C detaches it.\n' >&2; fi
[ -z "$pending_signal" ] || finish_run "$pending_status"
if [ "$stats_requested" = yes ]; then
    "$loader" --mode online --duration "$base_duration" --track-all --cpuperf auto "$@" &
else
    "$loader" --mode online --duration "$base_duration" --track-all --cpuperf auto "$@" >/dev/null &
fi
child=$!
[ -z "$pending_signal" ] || stop_run "$pending_status"
if wait "$child"; then run_status=0; else run_status=$?; fi
child=
finish_run "$run_status"
