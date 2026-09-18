#!/usr/bin/env bash
#
# 生成中心部署用的全套凭据：管理 token、每个 Agent 节点的独立密钥，
# 以及 Server 需要的 agent-tokens.yaml 映射。全部写入指定目录，不打印内容。
#
# 用法:
#   bash deploy/production/generate-credentials.sh <auth-dir> [node-code ...]
#
# node-code 是将要部署的 Agent 节点编号，可以先不传（生成空映射，
# 后续接入节点时重新在别的目录生成或按相同格式手工补充）。
# 产出的文件属部署账户主组、0640，容器通过补充组读取；
# 目录里任何文件已存在时直接报错退出，避免覆盖旧凭据。

set -Eeuo pipefail

if [[ $# -lt 1 ]]; then
  printf 'Usage: %s <auth-dir> [node-code ...]\n' "${BASH_SOURCE[0]}" >&2
  exit 1
fi

auth_dir=$1
shift
node_codes=("$@")
auth_group="${LABBRIDGE_AUTH_GROUP:-$(id -g)}"

if ! command -v openssl >/dev/null 2>&1; then
  printf 'openssl is required to generate credentials.\n' >&2
  exit 1
fi

# 文件名和 YAML 键都来自节点编号，先统一收窄字符集，避免转义问题。
for node in "${node_codes[@]}"; do
  if [[ ! $node =~ ^[a-z0-9][a-z0-9._-]{0,63}$ ]]; then
    printf 'Invalid node code %q: use 1-64 chars of a-z 0-9 . _ - starting with a letter or digit.\n' "$node" >&2
    exit 1
  fi
done
if (( ${#node_codes[@]} > 1 )); then
  unique_count=$(printf '%s\n' "${node_codes[@]}" | sort -u | wc -l)
  if (( ${#node_codes[@]} != unique_count )); then
    printf 'Duplicate node codes in arguments.\n' >&2
    exit 1
  fi
fi

# 文件内容原样交给 Server，所以只接受一个 64 位十六进制串加可选末尾换行。
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

# 先生成到临时目录再整体搬过去：中途失败不会留下半套凭据。
work_dir=$(mktemp -d)
cleanup() { rm -rf -- "$work_dir"; }
trap cleanup EXIT

# umask 只影响这批新文件；目录本身最后统一收紧到 0700。
generate_token() { openssl rand -hex 32; }

management_token=$(generate_token)
declare -A node_tokens=()
for node in "${node_codes[@]}"; do
  node_tokens[$node]=$(generate_token)
  if [[ ${node_tokens[$node]} == "$management_token" ]]; then
    printf 'Generated tokens collided; this is practically impossible, please retry.\n' >&2
    exit 1
  fi
done

if [[ -e $auth_dir ]]; then
  printf 'Auth directory %s already exists.\n' "$auth_dir" >&2
  printf 'Refusing to overwrite; move it away or pick a new directory.\n' >&2
  exit 1
fi

(umask 077
 mkdir -p -- "$auth_dir"

 printf '%s\n' "$management_token" > "$work_dir/management.token"

 if (( ${#node_codes[@]} == 0 )); then
   printf 'agent_tokens: {}\n' > "$work_dir/agent-tokens.yaml"
 else
   printf 'agent_tokens:\n' > "$work_dir/agent-tokens.yaml"
   for node in "${node_codes[@]}"; do
     printf '  "%s": "%s"\n' "$node" "${node_tokens[$node]}" >> "$work_dir/agent-tokens.yaml"
     printf '%s\n' "${node_tokens[$node]}" > "$work_dir/$node.token"
   done
 fi

 for file in "$work_dir"/*; do
   install -m 0640 -g "$auth_group" -- "$file" "$auth_dir/$(basename -- "$file")"
 done
)
chmod 0700 "$auth_dir"

# 生成完立刻自校验格式，凭据不对 Server 会拒绝启动，问题在这里就暴露。
read_hex_credential "$auth_dir/management.token" >/dev/null
for node in "${node_codes[@]}"; do
  read_hex_credential "$auth_dir/$node.token" >/dev/null
  grep -q "\"$node\": \"$(cat -- "$auth_dir/$node.token")\"" "$auth_dir/agent-tokens.yaml"
done

printf 'Credentials written to %s (dir 0700, files 0640, group %s):\n' "$auth_dir" "$auth_group"
printf '  management.token      for administrators and the Web console\n'
printf '  agent-tokens.yaml     node-code -> key mapping for the Server\n'
for node in "${node_codes[@]}"; do
  printf '  %s.token         key for Agent %s\n' "$node" "$node"
done
printf 'Token values were not printed; copy the files where the Agents can read them.\n'
printf 'In the env file set: LABBRIDGE_AUTH_DIR=%s LABBRIDGE_AUTH_GROUP=%s\n' \
  "$(cd -- "$auth_dir" && pwd -P)" "$auth_group"
