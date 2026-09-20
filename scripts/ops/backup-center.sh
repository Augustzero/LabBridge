#!/usr/bin/env bash
#
# LabBridge 中心备份：在中心机把 PostgreSQL 业务库连同部署配置、认证文件
# 打成一个备份批次，生成 SHA-256 清单。只做本机导出、打包和校验；
# 服务怎么停、批次怎么收集到原机之外，按 docs/operations/maintenance.md
# 的顺序人工完成。
#
# 批次约定：输出根目录下一个子目录一批，manifest.txt 最后才落盘，
# 有 manifest.txt 才算完整批次（check.sh 巡检也按这个找最近备份）。
# 中途失败时批次目录原样保留，里面没有 manifest.txt，人工排查后手动删除。
#
# 用法（在仓库根目录执行）:
#   bash scripts/ops/backup-center.sh --backup-id <批次号> --output-root <目录> \
#     [--env-file F] [--compose P]
#
# --backup-id    备份批次号，同一批中心和各节点用同一个编号，
#                建议用时间戳形式如 20260918T1530
# --output-root  备份输出根目录，批次目录建在它下面
# --env-file     环境文件，默认 /etc/labbridge/production.env
# --compose      Compose 文件，默认 deploy/production/compose.yaml
#
# 执行前 server 和 web 应该已经停掉、postgres 保持运行（维护文档定的顺序），
# 脚本会检查这个前置状态，顺序不对直接拒绝。

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/lib/backup-common.sh"

usage() { awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }

die() { printf '%s\n' "$*" >&2; exit 1; }

env_file=/etc/labbridge/production.env
compose_file=deploy/production/compose.yaml
backup_id=''
output_root=''

while (($#)); do
  [[ $1 == -h || $1 == --help || $# -ge 2 ]] || die "选项缺少取值: $1"
  case $1 in
    --env-file) env_file=$2; shift 2 ;;
    --compose) compose_file=$2; shift 2 ;;
    --backup-id) backup_id=$2; shift 2 ;;
    --output-root) output_root=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) printf '未知选项: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n $backup_id && -n $output_root ]] || { usage >&2; exit 2; }
