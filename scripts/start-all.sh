#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
Usage: scripts/start-all.sh [options]

Launch radar_app, target_gen, and target_control together with cooperative
shutdown and separate logs.

Options:
  --build-dir PATH    CMake build directory (auto-detected by default)
  --connext-dir PATH  RTI Connext DDS installation (uses CONNEXTDDS_DIR/NDDSHOME)
  --domain N          Simulation DDS domain, 0..232 (default: 92)
  --control-domain N  Target-control domain (default: simulation domain + 1)
  --targets N         Total targets: baseline plus N-1 randomized (default: 32)
  --run-seconds N     Stop after N seconds; 0 runs until the radar closes
  --disable-sub-3km   Restore the legacy hard 3 km receive gate
  -h, --help          Show this help

Examples:
  ./scripts/start-all.sh
  ./scripts/start-all.sh --domain 92 --targets 32
  ./scripts/start-all.sh --disable-sub-3km
  ./scripts/start-all.sh --domain 92 --control-domain 93 --run-seconds 60
EOF
}

for argument in "$@"; do
    case "$argument" in
        -h|--help)
            usage
            exit 0
            ;;
    esac
done

exec "$script_dir/run-demo.sh" --target-control "$@"
