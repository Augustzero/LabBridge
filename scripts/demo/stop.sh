#!/usr/bin/env bash

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
compose_file="$repo_root/deploy/demo/compose.yaml"
compose_project="${LABBRIDGE_DEMO_PROJECT:-labbridge-demo}"

# stop 只需要 compose 完成变量插值，凭据文件本身不必存在。
export LABBRIDGE_DEMO_AUTH_DIR="${LABBRIDGE_DEMO_AUTH_DIR:-$HOME/.local/state/labbridge/demo-auth}"
export LABBRIDGE_DEMO_AUTH_GROUP="${LABBRIDGE_DEMO_AUTH_GROUP:-$(id -g)}"

docker compose -p "$compose_project" -f "$compose_file" stop postgres server agent web
printf 'LabBridge demo services stopped. Named volumes and all evidence were retained.\n'
printf 'Restart or run ./scripts/demo/run.sh to create another isolated demo.\n'
