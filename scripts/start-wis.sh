#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

connext_dir="${CONNEXTDDS_DIR:-${NDDSHOME:-/Applications/rti_connext_dds-7.7.0}}"
config_file="$repo_root/config/radar_live_view_wis.xml"
config_name="DetectionBeamLiveView"
document_root="$repo_root/docs"
listening_ports="18080"
verbosity="3"
enable_builtin_topics=0

usage() {
    cat <<'EOF'
Usage: scripts/start-wis.sh [options]

Launch one Section 4 multi-topic live view through RTI Web Integration
Service. Defaults: DetectionBeamLiveView, docs document root, port 18080.

Single-view configuration names:
  DetectionBeamLiveView
  RmaOutageImpactLiveView
  AssociationDiagnosticsLiveView
  MotionGeometryLiveView
  TrackLossLiveView

Options:
  --connext-dir PATH       RTI Connext installation
  --config-file PATH      WIS XML configuration
  --cfg-name NAME         Single-view web_integration_service name
  --document-root PATH    Static web document root
  --listening-ports PORTS WIS port list, for example 18080 or 18080,18443s
  --verbosity 0..6        WIS logging verbosity
  --enable-builtin-topics Enable DDS built-in topics
  -h, --help              Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --connext-dir)
            [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 2; }
            connext_dir="$2"; shift 2 ;;
        --config-file)
            [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 2; }
            config_file="$2"; shift 2 ;;
        --cfg-name)
            [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 2; }
            config_name="$2"; shift 2 ;;
        --document-root)
            [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 2; }
            document_root="$2"; shift 2 ;;
        --listening-ports)
            [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 2; }
            listening_ports="$2"; shift 2 ;;
        --verbosity)
            [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 2; }
            verbosity="$2"; shift 2 ;;
        --enable-builtin-topics)
            enable_builtin_topics=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "ERROR: unknown option: $1" >&2
            usage >&2
            exit 2 ;;
    esac
done

[[ "$verbosity" =~ ^[0-6]$ ]] || { echo "ERROR: --verbosity must be 0..6" >&2; exit 2; }

wis_launcher="$connext_dir/bin/rtiwebintegrationservice"
[[ -x "$wis_launcher" ]] || {
    echo "ERROR: RTI Web Integration Service was not found at '$wis_launcher'." >&2
    echo "Set CONNEXTDDS_DIR or NDDSHOME, or pass --connext-dir." >&2
    exit 1
}
[[ -f "$config_file" ]] || { echo "ERROR: WIS configuration was not found at '$config_file'." >&2; exit 1; }
[[ -d "$document_root" ]] || { echo "ERROR: document root was not found at '$document_root'." >&2; exit 1; }

wis_arguments=(
    -cfgFile "$config_file"
    -cfgName "$config_name"
    -enableWebSockets
    -documentRoot "$document_root"
    -listeningPorts "$listening_ports"
    -verbosity "$verbosity"
)
if (( enable_builtin_topics )); then
    wis_arguments+=( -enableBuiltinTopics )
fi

printf 'Starting RTI Web Integration Service\n'
printf '  config:        %s\n' "$config_file"
printf '  configuration: %s\n' "$config_name"
printf '  document root: %s\n' "$document_root"
printf '  listening:     %s\n\n' "$listening_ports"

exec "$wis_launcher" "${wis_arguments[@]}"
