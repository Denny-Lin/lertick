#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Root-owned, immutable release bundles; only this service is ever stopped.
set -eu
fail() { printf 'lertick: %s\n' "$*" >&2; exit 1; }
prefix=${1:?prefix}; command=${2:?command}; bundle=${3:-}; destdir=${4:-}
case $prefix in /*) ;; *) fail 'PREFIX must be absolute' ;; esac
case $prefix in /|*/../*|*/./*|*/..|*/.) fail 'service PREFIX must not contain . or .. components' ;; esac
if [ "$command" != status ]; then
    case $prefix in *[!a-zA-Z0-9_./-]*) fail 'service PREFIX must use only letters, digits, _, /, . and -' ;; esac
fi
prefix=${prefix%/}
unit_dir=/etc/systemd/system
lock_dir=/run/lertick-deploy
unit=$unit_dir/lertick.service
store=$prefix/lib/lertick
releases=$store/releases
loader=$prefix/sbin/scx_lertick
stage=
cleanup() { [ -z "$stage" ] || rm -rf "$stage"; }
trap cleanup 0
trusted_path()
{
    remain=${1#/}; current=/
    while :; do
        [ ! -L "$current" ] || fail "symlink in privileged path: $current"
        if [ -e "$current" ]; then
            info=$(stat -c '%u %a' "$current") || fail "cannot inspect $current"
            set -- $info
            [ "$#" -eq 2 ] && [ "$1" = 0 ] || fail "path must be root-owned: $current"
            case $2 in ''|*[!0-7]*) fail "invalid permissions: $current" ;; esac
            [ "$((0$2 & 022))" -eq 0 ] || fail "path must not be group/other writable: $current"
            if [ -n "$remain" ]; then [ -d "$current" ] || fail "ancestor is not a directory: $current"; fi
        fi
        [ -n "$remain" ] || break
        part=${remain%%/*}
        case $remain in */*) remain=${remain#*/} ;; *) remain= ;; esac
        [ -n "$part" ] || continue
        current=${current%/}/$part
    done
}
have_systemd() { command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; }
state_read() { state=$(cat /sys/kernel/sched_ext/state 2>/dev/null) || state=unavailable; }
owned_unit()
{
    [ -f "$unit" ] && [ ! -L "$unit" ] || return 1
    grep -Fx "# Lertick managed unit; prefix=$prefix" "$unit" >/dev/null
}
assert_unit_owner()
{
    trusted_path "$unit"
    if [ -e "$unit" ]; then
        owned_unit || fail 'existing lertick.service belongs to another installation; kept unchanged'
    elif have_systemd; then
        fragment=$(systemctl show --property=FragmentPath --value lertick.service 2>/dev/null || :)
        [ -z "$fragment" ] || fail "another lertick.service already exists at $fragment; kept unchanged"
    fi
}
require_service()
{
    have_systemd || fail 'systemd is not running; use lertick run in the foreground'
    owned_unit || fail 'this prefix has no installed Lertick service; install it first'
}
service_active() { systemctl is-active --quiet lertick.service; }
service_enabled() { systemctl is-enabled --quiet lertick.service; }
check_start_safe()
{
    state_read
    if [ "$state" != disabled ]; then
        if service_active && [ "$state" = enabled ] && [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" = scx_lertick ]; then return; fi
        fail "sched_ext state=$state; another scheduler or foreground controller is active; stop it through its own manager"
    fi
}
check_default()
{
    state_read
    [ "$state" = disabled ] || fail "service stopped, but default scheduling is not confirmed (sched_ext=$state); no other scheduler was stopped"
    printf 'Lertick stopped; Linux default scheduling confirmed.\n'
}
check_running()
{
    service_active || return 1
    state_read
    [ "$state" = enabled ] && [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" = scx_lertick ]
}
atomic_copy()
{
    copy_source=$1; copy_target=$2; copy_permissions=$3
    if [ "$(id -u)" -eq 0 ]; then trusted_path "$copy_target"; else [ ! -L "$copy_target" ] || fail "symlink target: $copy_target"; fi
    [ ! -e "$copy_target" ] || [ -f "$copy_target" ] || fail "non-regular target: $copy_target"
    copy_temporary=$(mktemp "$(dirname "$copy_target")/.lertick.XXXXXX")
    if ! cp "$copy_source" "$copy_temporary" || ! chmod "$copy_permissions" "$copy_temporary" || ! mv -f "$copy_temporary" "$copy_target"; then
        rm -f "$copy_temporary"; return 1
    fi
}

write_marker()
{
    marker_tmp=$(mktemp "$store/.revision.XXXXXX")
    printf '%s\n' "$2" > "$marker_tmp"; chmod 644 "$marker_tmp"; mv -f "$marker_tmp" "$store/$1"
}
read_marker()
{
    [ -f "$store/$1" ] && [ ! -L "$store/$1" ] || return 1
    revision=$(cat "$store/$1")
    case $revision in ''|*[!a-f0-9]*) return 1 ;; esac
    [ "${#revision}" -eq 64 ] || return 1
    printf '%s\n' "$revision"
}
validate_release()
{
    release=$releases/$1
    trusted_path "$release"
    [ -d "$release" ] && [ -f "$release/manifest.sha256" ] && [ ! -L "$release/manifest.sha256" ] || return 1
    # Fixed filenames only: never run sha256sum on an untrusted manifest path list.
    for file in scx_lertick lertick service_manager.sh lertick.service; do
        [ -f "$release/$file" ] && [ ! -L "$release/$file" ] || return 1
    done
    expected=$(cd "$release" && sha256sum manifest.sha256); expected=${expected%% *}
    [ "$expected" = "$1" ] || return 1
    actual=$(cd "$release" && sha256sum scx_lertick lertick service_manager.sh lertick.service)
    if [ -e "$release/collect_report.py" ]; then
        [ -f "$release/collect_report.py" ] && [ ! -L "$release/collect_report.py" ] || return 1
        report_hash=$(cd "$release" && sha256sum collect_report.py)
        actual="$actual
$report_hash"
    fi
    [ "$actual" = "$(cat "$release/manifest.sha256")" ]
}
# Called only under the loader's separate controller/maintenance lock.
if [ "$command" = promote-locked ]; then
    [ "$(id -u)" -eq 0 ] && [ -z "$destdir" ] || fail 'promotion requires root'
    case $bundle in ''|*[!a-f0-9]*) fail 'invalid revision' ;; esac
    validate_release "$bundle" || fail 'release validation failed'
    assert_unit_owner
    mkdir -p "$prefix/bin" "$prefix/sbin" "$prefix/libexec/lertick" "$unit_dir"
    atomic_copy "$release/scx_lertick" "$loader" 755
    atomic_copy "$release/lertick" "$prefix/bin/lertick" 755
    atomic_copy "$release/service_manager.sh" "$prefix/libexec/lertick/service_manager.sh" 755
    atomic_copy "$release/lertick.service" "$unit" 644
    if [ -f "$release/collect_report.py" ]; then
        atomic_copy "$release/collect_report.py" "$prefix/libexec/lertick/collect_report.py" 555
    else
        trusted_path "$prefix/libexec/lertick/collect_report.py"
        rm -f "$prefix/libexec/lertick/collect_report.py"
    fi
    previous=$(read_marker current || :)
    if [ -n "$previous" ] && [ "$previous" != "$bundle" ]; then write_marker previous "$previous"; fi
    write_marker current "$bundle"
    exit 0
fi
if [ "$command" = uninstall-locked ]; then
    [ "$(id -u)" -eq 0 ] && [ -z "$destdir" ] || fail 'uninstall requires root'
    owned_unit || fail 'service ownership changed; files kept'
    for path in "$loader" "$prefix/bin/lertick" "$prefix/libexec/lertick/service_manager.sh" "$prefix/libexec/lertick/collect_report.py" "$unit" "$store"; do trusted_path "$path"; done
    rm -f "$loader" "$prefix/bin/lertick" "$prefix/libexec/lertick/service_manager.sh" "$prefix/libexec/lertick/collect_report.py" "$unit"
    rm -rf "$store"
    printf 'Removed Lertick files and service unit; user telemetry was kept.\n'
    exit 0
fi
if [ "$command" = status ]; then
    if have_systemd; then systemctl show --property=ActiveState,SubState,UnitFileState,Result lertick.service 2>/dev/null || :; fi
    for marker in current previous; do value=$(read_marker "$marker" || :); [ -z "$value" ] || printf '%s: %s\n' "$marker" "$value"; done
    exit 0
fi
if [ -n "$destdir" ]; then
    [ "$command" = install ] || fail 'DESTDIR only supports install staging'
    [ -n "$bundle" ] || fail 'staging requires a source checkout'
    # Staging deliberately does not create a release, touch systemd or execute BPF.
    target=$destdir$prefix; stage_unit=$destdir$unit_dir/lertick.service
    if [ "$(id -u)" -eq 0 ]; then trusted_path "$target/bin"; trusted_path "$target/sbin"; trusted_path "$target/libexec/lertick"; trusted_path "$stage_unit"; fi
    mkdir -p "$target/bin" "$target/sbin" "$target/libexec/lertick" "$(dirname "$stage_unit")"
    for pair in "build/scx_lertick:sbin/scx_lertick" "deploy.sh:bin/lertick" "scripts/service_manager.sh:libexec/lertick/service_manager.sh"; do
        from=${pair%%:*}; to=${pair#*:}; atomic_copy "$bundle/$from" "$target/$to" 755
    done
    [ ! -L "$stage_unit" ] || fail 'symlink staging unit'
    if [ -e "$stage_unit" ]; then grep -Fx "# Lertick managed unit; prefix=$prefix" "$stage_unit" >/dev/null || fail 'unknown staged unit; kept unchanged'; fi
    stage=$(mktemp -d "$destdir/.lertick-stage.XXXXXX")
    sed "s|@PREFIX@|$prefix|g" "$bundle/templates/lertick.service.in" > "$stage/unit"
    atomic_copy "$stage/unit" "$stage_unit" 644
    if [ -f "$bundle/scripts/collect_report.py" ]; then atomic_copy "$bundle/scripts/collect_report.py" "$target/libexec/lertick/collect_report.py" 555; fi
    printf 'Staged files and service unit under %s; no service operation.\n' "$destdir"
    exit 0
fi
[ "$(id -u)" -eq 0 ] || fail 'service/file management requires root'
umask 022
trusted_path "$prefix/bin"; trusted_path "$prefix/sbin"; trusted_path "$store"; trusted_path "$prefix/libexec/lertick"
trusted_path "$lock_dir"
mkdir -p "$lock_dir"; chmod 700 "$lock_dir"
trusted_path "$lock_dir/transaction.lock"
exec 9>"$lock_dir/transaction.lock"
flock -n 9 || fail 'another Lertick deployment transaction is running'
assert_unit_owner
case $command in
    start|restart|enable)
        require_service; check_start_safe
        # An explicit user retry may clear systemd's start-rate limit; automatic
        # update/recovery paths intentionally never reset or retry in a loop.
        systemctl reset-failed lertick.service
        case $command in start) systemctl start lertick.service ;; restart) systemctl restart lertick.service ;; enable) systemctl enable --now lertick.service ;; esac
        check_running || fail 'service did not confirm ready/attached; inspect lertick status and journalctl -u lertick'
        printf 'Lertick is ready and running.\n'; exit 0 ;;
    stop|disable)
        require_service
        if [ "$command" = stop ]; then systemctl stop lertick.service; else systemctl disable --now lertick.service; fi
        check_default; exit 0 ;;
    uninstall)
        if owned_unit && have_systemd; then systemctl disable --now lertick.service; fi
        state_read
        case $state in disabled|unavailable) ;; *) fail 'another scheduler is active; installed files kept' ;; esac
        [ -x "$loader" ] || fail 'installed loader unavailable; installed files kept'
        "$loader" --maintenance -- "$0" "$prefix" uninstall-locked
        if have_systemd; then systemctl daemon-reload; fi
        exit 0 ;;
    uninstall-locked)
        # This case must be dispatched before taking this transaction lock again.
        fail 'internal uninstall dispatch failed' ;;
    install|update|rollback) ;;
    *) fail "unknown service command: $command" ;;
