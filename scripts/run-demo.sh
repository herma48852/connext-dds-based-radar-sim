#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

build_dir=""
connext_dir="${CONNEXTDDS_DIR:-${NDDSHOME:-}}"
domain=92
control_domain=""
targets=32
run_seconds=0
headless=0
start_target_control=0

radar_pid=""
target_pid=""
control_pid=""
radar_status=""
target_status=""
control_status=""
cleanup_started=0
stop_file=""
log_dir=""
session_lock_dir=""

usage() {
    cat <<'EOF'
Usage: scripts/run-demo.sh [options]

Launch radar_app and target_gen together with cooperative shutdown and logs.
Use scripts/start-all.sh for the complete interactive three-process demo.

Options:
  --build-dir PATH    CMake build directory (auto-detected by default)
  --connext-dir PATH  RTI Connext DDS installation (uses CONNEXTDDS_DIR/NDDSHOME)
  --domain N          DDS domain, 0..232 (default: 92)
  --control-domain N  Target-control domain (default: simulation domain + 1)
  --targets N         Total targets: baseline plus N-1 randomized (default: 32)
  --run-seconds N     Stop after N seconds; 0 runs until window close/Ctrl-C
  --headless          Run radar_app without a window
  --target-control    Also start target_control (normally use start-all.sh)
  -h, --help          Show this help

Examples:
  ./scripts/run-demo.sh
  ./scripts/run-demo.sh --domain 92 --targets 32
  ./scripts/run-demo.sh --headless --run-seconds 20
EOF
}

die() {
    echo "error: $*" >&2
    exit 2
}

require_unsigned() {
    local name="$1"
    local value="$2"
    case "$value" in
        ""|*[!0-9]*) die "$name must be an unsigned integer" ;;
    esac
}

