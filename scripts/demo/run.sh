#!/usr/bin/env bash

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
compose_file="$repo_root/deploy/demo/compose.yaml"
compose_project="${LABBRIDGE_DEMO_PROJECT:-labbridge-demo}"
demo_run_key="${LABBRIDGE_DEMO_RUN_KEY:-$(date -u +%Y%m%d%H%M%S)-$(printf '%04x%04x' "$RANDOM" "$RANDOM")}"
auth_dir="${LABBRIDGE_DEMO_AUTH_DIR:-$HOME/.local/state/labbridge/demo-auth}"
auth_group="$(id -g)"

management_token_file="$auth_dir/management.token"
agent_tokens_file="$auth_dir/agent-tokens.yaml"
agent_token_file="$auth_dir/demo-node-001.token"

compose() {
  docker compose -p "$compose_project" -f "$compose_file" "$@"
}

# 文件内容原样交给 Server，所以这里也只接受一个可选的末尾换行。
read_hex_credential() {
  local token size
  token="$(cat -- "$1")"
  size="$(wc -c < "$1")"
  if [[ ! $token =~ ^[0-9a-f]{64}$ ]] || ((size != 64 && size != 65)); then
    printf 'Credential %s must contain a 64-hex token and an optional final newline.\n' "$1" >&2
    return 1
  fi
  printf '%s' "$token"
}

# 首次生成时就收紧权限，避免文件写完到 chmod 之间短暂可被其他账户读取。
prepare_demo_credentials() (
  umask 077
  local file management_token agent_token entry_pattern
  local files=("$management_token_file" "$agent_tokens_file" "$agent_token_file")
  if [[ -e $management_token_file || -e $agent_tokens_file || -e $agent_token_file ]]; then
    for file in "${files[@]}"; do
      if [[ ! -f $file ]]; then
        printf 'Demo credentials are incomplete; restore %s before retrying.\n' "$file" >&2
        return 1
      fi
    done
    management_token="$(read_hex_credential "$management_token_file")" || return 1
    agent_token="$(read_hex_credential "$agent_token_file")" || return 1
    if [[ $management_token == "$agent_token" ]]; then
      printf 'Management and Agent credentials must differ: %s\n' "$auth_dir" >&2
      return 1
    fi
    # 演示生成的是单行节点条目；提前发现手工修改后两份节点密钥没对齐。
    entry_pattern="^[[:space:]]+demo-node-001:[[:space:]]+"
    entry_pattern+="[\"']?${agent_token}[\"']?[[:space:]]*(#.*)?$"
    if ! grep -Eq "$entry_pattern" "$agent_tokens_file"; then
      printf 'Check demo-node-001 in %s: it must match %s.\n' \
        "$agent_tokens_file" "$agent_token_file" >&2
      return 1
    fi
    printf 'reusing existing demo credentials in %s\n' "$auth_dir"
  else
    mkdir -p -- "$auth_dir"
    chmod 0700 "$auth_dir"
    management_token="$(openssl rand -hex 32)"
    agent_token="$(openssl rand -hex 32)"
    printf '%s\n' "$management_token" > "$management_token_file"
    printf '%s\n' "$agent_token" > "$agent_token_file"
    printf 'agent_tokens:\n  demo-node-001: "%s"\n' "$agent_token" > "$agent_tokens_file"
    printf 'generated management token and demo-node-001 key in %s\n' "$auth_dir"
  fi

  # 复用凭据时也恢复约定权限，容器通过启动者的主组读取挂载文件。
  chmod 0700 "$auth_dir"
  for file in "${files[@]}"; do
    chgrp "$auth_group" "$file"
    chmod 0640 "$file"
  done
)

show_failure_context() {
  status=$?
  if (( status == 0 )); then
    return
  fi
  printf '\nDemo failed; services and volumes were kept for diagnosis.\n' >&2
  printf 'demo_run_key=%s compose_project=%s\n' "$demo_run_key" "$compose_project" >&2
  if command -v docker >/dev/null 2>&1; then
    compose ps >&2 || true
    printf 'Inspect logs with:\n  LABBRIDGE_DEMO_AUTH_DIR=%q LABBRIDGE_DEMO_AUTH_GROUP=%q docker compose -p %q -f %q logs --tail=200 server agent demo-runner\n' \
      "$LABBRIDGE_DEMO_AUTH_DIR" "$LABBRIDGE_DEMO_AUTH_GROUP" "$compose_project" "$compose_file" >&2
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
if ! command -v openssl >/dev/null 2>&1; then
  printf 'openssl is required to generate demo credentials.\n' >&2
  exit 1
fi

printf 'LabBridge development demo: API access requires credentials; restricted to this machine.\n'
printf 'Do not expose these services to the public internet or an untrusted LAN.\n'
printf 'demo_run_key=%s compose_project=%s\n\n' "$demo_run_key" "$compose_project"

prepare_demo_credentials
# 导出给 compose 插值：AUTH_DIR 用物理路径，保证本机守护进程能直接 bind mount；
# AUTH_GROUP 是启动者主组，容器靠它以补充组身份读取 0640 的凭据文件。
LABBRIDGE_DEMO_AUTH_DIR="$(cd -- "$auth_dir" && pwd -P)"
export LABBRIDGE_DEMO_AUTH_DIR
export LABBRIDGE_DEMO_AUTH_GROUP="$auth_group"
trap show_failure_context EXIT

printf '[1/4] building Server, Agent, Web and demo runner images\n'
if [[ ${LABBRIDGE_DEMO_SKIP_BUILD:-0} == 1 ]]; then
  printf 'using images built by the current review workflow\n'
else
  compose --profile demo build server agent web demo-runner
fi

printf '[2/4] starting PostgreSQL, Server, Agent and Web\n'
compose up -d --wait --wait-timeout 180 postgres server agent web

printf '[3/4] running isolated CSV demo and HTTP evidence checks\n'
compose --profile demo run --rm --no-deps \
  -e "DEMO_RUN_KEY=$demo_run_key" demo-runner run_demo.py

printf '[4/4] demo complete; services remain available for inspection\n'
printf 'Web console: http://127.0.0.1:%s/nodes\n' "${LABBRIDGE_WEB_PORT:-8080}"
printf 'Management token file: %s\n' "$LABBRIDGE_DEMO_AUTH_DIR/management.token"
printf 'Paste that token when the console asks for access credentials.\n'
printf 'Management API: http://127.0.0.1:%s/api/v1\n' "${LABBRIDGE_SERVER_PORT:-18080}"
printf 'Inspect all evidence: LABBRIDGE_DEMO_RUN_KEY=%q bash ./scripts/demo/inspect.sh\n' "$demo_run_key"
printf 'Stop without deleting data: bash ./scripts/demo/stop.sh\n'

trap - EXIT
