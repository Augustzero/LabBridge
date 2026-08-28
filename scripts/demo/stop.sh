#!/usr/bin/env bash

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
compose_file="$repo_root/deploy/demo/compose.yaml"
compose_project="${LABBRIDGE_DEMO_PROJECT:-labbridge-demo}"

docker compose -p "$compose_project" -f "$compose_file" stop postgres server agent web
printf 'LabBridge demo services stopped. Named volumes and all evidence were retained.\n'
printf 'Restart or run ./scripts/demo/run.sh to create another isolated demo.\n'