conflicting_demo_pids() {
    ps -axo pid=,command= 2>/dev/null |
        awk -v domain="$domain" -v control_domain="$control_domain" \
            -v include_control="$start_target_control" '
        {
            executable = $2
            sub(/^.*\//, "", executable)
            if (executable == "radar_app" || executable == "target_gen")
                expected_domain = domain
            else if (include_control == 1 && executable == "target_control")
                expected_domain = control_domain
            else
                next
            for (i = 3; i < NF; ++i) {
                if ($i == "--domain" && $(i + 1) == expected_domain) {
                    print $1
                    break
                }
            }
        }
    '
}

release_session_lock() {
    [[ -n "$session_lock_dir" ]] || return 0

    local owner=""
    if [[ -f "$session_lock_dir/launcher.pid" ]]; then
        IFS= read -r owner < "$session_lock_dir/launcher.pid" || true
    fi
    if [[ "$owner" == "$$" ]]; then
        rm -f "$session_lock_dir/launcher.pid"
        rmdir "$session_lock_dir" 2>/dev/null || true
    fi
    session_lock_dir=""
}

acquire_session_lock() {
    local lock_root="${TMPDIR:-/tmp}/aesa-radar-demo-$(id -u)"
    session_lock_dir="$lock_root/domain-$domain.lock"
    mkdir -p "$lock_root"

    if ! mkdir "$session_lock_dir" 2>/dev/null; then
        local owner=""
        if [[ -f "$session_lock_dir/launcher.pid" ]]; then
            IFS= read -r owner < "$session_lock_dir/launcher.pid" || true
        fi
        if [[ "$owner" =~ ^[0-9]+$ ]] && kill -0 "$owner" 2>/dev/null; then
            die "another demo launcher owns DDS domain $domain (PID $owner)"
        fi

        # The previous launcher died without running its EXIT trap.
        rm -f "$session_lock_dir/launcher.pid"
        rmdir "$session_lock_dir" 2>/dev/null ||
            die "cannot recover stale domain lock: $session_lock_dir"
        mkdir "$session_lock_dir" 2>/dev/null ||
            die "another demo launcher acquired DDS domain $domain"
    fi
    printf '%s\n' "$$" > "$session_lock_dir/launcher.pid"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a path"
            build_dir="$2"
            shift 2
            ;;
        --connext-dir)
            [[ $# -ge 2 ]] || die "--connext-dir requires a path"
            connext_dir="$2"
            shift 2
            ;;
        --domain)
            [[ $# -ge 2 ]] || die "--domain requires a value"
            domain="$2"
            shift 2
            ;;
        --control-domain)
            [[ $# -ge 2 ]] || die "--control-domain requires a value"
            control_domain="$2"
            shift 2
            ;;
        --targets)
            [[ $# -ge 2 ]] || die "--targets requires a value"
            targets="$2"
            shift 2
            ;;
        --run-seconds)
            [[ $# -ge 2 ]] || die "--run-seconds requires a value"
            run_seconds="$2"
            shift 2
            ;;
        --headless)
            headless=1
            shift
            ;;
        --target-control)
            start_target_control=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

require_unsigned "--domain" "$domain"
if [[ -n "$control_domain" ]]; then
    require_unsigned "--control-domain" "$control_domain"
fi
require_unsigned "--targets" "$targets"
require_unsigned "--run-seconds" "$run_seconds"
domain=$((10#$domain))
if [[ -n "$control_domain" ]]; then
    control_domain=$((10#$control_domain))
else
    control_domain=$(((domain + 1) % 233))
fi
targets=$((10#$targets))
run_seconds=$((10#$run_seconds))
((domain <= 232)) || die "--domain must be between 0 and 232"
((control_domain <= 232)) ||
    die "--control-domain must be between 0 and 232"
((control_domain != domain)) ||
    die "--control-domain must differ from --domain"
((targets >= 1 && targets <= 256)) || die "--targets must be between 1 and 256"
((run_seconds <= 604800)) || die "--run-seconds must not exceed 604800"

resolve_executables() {
    local candidate="$1"
    local radar_candidate=""

    if [[ -x "$candidate/radar_app.app/Contents/MacOS/radar_app" ]]; then
        radar_candidate="$candidate/radar_app.app/Contents/MacOS/radar_app"
    elif [[ -x "$candidate/radar_app" ]]; then
        radar_candidate="$candidate/radar_app"
    elif [[ -x "$candidate/bin/radar_app" ]]; then
        radar_candidate="$candidate/bin/radar_app"
    fi

    local target_candidate=""
    if [[ -x "$candidate/target_gen" ]]; then
        target_candidate="$candidate/target_gen"
    elif [[ -x "$candidate/bin/target_gen" ]]; then
        target_candidate="$candidate/bin/target_gen"
    fi

    local control_candidate=""
    if [[ -x "$candidate/target_control" ]]; then
        control_candidate="$candidate/target_control"
    elif [[ -x "$candidate/bin/target_control" ]]; then
        control_candidate="$candidate/bin/target_control"
    fi

    if [[ -n "$radar_candidate" && -n "$target_candidate" &&
          -n "$control_candidate" ]]; then
        build_dir="$(cd "$candidate" && pwd)"
        radar_exe="$radar_candidate"
        target_exe="$target_candidate"
        control_exe="$control_candidate"
        return 0
    fi
    return 1
}

radar_exe=""
target_exe=""
control_exe=""
if [[ -n "$build_dir" ]]; then
    [[ -d "$build_dir" ]] || die "build directory does not exist: $build_dir"
    resolve_executables "$build_dir" ||
        die "radar_app, target_gen, and target_control were not found in: $build_dir"
else
    for candidate in "$repo_root/build/macos-arm64" "$repo_root/build" "$repo_root"; do
        if [[ -d "$candidate" ]] && resolve_executables "$candidate"; then
            break
        fi
    done
    [[ -n "$radar_exe" && -n "$target_exe" && -n "$control_exe" ]] ||
        die "no complete build found; run cmake --build --preset macos-relwithdebinfo"
fi

if [[ -z "$connext_dir" && -d /Applications/rti_connext_dds-7.7.0 ]]; then
    connext_dir=/Applications/rti_connext_dds-7.7.0
fi
[[ -n "$connext_dir" ]] ||
    die "set CONNEXTDDS_DIR/NDDSHOME or pass --connext-dir"
[[ -d "$connext_dir" ]] || die "Connext installation does not exist: $connext_dir"
connext_dir="$(cd "$connext_dir" && pwd)"
export CONNEXTDDS_DIR="$connext_dir"
export NDDSHOME="$connext_dir"

connext_lib_dir=""
if [[ -f "$build_dir/radar-connext-arch.txt" ]]; then
    IFS= read -r configured_arch < "$build_dir/radar-connext-arch.txt"
    [[ -n "$configured_arch" ]] ||
        die "empty Connext architecture manifest: $build_dir/radar-connext-arch.txt"
    [[ "$configured_arch" != */* ]] ||
        die "invalid Connext architecture in build manifest: $configured_arch"
    [[ -d "$connext_dir/lib/$configured_arch" ]] ||
        die "build requires missing Connext architecture: $configured_arch"
    connext_lib_dir="$connext_dir/lib/$configured_arch"
elif [[ -n "${CONNEXTDDS_ARCH:-}" && -d "$connext_dir/lib/$CONNEXTDDS_ARCH" ]]; then
    connext_lib_dir="$connext_dir/lib/$CONNEXTDDS_ARCH"
else
    case "$(uname -s)" in
        Darwin) lib_pattern="*Darwin*" ;;
        Linux)  lib_pattern="*Linux*" ;;
        *)      lib_pattern="*" ;;
    esac
    for candidate in "$connext_dir"/lib/$lib_pattern; do
        if [[ -d "$candidate" ]]; then
            connext_lib_dir="$candidate"
            break
        fi
    done
fi
[[ -n "$connext_lib_dir" ]] ||
    die "no Connext target library directory found under: $connext_dir/lib"
export CONNEXTDDS_ARCH="${connext_lib_dir##*/}"

case "$(uname -s)" in
    Darwin)
        export DYLD_LIBRARY_PATH="$connext_lib_dir${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
        ;;
    Linux)
        export LD_LIBRARY_PATH="$connext_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        ;;
esac

if [[ -n "${RADAR_QOS_FILE:-}" ]]; then
    [[ -f "$RADAR_QOS_FILE" ]] || die "RADAR_QOS_FILE does not exist: $RADAR_QOS_FILE"
elif [[ -f "$repo_root/qos/radar_qos.xml" ]]; then
    export RADAR_QOS_FILE="$repo_root/qos/radar_qos.xml"
elif [[ -f "$build_dir/qos/radar_qos.xml" ]]; then
    export RADAR_QOS_FILE="$build_dir/qos/radar_qos.xml"
elif [[ -f "$build_dir/bin/qos/radar_qos.xml" ]]; then
    export RADAR_QOS_FILE="$build_dir/bin/qos/radar_qos.xml"
else
    die "qos/radar_qos.xml was not found in the repository, build, or installation directory"
fi

existing_demo_pids="$(conflicting_demo_pids)"
if [[ -n "$existing_demo_pids" ]]; then
    existing_demo_pids="${existing_demo_pids//$'\n'/, }"
    die "radar demo processes already use simulation domain $domain or control domain $control_domain (PIDs $existing_demo_pids); stop them or select other domains"
fi
acquire_session_lock

stamp="$(date +%Y%m%d-%H%M%S)"
log_dir="$build_dir/demo-logs/$stamp-$$"
mkdir -p "$log_dir"
stop_file="$log_dir/stop.signal"

process_running() {
    local pid="$1"
    [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null
}

collect_process() {
    local label="$1"
    local pid="$2"
    [[ -n "$pid" ]] || return 0

    local deadline=$((SECONDS + 15))
    while process_running "$pid" && ((SECONDS < deadline)); do
        sleep 0.2
    done
    if process_running "$pid"; then
        echo "$label did not stop cooperatively; sending SIGTERM" >&2
        kill -TERM "$pid" 2>/dev/null || true
        deadline=$((SECONDS + 2))
        while process_running "$pid" && ((SECONDS < deadline)); do
            sleep 0.2
        done
    fi
    if process_running "$pid"; then
        echo "$label ignored SIGTERM; sending SIGKILL" >&2
        kill -KILL "$pid" 2>/dev/null || true
    fi

    local status=0
    wait "$pid" || status=$?
    case "$label" in
        radar_app)      radar_status="$status" ;;
        target_gen)     target_status="$status" ;;
        target_control) control_status="$status" ;;
    esac
}

cleanup() {
    ((cleanup_started == 0)) || return 0
    cleanup_started=1
    set +e
    if [[ -n "$stop_file" ]]; then
        : > "$stop_file"
    fi
    collect_process "target_control" "$control_pid"
    collect_process "target_gen" "$target_pid"
    collect_process "radar_app" "$radar_pid"
    if [[ -n "$log_dir" ]]; then
        echo "Demo stopped. Logs: $log_dir"
    fi
    release_session_lock
    return 0
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

radar_args=(--domain "$domain" --stop-file "$stop_file")
target_args=(--domain "$domain" --control-domain "$control_domain"
             --targets "$targets" --stop-file "$stop_file")
control_args=(--domain "$control_domain" --stop-file "$stop_file")
if ((headless)); then
    radar_args+=(--headless)
fi
if ((run_seconds > 0)); then
    radar_args+=(--run-seconds "$run_seconds")
    target_args+=(--run-seconds "$run_seconds")
    control_args+=(--run-seconds "$run_seconds")
fi

"$radar_exe" "${radar_args[@]}" \
    >"$log_dir/radar.stdout.log" 2>"$log_dir/radar.stderr.log" &
radar_pid=$!
sleep 2
if ! process_running "$radar_pid"; then
    echo "radar_app exited during startup; see $log_dir" >&2
else
    "$target_exe" "${target_args[@]}" \
        >"$log_dir/target.stdout.log" 2>"$log_dir/target.stderr.log" &
    target_pid=$!
    if ((start_target_control)); then
        sleep 1
        "$control_exe" "${control_args[@]}" \
            >"$log_dir/control.stdout.log" 2>"$log_dir/control.stderr.log" &
        control_pid=$!
        sleep 1
        if ! process_running "$control_pid"; then
            echo "target_control exited during startup; see $log_dir/control.stderr.log" >&2
            echo "Radar and target generator remain active (PIDs $radar_pid, $target_pid)."
        else
            echo "AESA radar demo running on simulation domain $domain and control domain $control_domain (PIDs $radar_pid, $target_pid, $control_pid)."
            echo "Close the radar window or press Ctrl-C to stop all three processes."
        fi
    else
        echo "AESA radar demo running on DDS domain $domain (PIDs $radar_pid, $target_pid)."
        echo "Optional target UI: $control_exe --domain $control_domain"
        echo "Close the radar window or press Ctrl-C to stop both processes."
    fi
    echo "Logs: $log_dir"

    while process_running "$radar_pid" && process_running "$target_pid"; do
        sleep 0.2
    done
fi

cleanup
trap - EXIT INT TERM

exit_code=0
if [[ -n "$radar_status" && "$radar_status" -ne 0 ]]; then
    echo "radar_app exited with code $radar_status" >&2
    exit_code=1
fi
if [[ -n "$target_status" && "$target_status" -ne 0 ]]; then
    echo "target_gen exited with code $target_status" >&2
    exit_code=1
fi
if [[ -n "$control_status" && "$control_status" -ne 0 ]]; then
    echo "target_control exited with code $control_status" >&2
    exit_code=1
fi
exit "$exit_code"
