#!/usr/bin/env bash

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
compose_file="$repo_root/deploy/demo/compose.yaml"
compose_project="${LABBRIDGE_DEMO_PROJECT:-labbridge-demo}"
demo_run_key="${LABBRIDGE_DEMO_RUN_KEY:-$(date -u +%Y%m%d%H%M%S)-$(printf '%04x%04x' "$RANDOM" "$RANDOM")}"

compose() {
  docker compose -p "$compose_project" -f "$compose_file" "$@"
}

show_failure_context() {
  status=$?
  if (( status == 0 )); then
    return
  fi
  printf '\nDemo failed; services and volumes were kept for diagnosis.\n' >&2
  printf 'demo_run_key=%s compose_project=%s\n' "$demo_run_key" "$compose_project" >&2
  if command -v docker >/dev/null 2>&1; then
    compose ps >&2 || true
    printf 'Inspect logs with:\n  docker compose -p %q -f %q logs --tail=200 server agent demo-runner\n' \
      "$compose_project" "$compose_file" >&2
  fi
  exit "$status"
}

if [[ ! $demo_run_key =~ ^[a-z0-9][a-z0-9-]{7,47}$ ]]; then
  printf 'LABBRIDGE_DEMO_RUN_KEY must contain 8-48 lowercase letters, digits or hyphens.\n' >&2
  exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
  printf 'Docker CLI is required. Install Docker Engine and the Compose plugin first.\n' >&2
  exit 1
fi
if ! docker info >/dev/null 2>&1; then
  printf 'Docker daemon is unavailable. Start Docker and retry.\n' >&2
  exit 1
fi
if ! docker compose version >/dev/null 2>&1; then
  printf 'The Docker Compose plugin is required (docker compose).\n' >&2
  exit 1
fi

trap show_failure_context EXIT

printf 'LabBridge development demo: unauthenticated and restricted to this machine.\n'
printf 'Do not expose these services to the public internet or an untrusted LAN.\n'
printf 'demo_run_key=%s compose_project=%s\n\n' "$demo_run_key" "$compose_project"

printf '[1/6] building Server, Agent, Web and demo runner images\n'
if [[ ${LABBRIDGE_DEMO_SKIP_BUILD:-0} == 1 ]]; then
  printf 'using images built by the current review workflow\n'
else
  compose --profile demo build server agent web demo-runner
fi

printf '[2/6] starting PostgreSQL, Server and Agent\n'
compose up -d --wait --wait-timeout 180 postgres server agent

printf '[3/6] starting Web and checking the same-origin API proxy\n'
compose up -d --wait --wait-timeout 180 web

printf '[4/6] waiting for Agent demo-node-001\n'
printf 'runner will verify registration and a real heartbeat through the Web proxy\n'

printf '[5/6] creating the CSV task and verifying its complete evidence chain\n'
compose --profile demo run --rm --no-deps \
  -e "DEMO_RUN_KEY=$demo_run_key" demo-runner run_demo.py

printf '[6/6] demo complete; browser and services remain available\n'
printf 'Web console: http://127.0.0.1:%s/nodes\n' "${LABBRIDGE_WEB_PORT:-8080}"
printf 'Inspect all evidence: LABBRIDGE_DEMO_RUN_KEY=%q ./scripts/demo/inspect.sh\n' "$demo_run_key"
printf 'Stop without deleting data: ./scripts/demo/stop.sh\n'

trap - EXIT
