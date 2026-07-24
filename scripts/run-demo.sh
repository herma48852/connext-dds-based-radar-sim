#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

build_dir=""
connext_dir="${CONNEXTDDS_DIR:-${NDDSHOME:-}}"
domain=92
targets=16
run_seconds=0
headless=0

radar_pid=""
target_pid=""
radar_status=""
target_status=""
cleanup_started=0
stop_file=""
log_dir=""

usage() {
    cat <<'EOF'
Usage: scripts/run-demo.sh [options]

Launch radar_app and target_gen together with cooperative shutdown and logs.

Options:
  --build-dir PATH    CMake build directory (auto-detected by default)
  --connext-dir PATH  RTI Connext DDS installation (uses CONNEXTDDS_DIR/NDDSHOME)
  --domain N          DDS domain, 0..232 (default: 92)
  --targets N         Number of targets, 1..256 (default: 16)
  --run-seconds N     Stop after N seconds; 0 runs until window close/Ctrl-C
  --headless          Run radar_app without a window
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
require_unsigned "--targets" "$targets"
require_unsigned "--run-seconds" "$run_seconds"
domain=$((10#$domain))
targets=$((10#$targets))
run_seconds=$((10#$run_seconds))
((domain <= 232)) || die "--domain must be between 0 and 232"
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

    if [[ -n "$radar_candidate" && -n "$target_candidate" ]]; then
        build_dir="$(cd "$candidate" && pwd)"
        radar_exe="$radar_candidate"
        target_exe="$target_candidate"
        return 0
    fi
    return 1
}

radar_exe=""
target_exe=""
if [[ -n "$build_dir" ]]; then
    [[ -d "$build_dir" ]] || die "build directory does not exist: $build_dir"
    resolve_executables "$build_dir" ||
        die "radar_app and target_gen were not found in: $build_dir"
else
    for candidate in "$repo_root/build/macos-arm64" "$repo_root/build" "$repo_root"; do
        if [[ -d "$candidate" ]] && resolve_executables "$candidate"; then
            break
        fi
    done
    [[ -n "$radar_exe" && -n "$target_exe" ]] ||
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
if [[ -n "${CONNEXTDDS_ARCH:-}" && -d "$connext_dir/lib/$CONNEXTDDS_ARCH" ]]; then
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
else
    die "qos/radar_qos.xml was not found in the repository or build directory"
fi

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
    if [[ "$label" == "radar_app" ]]; then
        radar_status="$status"
    else
        target_status="$status"
    fi
}

cleanup() {
    ((cleanup_started == 0)) || return 0
    cleanup_started=1
    set +e
    if [[ -n "$stop_file" ]]; then
        : > "$stop_file"
    fi
    collect_process "target_gen" "$target_pid"
    collect_process "radar_app" "$radar_pid"
    if [[ -n "$log_dir" ]]; then
        echo "Demo stopped. Logs: $log_dir"
    fi
    return 0
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

radar_args=(--domain "$domain" --stop-file "$stop_file")
target_args=(--domain "$domain" --targets "$targets" --stop-file "$stop_file")
if ((headless)); then
    radar_args+=(--headless)
fi
if ((run_seconds > 0)); then
    radar_args+=(--run-seconds "$run_seconds")
    target_args+=(--run-seconds "$run_seconds")
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
    echo "AESA radar demo running on DDS domain $domain (PIDs $radar_pid, $target_pid)."
    echo "Close the radar window or press Ctrl-C to stop both processes."
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
exit "$exit_code"
