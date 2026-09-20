#!/usr/bin/env bash
#
# LabBridge Agent 节点备份：把队列库（连同 WAL 旁文件）、工作目录/归档、
# 输入目录、配置和 token 打成一个备份批次，生成 SHA-256 清单。
# 只做本机导出、打包和校验；Agent 要先正常停止，批次怎么收集、怎么恢复
# 按 docs/operations/maintenance.md 执行。
#
# 批次约定同中心备份：输出根目录下一个子目录一批，manifest.txt 最后落盘，
# 有 manifest.txt 才算完整批次（check.sh 巡检按这个找最近备份）。
# 中途失败时批次目录原样保留，人工排查后手动删除。
#
# 用法:
#   bash scripts/ops/backup-agent.sh --backup-id <批次号> --output-root <目录> \
#     [--config /etc/labbridge/agent.yaml]
#
# --backup-id    备份批次号，和中心侧同一批用同一个编号（如 20260918T1530）
# --output-root  备份输出根目录，批次目录建在它下面
# --config       Agent 配置文件，默认 /etc/labbridge/agent.yaml，
#                备份哪些路径全部从配置里读

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/lib/backup-common.sh"

usage() { awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }

die() { printf '%s\n' "$*" >&2; exit 1; }

config=/etc/labbridge/agent.yaml
backup_id=''
output_root=''

while (($#)); do
  [[ $1 == -h || $1 == --help || $# -ge 2 ]] || die "选项缺少取值: $1"
  case $1 in
    --config) config=$2; shift 2 ;;
    --backup-id) backup_id=$2; shift 2 ;;
    --output-root) output_root=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) printf '未知选项: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n $backup_id && -n $output_root ]] || { usage >&2; exit 2; }
[[ $backup_id =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] \
  || die "批次号得是字母或数字开头的 1-64 位 [A-Za-z0-9._-]: $backup_id"
command -v sqlite3 >/dev/null 2>&1 || die '缺少 sqlite3 命令（apt install sqlite3）'
command -v tar >/dev/null 2>&1 || die '缺少 tar 命令'
[[ -r $config ]] || die "Agent 配置不可读: $config（用 --config 指定）"

# 批次里有 token，产物按私有权限落盘
umask 077

node_code='' token_file='' queue_db='' work_dir=''
allowed_roots=()
config_values=$(python3 "$script_dir/lib/agent-config.py" "$config")
while IFS=$'\t' read -r key value; do
  case $key in
    agent.node_code) node_code=$value ;;
    agent.token_file) token_file=$value ;;
    storage.queue_db) queue_db=$value ;;
    storage.work_dir) work_dir=$value ;;
    tasks.allowed_local_roots) allowed_roots+=("$value") ;;
  esac
done <<<"$config_values"

[[ -f $queue_db ]] || die "队列库不存在: $queue_db"
[[ -d $work_dir ]] || die "工作目录不存在: $work_dir"
for root in "${allowed_roots[@]}"; do
  [[ $root != / ]] || die "输入目录不能是文件系统根目录"
  [[ -d $root ]] || die "输入目录不存在: $root"
done
[[ -n $token_file && -r $token_file ]] \
  || die "token 文件不可读: ${token_file:-<配置未填 token_file>}"

# Agent 必须停稳了再备：运行中打包会拿到队列、归档互相矛盾的状态
if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
  state=$(systemctl is-active labbridge-agent 2>/dev/null || true)
  case $state in
    active|activating|deactivating)
      die "labbridge-agent 还是 $state。先 systemctl stop，等它正常退出后再备份" ;;
  esac
fi
if command -v pgrep >/dev/null 2>&1 && pgrep -x labbridge_agent >/dev/null 2>&1; then
  die "检测到 labbridge_agent 进程还在跑，先正常停止再备份"
fi