[[ $backup_id =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] \
  || die "批次号得是字母或数字开头的 1-64 位 [A-Za-z0-9._-]: $backup_id"
[[ -r $env_file ]] || die "环境文件不可读: $env_file（用 --env-file 指定）"
[[ -r $compose_file ]] || die "Compose 文件不可读: $compose_file（用 --compose 指定，需在仓库根目录执行）"
command -v python3 >/dev/null 2>&1 || die '缺少 python3 命令'
command -v docker >/dev/null 2>&1 || die '缺少 docker 命令'
docker info >/dev/null 2>&1 || die 'Docker daemon 未运行'

# 批次里有 token 和数据库密码，产物按私有权限落盘
umask 077

compose_cmd=(docker compose --env-file "$env_file" -f "$compose_file")
# 环境文件的引号、变量展开交给 Compose，自行 sed 容易和实际部署读出不同值。
resolved_config=$("${compose_cmd[@]}" config --format json)
config_values=$(python3 -c '
import json, pathlib, sys
services = json.load(sys.stdin)["services"]
mount = next(v for v in services["server"]["volumes"]
             if v["target"] == "/etc/labbridge/auth/management.token")
values = (str(pathlib.Path(mount["source"]).parent),
          services["server"]["image"].rsplit(":", 1)[-1], services["postgres"]["image"])
if any(any(c in v for c in "\t\r\n") for v in values):
    sys.exit("部署配置含非法控制字符")
print("\t".join(values))
' <<<"$resolved_config")
unset resolved_config
IFS=$'\t' read -r auth_dir image_tag postgres_image <<<"$config_values"
[[ -n $auth_dir && -d $auth_dir ]] || die "环境文件里的 LABBRIDGE_AUTH_DIR 不存在: ${auth_dir:-<空>}"
env_real=$(realpath -- "$env_file")
auth_real=$(realpath -- "$auth_dir")

mkdir -p -- "$output_root"
batch_dir="$output_root/$backup_id"
[[ -e $batch_dir ]] && die "批次目录已存在，一批备份换一个新的批次号: $batch_dir"
batch_real=$(realpath -m -- "$batch_dir")

# 输出不能落在被备份的内容里，不然会把备份再备进去
for src in "$(dirname -- "$env_real")" "$auth_real"; do
  if [[ $batch_real == "$src" || $batch_real == "$src"/* ]]; then
    die "输出目录在被备份内容里: $batch_real 在 $src 之下"
  fi
done



# 前置状态：postgres 在跑（导出走容器内连接），server/web 已停。
# 停写是为了让中心状态和各节点备份时对得上，这是维护文档定的顺序。
running_services=$("${compose_cmd[@]}" ps --status running --services 2>/dev/null || true)
if ! grep -qx postgres <<<"$running_services"; then
  die "postgres 未运行，先启动它再备份: ${compose_cmd[*]} up -d postgres"
fi
for svc in server web; do
  if grep -qx "$svc" <<<"$running_services"; then
    die "$svc 还在运行。备份顺序是先停 server/web（postgres 保留），见 docs/operations/maintenance.md"
  fi
done

# 空间预检：导出文件的体量不会超过数据库本身，按它保守估计
db_size=$("${compose_cmd[@]}" exec -T postgres psql -U labbridge -d labbridge -tAc \
  "SELECT pg_database_size('labbridge')" | tr -d '[:space:]')
avail_b=$(df -B1 --output=avail "$output_root" | tail -n 1 | tr -d ' ')
if (( avail_b < db_size )); then
  die "输出目录可用 ${avail_b} 字节，小于数据库当前大小 ${db_size} 字节，先清理或换目录"
fi

mkdir -- "$batch_dir"

printf '[1/6] 导出数据库（pg_dump 自定义格式）\n'
"${compose_cmd[@]}" exec -T postgres pg_dump -U labbridge -d labbridge --format=custom \
  > "$batch_dir/center.pgdump"

printf '[2/6] 回读校验导出文件\n'
# pg_restore 从 stdin 回读，不在容器中留下共享临时文件。
"${compose_cmd[@]}" exec -T postgres pg_restore --list < "$batch_dir/center.pgdump" > /dev/null

printf '[3/6] 记录版本与各表行数\n'
pg_version=$("${compose_cmd[@]}" exec -T postgres psql -U labbridge -d labbridge -tAc \
  'SHOW server_version' | tr -d '[:space:]')
# 行数进 manifest，恢复后拿它回验；一次查询取全，表名都是代码里的常量
tables=(nodes data_sources tasks task_runs raw_files parsed_records qc_rules
  task_qc_rules qc_results alerts agent_report_receipts)
query="SELECT 'nodes', count(*) FROM nodes"
for t in "${tables[@]:1}"; do
  query+=" UNION ALL SELECT '$t', count(*) FROM $t"
done
row_counts=$("${compose_cmd[@]}" exec -T postgres psql -U labbridge -d labbridge -tA -F'=' \
  -c "$query")

printf '[4/6] 复制配置与认证文件\n'
mkdir -p -- "$batch_dir/config"
cp -a -- "$env_real" "$batch_dir/config/production.env"
cp -a -- "$auth_real" "$batch_dir/config/auth"

printf '[5/6] 生成 SHA-256 清单并回验\n'
(cd -- "$batch_dir"; backup_checksums . center.pgdump config/production.env config/auth/*)

printf '[6/6] 写 manifest（有 manifest 才算完整批次）\n'
# 节点清单从凭据目录的 <node>.token 文件名取，恢复后核对“同批备份齐全”靠它
node_codes=$(find "$auth_real" -maxdepth 1 -name '*.token' ! -name 'management.token' \
  -printf '%f\n' | sed 's/\.token$//' | sort | tr '\n' ' ')
[[ -n $node_codes ]] && node_codes=" ${node_codes% }"
manifest="$batch_dir/manifest.pending"
created_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
backup_host=$(hostname)
{
  printf 'LabBridge center backup\n'
  printf 'backup_id: %s\n' "$backup_id"
  printf 'created_at_utc: %s\n' "$created_at"
  printf 'role: center\n'
  printf 'host: %s\n' "$backup_host"
  printf 'image_tag: %s\n' "${image_tag:-<未设置>}"
  printf 'postgres_image: %s\n' "${postgres_image:-postgres:16.15-bookworm}"
  printf 'postgres_server_version: %s\n' "$pg_version"
  printf 'node_codes:%s\n' "${node_codes:- (none)}"
  printf '\nsource_paths:\n'
  printf '  env_file: %s\n' "$env_real"
  printf '  auth_dir: %s\n' "$auth_real"
  printf '  compose_file: %s\n' "$(realpath -m -- "$compose_file")"
  printf '\ntable_row_counts:\n'
  sed 's/^/  /' <<<"$row_counts"
  printf '\ncontents:\n'
  printf '  center.pgdump        PostgreSQL 自定义格式导出，恢复用 pg_restore\n'
  printf '  config/              环境文件与认证文件，含 token，注意目录权限\n'
  printf '  SHA256SUMS           数据文件校验清单\n'
  printf '\nrestore guide: docs/operations/maintenance.md\n'
} > "$manifest"
publish_backup "$batch_dir"

printf '备份完成: %s\n' "$(realpath -- "$batch_dir")"
printf '后续: 各节点跑同批次的 backup-agent.sh，收集核对办法见 maintenance.md。\n'