esac
old_revision=$(read_marker current || :)
old_previous=$(read_marker previous || :)
was_active=no; was_enabled=no
if have_systemd && owned_unit; then
    if service_active; then was_active=yes; fi
    if service_enabled; then was_enabled=yes; fi
fi
state_read
if [ "$state" != disabled ]; then
    [ "$was_active" = yes ] && [ "$state" = enabled ] && [ "$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)" = scx_lertick ] ||
        fail "sched_ext state=$state; foreground or foreign scheduler active; installed release kept"
fi
if [ "$command" = install ]; then
    [ -z "$old_revision" ] || fail 'Lertick is already installed; use update'
    [ "$was_active" = no ] || fail 'service is active; use update'
fi
if [ "$command" = rollback ]; then
    candidate=$(read_marker previous || :)
    [ -n "$candidate" ] && [ "$candidate" != "$old_revision" ] || fail 'no different previous release is available'
    validate_release "$candidate" || fail 'previous release is invalid; installed release kept'
else
    [ -n "$bundle" ] || fail 'a prebuilt source checkout is required'
    trusted_path "$releases"
    mkdir -p "$releases"
    stage=$(mktemp -d "$store/.candidate.XXXXXX")
    chmod 700 "$stage"
    for pair in "build/scx_lertick:scx_lertick" "deploy.sh:lertick" "scripts/service_manager.sh:service_manager.sh"; do
        from=${pair%%:*}; to=${pair#*:}
        [ -f "$bundle/$from" ] && [ ! -L "$bundle/$from" ] || fail "missing/non-regular candidate file: $from"
        cp "$bundle/$from" "$stage/$to"; chmod 555 "$stage/$to"
    done
    sed "s|@PREFIX@|$prefix|g" "$bundle/templates/lertick.service.in" > "$stage/lertick.service"; chmod 444 "$stage/lertick.service"
    [ -f "$bundle/scripts/collect_report.py" ] && [ ! -L "$bundle/scripts/collect_report.py" ] || fail 'candidate report tool is missing/non-regular'
    cp "$bundle/scripts/collect_report.py" "$stage/collect_report.py"; chmod 555 "$stage/collect_report.py"
    (cd "$stage" && sha256sum scx_lertick lertick service_manager.sh lertick.service > manifest.sha256)
    if [ -f "$stage/collect_report.py" ]; then (cd "$stage" && sha256sum collect_report.py >> manifest.sha256); fi
    digest=$(sha256sum "$stage/manifest.sha256"); candidate=${digest%% *}
    if [ -e "$releases/$candidate" ]; then
        validate_release "$candidate" || fail 'existing candidate release is invalid'
        rm -rf "$stage"; stage=
    else
        chmod 444 "$stage/manifest.sha256"; chmod 755 "$stage"
        mv "$stage" "$releases/$candidate"; stage=
    fi
fi
candidate_loader=$releases/$candidate/scx_lertick
transaction_manager=$releases/$candidate/service_manager.sh
# Verifier-only path never attaches, pins, takes the controller lock or stops old service.
"$candidate_loader" --verify --mode online --track-all --cpuperf auto --summary-only >/dev/null || fail 'candidate verification failed; current service and release kept'
if [ "$candidate" = "$old_revision" ]; then printf 'Already at release %s; no restart.\n' "$candidate"; exit 0; fi
# Pin maintenance to the known-good immutable loader before stopping anything.
# The live sbin path will be overwritten and must never become our recovery tool.
promoter=$candidate_loader
if [ -n "$old_revision" ]; then
    validate_release "$old_revision" || fail 'installed release is invalid; current service kept'
    promoter=$releases/$old_revision/scx_lertick
fi
if [ "$was_active" = yes ]; then systemctl stop lertick.service || fail 'could not stop this service; release kept'; fi
state_read
[ "$state" = disabled ] || fail 'sched_ext did not become disabled; no files promoted and no other scheduler stopped'
# Keep transaction lock, but release the controller maintenance lock before starting.
promote()
{
    "$promoter" --maintenance -- "$transaction_manager" "$prefix" promote-locked "$1"
}
rollback_once()
{
    printf 'Lertick update failed; attempting previous bundle once.\n' >&2
    if [ -z "$old_revision" ]; then return 1; fi
    # Stop only this owned unit; never act on a foreign/global scheduler PID.
    if have_systemd; then systemctl stop lertick.service || return 1; fi
    state_read; [ "$state" = disabled ] || return 1
    promote "$old_revision" || return 1
    # Automatic recovery must not advertise the known-bad candidate as rollback.
    if [ -n "$old_previous" ]; then write_marker previous "$old_previous"; else rm -f "$store/previous"; fi
    if have_systemd; then systemctl daemon-reload || return 1; fi
    if [ "$was_active" = yes ]; then systemctl start lertick.service && check_running || return 1; fi
    printf 'Previous bundle restored; enabled state kept (%s).\n' "$was_enabled" >&2
}
if ! promote "$candidate"; then rollback_once || printf 'Rollback unavailable/failed; Linux default scheduling remains the intended fallback.\n' >&2; exit 1; fi
if have_systemd; then
    if ! systemctl daemon-reload; then rollback_once || :; exit 1; fi
    if [ "$was_active" = yes ]; then
        if ! systemctl start lertick.service || ! check_running; then rollback_once || printf 'Rollback failed; automatic retries are disabled. Inspect service status.\n' >&2; exit 1; fi
    fi
fi
printf 'Installed release %s; active=%s enabled=%s.\n' "$candidate" "$was_active" "$was_enabled"
if [ "$command" = install ]; then printf 'Service installed without starting/enabling it. Run lertick enable to start now and at boot.\n'; fi
