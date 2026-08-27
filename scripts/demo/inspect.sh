#!/usr/bin/env bash

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
compose_file="$repo_root/deploy/demo/compose.yaml"
compose_project="${LABBRIDGE_DEMO_PROJECT:-labbridge-demo}"
demo_run_key="${LABBRIDGE_DEMO_RUN_KEY:?Set LABBRIDGE_DEMO_RUN_KEY to the key printed by run.sh}"

docker compose -p "$compose_project" -f "$compose_file" --profile demo run \
  --rm --no-deps -e "DEMO_RUN_KEY=$demo_run_key" demo-runner inspect_demo.py