# 状态按目录树合并打包：queue_db 所在目录和 work_dir 谁包含谁，就用外层那个
strip_root() { local p=${1%/}; printf '%s' "${p#/}"; }
state_root=$(dirname -- "$queue_db")
[[ $state_root != / && $work_dir != / ]] || die "状态目录不能是文件系统根目录"
state_members=("$(strip_root "$state_root")")
work_member=$(strip_root "$work_dir")
if [[ ${state_members[0]} == "$work_member"/* ]]; then
  state_members=("$work_member")
elif [[ $work_member != "${state_members[0]}" && $work_member != "${state_members[0]}"/* ]]; then
  state_members+=("$work_member")
fi
input_members=()
for root in "${allowed_roots[@]}"; do
  input_members+=("$(strip_root "$root")")
done

mkdir -p -- "$output_root"
batch_dir="$output_root/$backup_id"
[[ -e $batch_dir ]] && die "批次目录已存在，一批备份换一个新的批次号: $batch_dir"
batch_real=$(realpath -m -- "$batch_dir")

# 输出不能落在被备份的内容里，不然 tar 会把越写越大的备份自己打进去
for src in "$state_root" "$work_dir" "${allowed_roots[@]}" \
  "$(dirname -- "$(realpath -m -- "$config")")" \
  "$(dirname -- "$(realpath -m -- "$token_file")")"; do
  src_real=$(realpath -- "$src")
  if [[ $batch_real == "$src_real" || $batch_real == "$src_real"/* ]]; then
    die "输出目录在被备份内容里: $batch_real 在 $src_real 之下"
  fi
done

# 空间预检：tar 的体量就是源内容的大小，不够就直接拒绝，别跑到一半才失败
needed_b=0
for member in "${state_members[@]}" "${input_members[@]}"; do
  needed_b=$((needed_b + $(du -sb -- "/$member" | cut -f1)))
done
avail_b=$(df -B1 --output=avail "$output_root" | tail -n 1 | tr -d ' ')
if (( avail_b < needed_b )); then
  die "输出目录可用 ${avail_b} 字节，小于待打包内容 ${needed_b} 字节，先清理或换目录"
fi

# 只读连接必须看得到 WAL，不能退回忽略 WAL 的 immutable 模式。
queue_uri="file:$queue_db?mode=ro"
queue_open_mode=ro
sqlite3 -batch "$queue_uri" 'SELECT 1 FROM queue_metadata' >/dev/null \
  || die "队列库无法完整只读打开: $queue_db；检查库、WAL 和目录权限"
query_queue=(sqlite3 -batch -noheader "$queue_uri")

# 备份前把库的状态摸清楚：身份、完整性、积压情况都记进 manifest，
# 恢复后逐项对回来；库本身坏了就别把它当成好备份备走
meta_node=$("${query_queue[@]}" 'SELECT node_code FROM queue_metadata WHERE singleton_id = 1')
[[ $meta_node == "$node_code" ]] \
  || die "队列身份($meta_node)与配置节点编号($node_code)不一致，先查清楚再备份"
integrity=$("${query_queue[@]}" 'PRAGMA integrity_check;')
[[ $integrity == ok ]] || die "队列库完整性检查没通过: $integrity"
pending_jobs=$("${query_queue[@]}" 'SELECT count(*) FROM pending_jobs')
pending_deliveries=$("${query_queue[@]}" 'SELECT count(*) FROM pending_deliveries')
ra_count=$("${query_queue[@]}" "SELECT count(*) FROM pending_jobs WHERE stage = 'requires_attention'")
stage_line=$("${query_queue[@]}" "SELECT stage || '=' || count(*) FROM pending_jobs \
  GROUP BY stage ORDER BY stage" | tr '\n' ' ')
work_files=$(find "$work_dir" -type f | wc -l)
work_bytes=$(find "$work_dir" -type f -printf '%s\n' | awk '{s+=$1} END{print s+0}')

mkdir -- "$batch_dir"

printf '[1/5] 打包状态目录（队列、WAL、工作与归档）\n'
# pax 格式保住纳秒级时间戳和权限；成员用相对路径，恢复时 tar -C / 落回原绝对位置
tar --format=pax --create --file "$batch_dir/agent-state.tar" -C / "${state_members[@]}"

printf '[2/5] 打包输入目录\n'
tar --format=pax --create --file "$batch_dir/input.tar" -C / "${input_members[@]}"

printf '[3/5] 复制配置与 token\n'
mkdir -p -- "$batch_dir/config/auth"
cp -a -- "$config" "$batch_dir/config/agent.yaml"
cp -a -- "$token_file" "$batch_dir/config/auth/$(basename -- "$token_file")"

printf '[4/5] 生成 SHA-256 清单并回验\n'
(cd -- "$batch_dir"; backup_checksums . agent-state.tar input.tar config/agent.yaml config/auth/*)

printf '[5/5] 写 manifest（有 manifest 才算完整批次）\n'
# 生产布局下从 current 链接读版本；开发机没有 /opt 布局就如实记 unknown
agent_revision=unknown
if [[ -e /opt/labbridge/current ]]; then
  agent_revision=$(basename -- "$(readlink -f -- /opt/labbridge/current)")
fi
manifest="$batch_dir/manifest.pending"
created_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
backup_host=$(hostname)
{
  printf 'LabBridge agent backup\n'
  printf 'backup_id: %s\n' "$backup_id"
  printf 'created_at_utc: %s\n' "$created_at"
  printf 'role: agent\n'
  printf 'host: %s\n' "$backup_host"
  printf 'node_code: %s\n' "$node_code"
  printf 'agent_revision: %s\n' "$agent_revision"
  printf '\nsource_paths:\n'
  printf '  config: %s\n' "$(realpath -m -- "$config")"
  printf '  token_file: %s\n' "$(realpath -m -- "$token_file")"
  printf '  work_dir: %s\n' "$work_dir"
  for member in "${state_members[@]}"; do
    printf '  state_member: /%s\n' "$member"
  done
  for root in "${allowed_roots[@]}"; do
    printf '  input_root: %s\n' "$root"
  done
  printf '\nqueue:\n'
  printf '  integrity_check: ok\n'
  printf '  open_mode: %s\n' "$queue_open_mode"
  printf '  pending_jobs: %s\n' "$pending_jobs"
  printf '  pending_deliveries: %s\n' "$pending_deliveries"
  printf '  requires_attention: %s\n' "$ra_count"
  printf '  stages: %s\n' "${stage_line:-<空>}"
  printf '\nstats:\n'
  printf '  work_files: %s\n' "$work_files"
  printf '  work_bytes: %s\n' "$work_bytes"
  for root in "${allowed_roots[@]}"; do
    printf '  input_files: %s\n' "$(find "$root" -type f | wc -l)"
    printf '  input_bytes: %s\n' "$(du -sb -- "$root" | cut -f1)"
    printf '  input_root: %s\n' "$root"
  done
  printf '\ncontents:\n'
  printf '  agent-state.tar     队列库、WAL 旁文件、工作目录与归档（pax 格式）\n'
  printf '  input.tar           输入目录全部文件（pax 格式）\n'
  printf '  config/             配置与 token，注意目录权限\n'
  printf '  SHA256SUMS          数据文件校验清单\n'
  printf '\nrestore guide: docs/operations/maintenance.md\n'
} > "$manifest"
publish_backup "$batch_dir"

printf '备份完成: %s\n' "$(realpath -- "$batch_dir")"
printf '后续: 和中心侧同批次一起收集核对，办法见 maintenance.md。\n'
