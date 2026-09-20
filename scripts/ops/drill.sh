#!/usr/bin/env bash
#
# LabBridge 部署运维演练（Phase 028-05 收尾）：在一台机器上搭一个独立的
# "中心 Compose + 两个现场 Agent"环境，把技术方案 §7 的验收场景挨个走一遍。
#
# 隔离方式：演练项目名固定 labbridge-drill，靠 COMPOSE_PROJECT_NAME 覆盖
# deploy/production/compose.yaml 里的 name:，和真实的 labbridge-center 项目
# 互不影响（各自的容器、网络、数据卷）。凭据、端口（默认 18081）、备份目录
# 都在工作目录里。Agent 用打包产物里的二进制以普通进程跑在本机，目录布局
# 照 agent-deployment.md 的约定缩小进工作目录。
#
# 场景命令按下面列出的顺序执行，后面的场景依赖前面的状态；任一场景失败
# 会直接停住并保留环境（down/clean 都得手动执行），方便排查。
#
# 用法:
#   bash scripts/ops/drill.sh <命令> [选项] [--workdir D]
#
# 环境命令:
#   up --image-tag T --agent-package D [--images D]
#          从保存产物安装演练环境：装载/校验镜像、生成凭据、写环境文件、
#          compose up、布置 Agent 二进制（releases/<revision> + current）、
#          启动两个 Agent 并建好数据源/QC 规则/每分钟任务。
#            --image-tag     中心镜像 tag（保存产物清单里记录的）
#            --agent-package Agent 安装包目录（package-agent.sh 的产物）
#            --images        镜像导出目录（export-images.sh 的产物），
#                            给了就先 docker load，验证 tar 包能装
#   down     停两个 Agent 和中心服务，数据都保留
#   clean                     列出需管理员手动清理的路径，不批量删除
#   status   compose 服务、Agent 进程、队列待处理概览
#
# 工具命令:
#   api METHOD PATH [BODY]     以管理 token 经 Web 入口调中心 API
#   deliver NODE FILE [--count N] [--prefix P]
#                              模拟上游交付：先在扫描范围外备好，再以唯一
#                              文件名挪进节点 inbox（交付后不再改动）
#   agent-start NODE           启动 Agent（host 进程，走 current 链接）
#   agent-stop NODE [超时秒数] 正常停止（SIGTERM 等优雅退出，默认 60s）
#   agent-kill NODE            强杀（SIGKILL）
#   verify                     SQL 回验证据链 + 归档哈希 + 控制台深链接
#
# 场景命令（按顺序执行，过程和结论都追加进工作目录的 drill.log）:
#   scenario-e2e             输入交付至证据查询闭环（从零安装的验收部分）
#   scenario-upgrade [--new-images D] [--new-package D]
#                            完整备份后升级到新版本、回退、再升级；
#                            不给 --new-* 就现场构建当前源码的产物
#   scenario-offline         断网持续产生文件：start 阻塞 + 离线重启 + 恢复补交
#   scenario-fingerprint     指纹容量复现旧文件重采，验证输入移出策略能止住
#   scenario-space           限额存储上复现归档/队列写入失败，转移扩容后恢复
#   scenario-stops           正常停止与强杀的恢复核对
#   scenario-backup-restore  带未完成作业的完整备份 → 灾后恢复 → 原键续传
#   scenario-scale [--files N]
#                            代表规模（明确标注的模拟规模）实测停机/备份/恢复
#                            耗时与空间占用
#
# 面向 Ubuntu 24.04，依赖 GNU date/df/stat、docker、sqlite3、curl、python3、python3-yaml。
# 掉电、systemd 主机重启不在本脚本范围，按方案用隔离 Linux 环境另行验证。

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
cd "$repo_root"

usage() { awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }
die() { printf '%s\n' "$*" >&2; exit 1; }

PROJECT=labbridge-drill
COMPOSE_FILE=deploy/production/compose.yaml
WEB_PORT=18081
WORKDIR=/tmp/lb02805-drill
# 备份输出放在工作目录之外：环境文件/凭据都在工作目录里，备份工具会拒绝
# 把产物写进被备份内容，现场也一样（备份要落到别的盘/别的机器）。
BACKUPS_DIR=${WORKDIR}-backups
NODES=(drill-a drill-b)
# 全局导出：所有 docker compose 调用（包括 backup-center.sh / check.sh 里
# 发起的）都落到演练项目，绝不碰真实的 labbridge-center。
export COMPOSE_PROJECT_NAME=$PROJECT

# ---------------------------------------------------------------------------
# 输出与断言
# ---------------------------------------------------------------------------

scenario_failures=0
pass() { printf '  [PASS] %s\n' "$*"; }
fail() { printf '  [FAIL] %s\n' "$*"; scenario_failures=$((scenario_failures + 1)); }
note() { printf '        %s\n' "$*"; }
step() { printf '\n=== %s ===\n' "$*"; log_line "STEP $*"; }

# 演练时间线：关键动作和结论都记进去，收尾记录直接引用它。
log_line() {
  if [[ -d $WORKDIR ]]; then
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$WORKDIR/drill.log"
  fi
}

check_eq() {
  local label=$1 expected=$2 actual=$3
  if [[ $expected == "$actual" ]]; then
    pass "$label（$actual）"
  else
    fail "$label：期望 $expected，实际 $actual"
  fi
}
check_ge() {
  local label=$1 min=$2 actual=$3
  if (( actual >= min )); then
    pass "$label（$actual >= $min）"
  else
    fail "$label：$actual 小于 $min"
  fi
}

now_ms() { date +%s%3N; }
fmt_ms() { printf '%d.%03ds' $(( $1 / 1000 )) $(( $1 % 1000 )); }

finish_scenario() {
  local name=$1
  printf '\n=== %s 结果: ' "$name"
  if ((scenario_failures == 0)); then
    printf '通过 ===\n'
    log_line "SCENARIO $name PASS"
    exit 0
  fi
  printf '发现 %d 个问题（环境保留，可排查后重跑） ===\n' "$scenario_failures"
  log_line "SCENARIO $name FAIL failures=$scenario_failures"
  exit 1
}

# ---------------------------------------------------------------------------
# 中心访问
# ---------------------------------------------------------------------------

require_workdir() {
  [[ -r $WORKDIR/production.env ]] \
    || die "工作目录还没安装（缺 $WORKDIR/production.env），先执行 up；或用 --workdir 指对目录"
}

# 演练专用 compose 包装：永远带演练环境文件；项目名已全局导出
compose() {
  docker compose --env-file "$WORKDIR/production.env" -f "$COMPOSE_FILE" "$@"
}

# 容器内 psql 单值查询（去空白）
psql_q() {
  compose exec -T postgres psql -U labbridge -d labbridge -tAc "$1" | tr -d '[:space:]'
}

# 容器内 psql 原始输出（多行多列，制表符分隔）
psql_rows() {
  compose exec -T postgres psql -U labbridge -d labbridge -tA -F$'\t' -c "$1"
}

# 管理查询：经 Web 入口（和 Agent、浏览器同一条路），结果放 API_BODY / API_CODE
# Web 入口地址：优先用发布端口；个别环境（如 WSL 重启后）docker 的宿主机
# 端口发布会失灵，但容器网络本身是好的，这时退回 web 容器的直连地址。
web_entry() {
  local url="http://127.0.0.1:${WEB_PORT}" ip
  if ! curl -s --max-time 2 -o /dev/null "$url/"; then
    ip=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' \
      "${PROJECT}-web-1" 2>/dev/null || true)
    [[ -n $ip ]] && url="http://$ip:8080"
  fi
  printf '%s' "$url"
}

api() {
  local method=$1 path=$2 body=${3:-} out
  local -a args=(--silent --show-error --max-time 15 -X "$method"
    --header "Authorization: Bearer $(cat "$WORKDIR/auth/management.token")"
    -w '\n%{http_code}'
    "$(web_entry)${path}")
  if [[ -n $body ]]; then
    args+=(--header 'Content-Type: application/json' --data "$body")
  fi
  if ! out=$(curl "${args[@]}" 2>&1); then
    API_CODE=000; API_BODY=${out%%$'\n'*}; return 0
  fi
  API_CODE=${out##*$'\n'}
  API_BODY=${out%$'\n'*}
}

# 等中心出现至少 min 个满足条件的行；超时返回非零
wait_sql() {
  local sql=$1 min=$2 timeout=${3:-180} waited=0 got
  while :; do
    got=$(psql_q "$sql" 2>/dev/null || true)
    [[ -z $got ]] && got=0
    if (( got >= min )); then return 0; fi
    if (( waited >= timeout )); then return 1; fi
    sleep 5; waited=$((waited + 5))
  done
}

# 等两个节点心跳都新鲜（重启/切换后确认真正在线）
wait_heartbeats() {
  local timeout=${1:-90} waited=0 got
  while :; do
    got=$(psql_q "SELECT count(*) FROM nodes WHERE node_code IN ('drill-a','drill-b') AND last_heartbeat_at > now() - interval '30 seconds'" 2>/dev/null || true)
    [[ -z $got ]] && got=0
    if (( got >= 2 )); then return 0; fi
    if (( waited >= timeout )); then return 1; fi
    sleep 5; waited=$((waited + 5))
  done
}

# ---------------------------------------------------------------------------
# Agent 进程管理（drill-a / drill-b 是 host 进程；drill-c 是容器，见空间场景）
# ---------------------------------------------------------------------------

agent_pid_of() { cat "$WORKDIR/run/$1.pid" 2>/dev/null || true; }

agent_running() {
  local pid
  pid=$(agent_pid_of "$1")
  [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null
}

agent_binary() { readlink -f -- "$WORKDIR/opt/labbridge/current/labbridge_agent"; }

agent_start() {
  local node=$1
  if agent_running "$node"; then
    note "Agent $node 已在运行（pid $(agent_pid_of "$node")），跳过启动"
    return 0
  fi
  mkdir -p -- "$WORKDIR/run" "$WORKDIR/state/$node/work" "$WORKDIR/inbox/$node"
  nohup "$(agent_binary)" "$WORKDIR/etc/$node/agent.yaml" \
    >> "$WORKDIR/run/$node.log" 2>&1 &
  local pid=$!
  printf '%s\n' "$pid" > "$WORKDIR/run/$node.pid"
  sleep 2
  kill -0 "$pid" 2>/dev/null \
    || die "Agent $node 启动后立刻退出，看日志: $WORKDIR/run/$node.log"
  note "Agent $node 已启动（pid $pid，二进制 $(agent_binary)）"
}

agent_stop() {
  local node=$1 timeout=${2:-60} pid waited=0
  pid=$(agent_pid_of "$node")
  [[ -n $pid ]] || { note "Agent $node 没有 pid 记录，当作已停止"; return 0; }
  kill -TERM "$pid"
  while kill -0 "$pid" 2>/dev/null; do
    if (( waited >= timeout )); then
      fail "Agent $node ${timeout}s 内没有优雅退出（强杀不算正常停机）"
      return 1
    fi
    sleep 1; waited=$((waited + 1))
  done
  rm -f -- "$WORKDIR/run/$node.pid"
  note "Agent $node 已正常停止（等待 ${waited}s）"
}

agent_kill() {
  local node=$1 pid
  pid=$(agent_pid_of "$node")
  [[ -n $pid ]] || die "Agent $node 没有 pid 记录"
  kill -KILL "$pid" || true
  sleep 1
  rm -f -- "$WORKDIR/run/$node.pid"
  note "Agent $node 已被 SIGKILL"
}

# 队列只读查询：Agent 在跑用 mode=ro（能带上 WAL），停了退回 immutable
queue_q() {
  # 分两行声明：同一行里 db= 引用 $node 时赋值还没生效，会撞上外层同名变量
  local node=$1 sql=$2
  local db="$WORKDIR/state/$node/agent.db" uri
  [[ -f $db ]] || { printf '0'; return; }
  if agent_running "$node"; then
    uri="file:$db?mode=ro"
  else
    uri="file:$db?mode=ro&immutable=1"
  fi
  sqlite3 -batch -noheader "$uri" "$sql" 2>/dev/null || true
}

queue_pending() { queue_q "$1" 'SELECT count(*) FROM pending_jobs'; }

wait_queue_empty() {
  local node=$1 timeout=${2:-180} waited=0
  while :; do
    if (( $(queue_pending "$node") == 0 )); then return 0; fi
    if (( waited >= timeout )); then return 1; fi
    sleep 5; waited=$((waited + 5))
  done
}

# 模拟上游交付：文件先在扫描范围外备好，再用唯一名字挪进 inbox。
# 交付后不再改动，对应现场"不可变文件"的约定。
deliver() {
  local node=$1 fixture=$2 count=1 prefix=batch
  shift 2
  while (($#)); do
    case $1 in
      --count) count=$2; shift 2 ;;
      --prefix) prefix=$2; shift 2 ;;
      *) die "deliver 未知选项: $1" ;;
    esac
  done
  [[ -r $fixture ]] || die "交付源文件不可读: $fixture"
  mkdir -p -- "$WORKDIR/staging" "$WORKDIR/inbox/$node"
  local i name
  for ((i = 1; i <= count; i++)); do
    name="${prefix}-$(date +%Y%m%dT%H%M%S)-$RANDOM-$i.csv"
    cp -- "$fixture" "$WORKDIR/staging/$name"
    mv -- "$WORKDIR/staging/$name" "$WORKDIR/inbox/$node/$name"
  done
  note "已向 $node 交付 ${count} 个文件（前缀 $prefix）"
}

# 生成一个内容唯一的观测 CSV：offset 用来错开时间戳，保证文件哈希互不相同
# （指纹按内容算，同内容文件在指纹场景里分不出彼此）。
make_observation_csv() {
  local path=$1 rows=$2 offset=$3
  python3 - "$path" "$rows" "$offset" <<'PY'
import sys
path, rows, offset = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, 'w') as f:
    f.write('station_code,device_code,record_time,temperature,humidity\n')
    for i in range(rows):
        ts = offset + i * 5
        minute, second = divmod(ts, 60)
        hour, minute = divmod(minute, 60)
        f.write('DRILL,DV001,2026-09-18 %02d:%02d:%02d,22.%d,57\n'
                % (hour % 24, minute, second, i % 10))
PY
}

# ---------------------------------------------------------------------------
# 证据链回验（场景和 verify 命令共用）
# ---------------------------------------------------------------------------

verify_evidence() {
  step '证据链回验（task_runs → raw_files → parsed_records → qc_results → 回执）'
  check_eq 'raw_files 全部挂在有效运行下（无孤儿）' 0 \
    "$(psql_q 'SELECT count(*) FROM raw_files rf LEFT JOIN task_runs tr ON tr.id = rf.task_run_id WHERE tr.id IS NULL')"
  check_eq 'parsed_records 全部挂在有效原始文件下' 0 \
    "$(psql_q 'SELECT count(*) FROM parsed_records p LEFT JOIN raw_files rf ON rf.id = p.raw_file_id WHERE rf.id IS NULL AND p.raw_file_id IS NOT NULL')"
  check_eq 'qc_results 全部挂在有效解析记录下' 0 \
    "$(psql_q 'SELECT count(*) FROM qc_results q LEFT JOIN parsed_records p ON p.id = q.parsed_record_id WHERE p.id IS NULL')"
  check_eq '幂等回执无重复键' 0 \
    "$(psql_q 'SELECT count(*) FROM (SELECT node_id, request_type, idempotency_key FROM agent_report_receipts GROUP BY 1,2,3 HAVING count(*) > 1) t')"

  local runs succeeded failed raws parsed qc alerts
  runs=$(psql_q 'SELECT count(*) FROM task_runs')
  succeeded=$(psql_q "SELECT count(*) FROM task_runs WHERE status = 'succeeded'")
  failed=$(psql_q "SELECT count(*) FROM task_runs WHERE status = 'failed'")
  raws=$(psql_q 'SELECT count(*) FROM raw_files')
  parsed=$(psql_q 'SELECT count(*) FROM parsed_records')
  qc=$(psql_q 'SELECT count(*) FROM qc_results')
  alerts=$(psql_q 'SELECT count(*) FROM alerts')
  note "运行 $runs（成功 $succeeded / 失败 $failed）、原始文件 $raws、解析记录 $parsed、QC 结果 $qc、告警 $alerts"
  log_line "evidence runs=$runs succeeded=$succeeded failed=$failed raw=$raws parsed=$parsed qc=$qc alerts=$alerts"

  step '归档 SHA-256 与中心记录核对（host 节点）'
  local node bad checked total_bad=0 id hash path
  for node in "${NODES[@]}"; do
    bad=0; checked=0
    while IFS=$'\t' read -r id hash path; do
      [[ -n $id ]] || continue
      if [[ ! -f $path ]]; then
        bad=$((bad + 1)); note "$node 原始文件 #$id 归档缺失: $path"
        continue
      fi
      checked=$((checked + 1))
      if [[ $(sha256sum "$path" | cut -d' ' -f1) != "$hash" ]]; then
        bad=$((bad + 1)); note "$node 原始文件 #$id 哈希不一致: $path"
      fi
    done < <(psql_rows "SELECT rf.id, rf.file_hash, rf.storage_path
      FROM raw_files rf JOIN nodes n ON n.id = rf.node_id
      WHERE n.node_code = '$node' AND rf.file_hash IS NOT NULL")
    total_bad=$((total_bad + bad))
    note "$node: 核对 $checked 个归档哈希，不一致 $bad 个"
    log_line "archive-hash node=$node checked=$checked bad=$bad"
  done
  check_eq '归档哈希全部一致' 0 "$total_bad"

  step '控制台与查询能力（深链接走 Web 入口）'
  local code path entry
  entry=$(web_entry)
  for path in / /nodes /tasks; do
    code=$(curl -s -o /dev/null -w '%{http_code}' "$entry$path")
    check_eq "SPA 深链接 $path 返回 200" 200 "$code"
  done
  api GET /api/v1/nodes?limit=5
  check_eq '带管理 token 的节点查询返回 200' 200 "$API_CODE"
}

# ---------------------------------------------------------------------------
# 备份与恢复（备份恢复场景和规模场景共用）
# ---------------------------------------------------------------------------

# 完整备份：停 Agent → 停 server/web → 中心备份 → 各节点备份 → 收集核对。
# 调用方负责在调用前安排好想要的积压状态。
do_full_backup() {
  local label=$1
  local backup_id="${label}-$(date +%Y%m%dT%H%M)"
  local node
  for node in "${NODES[@]}"; do
    agent_running "$node" && agent_stop "$node"
  done
  compose stop server web >/dev/null
  local started=$(now_ms)
  mkdir -p -- "$BACKUPS_DIR/$label"
  bash scripts/ops/backup-center.sh --backup-id "$backup_id" \
    --output-root "$BACKUPS_DIR/$label/center" \
    --env-file "$WORKDIR/production.env" --compose "$COMPOSE_FILE" >/dev/null
  local center_ms=$(( $(now_ms) - started ))
  for node in "${NODES[@]}"; do
    bash scripts/ops/backup-agent.sh --backup-id "$backup_id" \
      --output-root "$BACKUPS_DIR/$label/$node" \
      --config "$WORKDIR/etc/$node/agent.yaml" >/dev/null
  done
  note "备份 $backup_id：中心 $(fmt_ms $center_ms)、中心+两节点共 $(fmt_ms $(( $(now_ms) - started )) )"

  # 收集核对：三部分哈希各自校验，都有 manifest 才算完整批次
  local part ok=1
  for part in center "${NODES[@]}"; do
    if ! (cd "$BACKUPS_DIR/$label/$part/$backup_id" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
      fail "$part 备份哈希校验失败"; ok=0
    fi
    if [[ ! -f $BACKUPS_DIR/$label/$part/$backup_id/manifest.txt ]]; then
      fail "$part 批次缺 manifest，不算完整批次"; ok=0
    fi
  done
  if ((ok)); then pass "批次 $backup_id 完整（中心 + ${NODES[*]} 哈希全过）"; fi
  BACKUP_ID=$backup_id
  BACKUP_ROOT=$BACKUPS_DIR/$label
}

# 恢复中心：当前库留证 → 拆栈删卷 → 起新库 → 清 initdb 预置 schema →
# pg_restore → 逐表行数对备份 manifest
restore_center() {
  local backup_id=$1 root=$2
  compose exec -T postgres pg_dump -U labbridge -d labbridge --format=custom \
    > "$root/pre-restore_$backup_id.pgdump"
  compose down >/dev/null 2>&1 || true
  docker volume rm "${PROJECT}_postgres_data" >/dev/null
  # 新卷要完整跑一遍 initdb，--wait 等 healthcheck 过了再连，否则 psql 会撞上拒连
  compose up -d --wait postgres >/dev/null
  compose exec -T postgres psql -U labbridge -d labbridge \
    -c 'DROP SCHEMA public CASCADE; CREATE SCHEMA public;' >/dev/null
  compose exec -T postgres pg_restore -U labbridge -d labbridge \
    --exit-on-error --no-owner --no-privileges \
    < "$root/center/$backup_id/center.pgdump"
  compose up -d --wait >/dev/null

  local mismatch=0 table expected actual
  while IFS='=' read -r table expected; do
    [[ -n $table ]] || continue
    actual=$(psql_q "SELECT count(*) FROM $table")
    if [[ $expected != "$actual" ]]; then
      fail "表 $table 行数不一致：备份 $expected，恢复后 $actual"; mismatch=1
    fi
  done < <(sed -n 's/^  \([a-z_]*\)=\([0-9]*\)$/\1=\2/p' "$root/center/$backup_id/manifest.txt")
  if ((mismatch == 0)); then pass '中心恢复行数与备份 manifest 逐表一致'; fi
}

# 恢复节点：旧目录留证 → tar 落回原绝对路径 → 队列完整性核对
restore_agents() {
  local backup_id=$1 root=$2 node integrity
  for node in "${NODES[@]}"; do
    if [[ -d $WORKDIR/state/$node ]]; then
      mv -- "$WORKDIR/state/$node" "$WORKDIR/state/$node.pre-restore_$backup_id"
    fi
  done
  for node in "${NODES[@]}"; do
    tar -xpf "$root/$node/$backup_id/agent-state.tar" -C /
    tar -xpf "$root/$node/$backup_id/input.tar" -C /
  done
  for node in "${NODES[@]}"; do
    integrity=$(sqlite3 -batch "file:$WORKDIR/state/$node/agent.db?mode=ro&immutable=1" \
      'PRAGMA integrity_check;' 2>/dev/null || true)
    check_eq "$node 队列库 integrity_check" ok "$integrity"
  done
}

# ---------------------------------------------------------------------------
# up / down / clean / status
# ---------------------------------------------------------------------------

# 给一个节点建数据源 + 每分钟任务；QC 规则是全局的，只建一次
setup_node_tasks() {
  local node=$1 root=$2 rule_required rule_timestamp source_id task_id
  if (( $(psql_q "SELECT count(*) FROM qc_rules WHERE name = 'drill-required-fields'") == 0 )); then
    api POST /api/v1/qc-rules '{"name":"drill-required-fields","rule_type":"required_fields","config":{},"enabled":true}'
    [[ $API_CODE == 20* ]] || die "建 QC 规则失败: HTTP $API_CODE $API_BODY"
  fi
  if (( $(psql_q "SELECT count(*) FROM qc_rules WHERE name = 'drill-timestamp'") == 0 )); then
    api POST /api/v1/qc-rules '{"name":"drill-timestamp","rule_type":"basic_timestamp_format","config":{},"enabled":true}'
    [[ $API_CODE == 20* ]] || die "建 QC 规则失败: HTTP $API_CODE $API_BODY"
  fi
  rule_required=$(psql_q "SELECT id FROM qc_rules WHERE name = 'drill-required-fields'")
  rule_timestamp=$(psql_q "SELECT id FROM qc_rules WHERE name = 'drill-timestamp'")

  api POST /api/v1/data-sources "$(python3 - "$node" "$root" <<'PY'
import json, sys
node, root = sys.argv[1:3]
print(json.dumps({
    "node_code": node, "source_type": "local_directory",
    "name": node + " inbox",
    "config": {"root_path": root, "extension": ".csv"},
    "enabled": True}))
PY
)"
  [[ $API_CODE == 20* ]] || die "$node 建数据源失败: HTTP $API_CODE $API_BODY"
  # id 直接从库里读，接口返回体结构不拿来当依赖
  source_id=$(psql_q "SELECT ds.id FROM data_sources ds JOIN nodes n ON n.id = ds.node_id WHERE n.node_code = '$node' ORDER BY ds.id DESC LIMIT 1")
  api POST /api/v1/tasks "$(python3 - "$node" "$source_id" "$rule_required" "$rule_timestamp" <<'PY'
import json, sys
node, source_id, r1, r2 = sys.argv[1:5]
print(json.dumps({
    "node_code": node, "data_source_id": source_id,
    "name": node + " csv import", "task_type": "local_file_import",
    "schedule_expr": "* * * * *", "parser_type": "csv_observation",
    "qc_profile": "default", "qc_rule_ids": [r1, r2], "enabled": True}))
PY
)"
  [[ $API_CODE == 20* ]] || die "$node 建任务失败: HTTP $API_CODE $API_BODY"
  task_id=$(psql_q "SELECT t.id FROM tasks t JOIN nodes n ON n.id = t.node_id WHERE n.node_code = '$node' ORDER BY t.id DESC LIMIT 1")
  note "$node: 数据源 #$source_id、任务 #$task_id、QC 规则 #$rule_required #$rule_timestamp"
}

cmd_up() {
  local image_tag='' agent_package='' images_dir=''
  while (($#)); do
    case $1 in
      --image-tag) image_tag=$2; shift 2 ;;
      --agent-package) agent_package=$2; shift 2 ;;
      --images) images_dir=$2; shift 2 ;;
      *) die "up 未知选项: $1" ;;
    esac
  done
  [[ -n $image_tag && -n $agent_package ]] \
    || die "up 需要 --image-tag <保存产物的镜像 tag> 和 --agent-package <Agent 安装包目录>"

  for tool in docker sqlite3 curl python3 openssl; do
    command -v "$tool" >/dev/null 2>&1 || die "缺少 $tool"
  done
  [[ -e $WORKDIR/production.env ]] && die "$WORKDIR 已安装过，先 clean 再来"

  # auth 目录留给 generate-credentials.sh 自己建（它拒绝覆盖已有目录）
  mkdir -p -- "$WORKDIR"/{etc,staging,artifacts,run,removed,opt/labbridge/releases}

  step '装载与校验保存产物'
  if [[ -n $images_dir ]]; then
    [[ -d $images_dir ]] || die "--images 目录不存在: $images_dir"
    local tar
    for tar in "$images_dir"/images/*.tar; do
      [[ -e $tar ]] || continue
      docker load -i "$tar" >/dev/null
      note "已装载 $(basename -- "$tar")"
    done
    if (cd "$images_dir" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
      pass '镜像导出产物 SHA-256 清单校验通过'
    else
      fail '镜像导出产物校验失败'
    fi
  fi
  docker image inspect "labbridge/server:$image_tag" >/dev/null 2>&1 \
    || die "本地没有 labbridge/server:$image_tag；先 docker load 或现场构建"
  [[ -x $agent_package/bin/labbridge_agent ]] || die "Agent 包里没有二进制: $agent_package/bin/labbridge_agent"
  local revision
  revision=$(sed -n 's/^source_revision: //p' "$agent_package/manifest.txt")
  [[ -n $revision ]] || revision=unknown
  if (cd "$agent_package" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
    pass "Agent 安装包校验通过（revision ${revision:0:8}）"
  else
    fail 'Agent 安装包校验失败'
  fi

  step '生成演练凭据与环境文件'
  bash deploy/production/generate-credentials.sh "$WORKDIR/auth" drill-a drill-b drill-c >/dev/null
  {
    printf 'LABBRIDGE_POSTGRES_PASSWORD=%s\n' "$(openssl rand -hex 24)"
    printf 'LABBRIDGE_IMAGE_TAG=%s\n' "$image_tag"
    printf 'LABBRIDGE_AUTH_DIR=%s\n' "$WORKDIR/auth"
    printf 'LABBRIDGE_AUTH_GROUP=%s\n' "$(id -g)"
    printf 'LABBRIDGE_WEB_BIND_ADDR=127.0.0.1\n'
    printf 'LABBRIDGE_WEB_PORT=%s\n' "$WEB_PORT"
  } > "$WORKDIR/production.env"
  chmod 0600 "$WORKDIR/production.env"

  step '中心三件套起来并等健康'
  compose up -d --wait >/dev/null
  local running
  running=$(compose ps --status running --services | sort | tr '\n' ' ')
  check_eq 'postgres/server/web 全部 running' 'postgres server web ' "$running"
  api GET /api/v1/nodes?limit=1
  check_eq '认证查询经 Web 入口返回 200' 200 "$API_CODE"

  step '布置 Agent 二进制（releases/<revision> + current 链接）'
  mkdir -p -- "$WORKDIR/opt/labbridge/releases/$revision"
  install -m 0755 -- "$agent_package/bin/labbridge_agent" \
    "$WORKDIR/opt/labbridge/releases/$revision/labbridge_agent"
  ln -sfn -- "$WORKDIR/opt/labbridge/releases/$revision" "$WORKDIR/opt/labbridge/current"

  step '写两个节点的正式风格配置'
  local node capacity entry
  entry=$(web_entry)
  note "Agent 的中心入口: $entry"
  for node in "${NODES[@]}"; do
    # drill-b 的指纹容量故意压到 2，指纹场景用它复现旧文件重采
    capacity=10000
    [[ $node == drill-b ]] && capacity=2
    mkdir -p -- "$WORKDIR/etc/$node" "$WORKDIR/state/$node/work" "$WORKDIR/inbox/$node"
    cat > "$WORKDIR/etc/$node/agent.yaml" <<EOF
agent:
  node_code: $node
  name: $node
  server_url: $entry
  request_timeout_seconds: 5
  heartbeat_interval_seconds: 5
  token_file: $WORKDIR/auth/$node.token

storage:
  queue_db: $WORKDIR/state/$node/agent.db
  work_dir: $WORKDIR/state/$node/work
  max_pending_jobs: 1000
  processed_fingerprint_capacity_per_task: $capacity

delivery:
  # 演练把重试间隔压短，恢复阶段不用干等指数退避；语义与正式配置一致
  retry_initial_seconds: 1
  retry_max_seconds: 15

tasks:
  poll_interval_seconds: 5
  allowed_local_roots:
    - $WORKDIR/inbox/$node
EOF
  done

  step '启动 Agent 并等注册心跳'
  local started=$(now_ms)
  for node in "${NODES[@]}"; do agent_start "$node"; done
  if wait_sql "SELECT count(*) FROM nodes WHERE node_code IN ('drill-a','drill-b')" 2 60; then
    pass "两个节点已注册（耗时 $(fmt_ms $(( $(now_ms) - started )) )）"
  else
    fail '60s 内没有等到两个节点注册'
  fi

  step '建数据源、QC 规则和每分钟任务'
  setup_node_tasks drill-a "$WORKDIR/inbox/drill-a"
  setup_node_tasks drill-b "$WORKDIR/inbox/drill-b"

  log_line "up done image_tag=$image_tag agent_revision=$revision"
  printf '\n演练环境就绪: %s（Web 入口 http://127.0.0.1:%s）\n' "$WORKDIR" "$WEB_PORT"
}

cmd_down() {
  require_workdir
  local node
  for node in "${NODES[@]}"; do
    if agent_running "$node"; then agent_stop "$node" || true; fi
  done
  compose stop server web >/dev/null
  note 'Agent 与 server/web 已停止（postgres 保留，数据不动）'
}

cmd_clean() {
  printf '项目禁止批量删除，clean 不执行清理。请先停止演练，再由管理员手动处理：\n'
  printf '  工作目录: %s\n  备份目录: %s\n  数据卷: %s_postgres_data\n' "$WORKDIR" "$BACKUPS_DIR" "$PROJECT"
  return 1
}

# 重跑前只把明确的演练路径改名留证，不批量删除文件或目录。
retain_drill_path() {
  local path=$1 root_real path_real saved
  root_real=$(realpath -m -- "$WORKDIR")
  path_real=$(realpath -m -- "$path")
  [[ $path_real == "$root_real"/* ]] || die "拒绝移动演练目录外路径: $path"
  if [[ -e $path || -L $path ]]; then
    saved="${path}.retained-$(date +%s%N)"
    mv -T -- "$path" "$saved"
    note "旧现场已留存: $saved"
  fi
}

cmd_status() {
  require_workdir
  printf '=== 演练环境 %s（项目 %s） ===\n' "$WORKDIR" "$PROJECT"
  compose ps
  local node pid pending
  for node in "${NODES[@]}"; do
    pid=$(agent_pid_of "$node")
    if agent_running "$node"; then
      pending=$(queue_pending "$node")
      printf 'Agent %-8s 运行中 pid=%s 待处理=%s\n' "$node" "$pid" "$pending"
    else
      printf 'Agent %-8s 未运行\n' "$node"
    fi
  done
  docker ps --filter name=labridge-drill-agent-c --format 'Agent drill-c 容器: {{.Status}}' | head -1
}

# ---------------------------------------------------------------------------
# 工具命令
# ---------------------------------------------------------------------------

cmd_api() {
  require_workdir
  (($# >= 2)) || die "用法: api METHOD PATH [BODY]"
  api "$1" "$2" "${3:-}"
  printf '%s\n' "$API_BODY"
  printf 'HTTP %s\n' "$API_CODE"
}

cmd_deliver() {
  require_workdir
  (($# >= 2)) || die "用法: deliver NODE FILE [--count N] [--prefix P]"
  deliver "$@"
}

cmd_agent() {
  require_workdir
  local action=$1 node=${2:-}
  [[ -n $node ]] || die "缺少节点名（drill-a / drill-b）"
  case $action in
    agent-start) agent_start "$node" ;;
    agent-stop) agent_stop "$node" "${3:-60}" ;;
    agent-kill) agent_kill "$node" ;;
    *) die "未知命令: $action" ;;
  esac
}

cmd_verify() {
  require_workdir
  verify_evidence
  finish_scenario verify
}

# ---------------------------------------------------------------------------
# 场景 1：输入交付至证据查询闭环
# ---------------------------------------------------------------------------

cmd_scenario_e2e() {
  require_workdir
  step '场景 1：输入交付至证据查询闭环'
  deliver drill-a tests/fixtures/ops/observation-normal.csv --prefix e2e-ok
  deliver drill-a tests/fixtures/ops/observation-qc-flag.csv --prefix e2e-qc
  local started=$(now_ms)
  if wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'e2e-ok-%'" 1 180 \
    && wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'e2e-qc-%'" 1 60; then
    pass "两个文件都进入证据链（耗时 $(fmt_ms $(( $(now_ms) - started )) )）"
  else
    fail '180s 内没有等到两个文件入证据链'
  fi
  wait_queue_empty drill-a 60 || note 'drill-a 队列还有残留作业'

  local failed_qc
  failed_qc=$(psql_q "SELECT count(*) FROM qc_results WHERE result != 'pass' AND parsed_record_id IN (SELECT p.id FROM parsed_records p JOIN raw_files rf ON rf.id = p.raw_file_id WHERE rf.original_name LIKE 'e2e-qc-%')")
  check_ge '坏行被 QC 拦出失败结果' 2 "$failed_qc"
  verify_evidence
  finish_scenario scenario-e2e
}

# ---------------------------------------------------------------------------
# 场景 2：升级与回退
# ---------------------------------------------------------------------------

cmd_scenario_upgrade() {
  require_workdir
  step '场景 2：升级、回退、再升级（新旧两套产物）'
  local new_images='' new_package=''
  while (($#)); do
    case $1 in
      --new-images) new_images=$2; shift 2 ;;
      --new-package) new_package=$2; shift 2 ;;
      *) die "scenario-upgrade 未知选项: $1" ;;
    esac
  done

  local old_tag old_rev node
  old_tag=$(sed -n 's/^LABBRIDGE_IMAGE_TAG=//p' "$WORKDIR/production.env")
  old_rev=$(basename -- "$(readlink -f -- "$WORKDIR/opt/labbridge/current")")
  note "当前版本：镜像 $old_tag / Agent $old_rev"

  step '准备新产物（没给目录就现场构建当前源码）'
  if [[ -z $new_images || -z $new_package ]]; then
    local build_ms=$(now_ms)
    [[ -d $WORKDIR/artifacts/images-new ]] || bash scripts/ops/export-images.sh "$WORKDIR/artifacts/images-new"
    [[ -d $WORKDIR/artifacts/package-new ]] || bash scripts/ops/package-agent.sh "$WORKDIR/artifacts/package-new"
    note "构建/导出新产物耗时 $(fmt_ms $(( $(now_ms) - build_ms )) )"
  fi
  new_images=${new_images:-$WORKDIR/artifacts/images-new}
  new_package=${new_package:-$WORKDIR/artifacts/package-new}
  local new_tag new_rev
  new_tag=$(sed -n 's/^image_tag: //p' "$new_images/manifest.txt")
  new_rev=$(sed -n 's/^source_revision: //p' "$new_package/manifest.txt")
  [[ -n $new_tag && -n $new_rev ]] || die '新产物 manifest 缺 tag/revision，先检查产物目录'
  note "新版本：镜像 $new_tag / Agent $new_rev"
  docker load -i "$new_images"/images/labbridge-server_"$new_tag".tar >/dev/null
  docker load -i "$new_images"/images/labbridge-web_"$new_tag".tar >/dev/null
  mkdir -p -- "$WORKDIR/opt/labbridge/releases/$new_rev"
  install -m 0755 -- "$new_package/bin/labbridge_agent" \
    "$WORKDIR/opt/labbridge/releases/$new_rev/labbridge_agent"

  step '升级前完整备份（没有当次完整备份不动手）'
  do_full_backup pre-upgrade

  step '中心切换新镜像，Agent 切新二进制（releases + current + 重启）'
  local switch_ms=$(now_ms)
  sed -i "s/^LABBRIDGE_IMAGE_TAG=.*/LABBRIDGE_IMAGE_TAG=$new_tag/" "$WORKDIR/production.env"
  compose up -d --wait >/dev/null
  for node in "${NODES[@]}"; do agent_stop "$node"; done
  ln -sfn -- "$WORKDIR/opt/labbridge/releases/$new_rev" "$WORKDIR/opt/labbridge/current"
  for node in "${NODES[@]}"; do agent_start "$node"; done
  note "切换耗时 $(fmt_ms $(( $(now_ms) - switch_ms )) )"
  api GET /api/v1/nodes?limit=1
  check_eq '新镜像中心认证查询 200' 200 "$API_CODE"
  wait_heartbeats 90 && pass '升级后两个节点心跳恢复' || fail '升级后心跳没恢复'

  step '升级后核对旧证据 + 新文件闭环'
  verify_evidence
  deliver drill-a tests/fixtures/ops/observation-normal.csv --prefix upgrade-new
  if wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'upgrade-new-%'" 1 180; then
    pass '新版本上旧数据可查、新文件完成闭环'
  else
    fail '新版本上没有等到新文件入证据链'
  fi

  step '回退：切回旧镜像和旧二进制（数据不动）'
  local rollback_ms=$(now_ms)
  sed -i "s/^LABBRIDGE_IMAGE_TAG=.*/LABBRIDGE_IMAGE_TAG=$old_tag/" "$WORKDIR/production.env"
  compose up -d --wait >/dev/null
  for node in "${NODES[@]}"; do agent_stop "$node"; done
  ln -sfn -- "$WORKDIR/opt/labbridge/releases/$old_rev" "$WORKDIR/opt/labbridge/current"
  for node in "${NODES[@]}"; do agent_start "$node"; done
  note "回退耗时 $(fmt_ms $(( $(now_ms) - rollback_ms )) )"
  wait_heartbeats 90 || fail '回退后心跳没恢复'
  verify_evidence
  deliver drill-a tests/fixtures/ops/observation-normal.csv --prefix rollback-old
  if wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'rollback-old-%'" 1 180; then
    pass '回退版本上数据保留、新文件完成闭环'
  else
    fail '回退版本上没有等到新文件入证据链'
  fi

  step '再升级回新版本（后续场景都在新版本上跑）'
  sed -i "s/^LABBRIDGE_IMAGE_TAG=.*/LABBRIDGE_IMAGE_TAG=$new_tag/" "$WORKDIR/production.env"
  compose up -d --wait >/dev/null
  for node in "${NODES[@]}"; do agent_stop "$node"; done
  ln -sfn -- "$WORKDIR/opt/labbridge/releases/$new_rev" "$WORKDIR/opt/labbridge/current"
  for node in "${NODES[@]}"; do agent_start "$node"; done
  wait_heartbeats 90 || fail '再升级后心跳没恢复'
  log_line "upgrade old=$old_tag/$old_rev new=$new_tag/$new_rev"
  finish_scenario scenario-upgrade
}

# ---------------------------------------------------------------------------
# 场景 3：断网持续产生文件
# ---------------------------------------------------------------------------

cmd_scenario_offline() {
  require_workdir
  step '场景 3：断网期间持续产生文件（start 阻塞 + 离线重启 + 恢复补交）'

  step '断开中心（停 server/web，模拟网络不可达），上游继续交付 3 个文件'
  compose stop server web >/dev/null
  local i
  for i in 1 2 3; do
    make_observation_csv "$WORKDIR/staging/offline-$i.csv" 5 $((i * 1000 + RANDOM))
    deliver drill-a "$WORKDIR/staging/offline-$i.csv" --prefix offline-$i
  done

  step '等待 start 阻塞：作业入队、反复重试'
  local waited=0 stage_summary=''
  while ((waited < 90)); do
    stage_summary=$(queue_q drill-a "SELECT stage || '=' || count(*) FROM pending_jobs GROUP BY stage" | tr '\n' ' ')
    [[ $stage_summary == *pending* ]] && break
    sleep 5; waited=$((waited + 5))
  done
  check_ge '断网下作业入队（start_pending/retry_wait）' 1 "$(queue_pending drill-a)"
  note "队列阶段: ${stage_summary:-空}"
  check_ge '已留下失败投递记录' 1 "$(queue_q drill-a "SELECT count(*) FROM delivery_attempts WHERE outcome != 'success'")"

  step '断网状态下重启 Agent（离线重启，队列必须恢复）'
  agent_stop drill-a
  agent_start drill-a
  sleep 5
  check_ge '离线重启后作业仍在队列里' 1 "$(queue_pending drill-a)"

  step '巡检在断网下能指出问题'
  local check_out
  check_out=$(bash scripts/ops/check.sh agent --config "$WORKDIR/etc/drill-a/agent.yaml" --max-pending-age-seconds 1 2>&1 || true)
  if grep -q '最老作业已等待' <<<"$check_out"; then
    pass 'check.sh agent 指出最老作业超龄'
  else
    fail 'check.sh agent 没有指出积压'
    sed 's/^/        /' <<<"$check_out"
  fi
  log_line "offline queue-stage: ${stage_summary:-empty}"

  step '恢复中心，等积压全部补交'
  compose up -d --wait >/dev/null
  local started=$(now_ms)
  if wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'offline-%'" 3 240; then
    pass "断网期间 3 个文件恢复后全部入证据链（补交耗时 $(fmt_ms $(( $(now_ms) - started )) )）"
  else
    fail '恢复后 240s 内没有等到 3 个 offline 文件'
  fi
  wait_queue_empty drill-a 120 && pass 'drill-a 队列清空' || fail 'drill-a 队列还有残留'
  # 同一个文件名出现两份证据才是真重复；不同文件名内容相同属于上游重复交付，是允许的
  check_eq 'offline 文件没有重复证据（同名只入链一次）' 0 \
    "$(psql_q "SELECT count(*) FROM (SELECT rf.node_id, rf.original_name, count(*) c FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'offline-%' GROUP BY 1,2 HAVING count(*) > 1) t")"
  verify_evidence
  finish_scenario scenario-offline
}

# ---------------------------------------------------------------------------
# 场景 4：指纹容量
# ---------------------------------------------------------------------------

cmd_scenario_fingerprint() {
  require_workdir
  step '场景 4：指纹容量复现旧文件重采，输入移出策略止住重复'

  step 'drill-b（容量 2）逐个交付 3 个内容互不相同的文件'
  local i
  for i in 1 2 3; do
    make_observation_csv "$WORKDIR/staging/fp-$i.csv" 5 $((i * 137 + RANDOM))
    deliver drill-b "$WORKDIR/staging/fp-$i.csv" --prefix fp-$i
    if ! wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-b' AND rf.original_name LIKE 'fp-$i-%'" 1 180; then
      fail "fp-$i 没有被采集"
      finish_scenario scenario-fingerprint
    fi
    note "fp-$i 已入证据链"
  done

  step 'fp-1 已被容量淘汰且仍在 inbox：等下一轮复现重采'
  local recollected=0 waited=0
  while ((waited < 240)); do
    if (( $(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-b' AND rf.original_name LIKE 'fp-1-%'") >= 2 )); then
      recollected=1; break
    fi
    sleep 10; waited=$((waited + 10))
  done
  if ((recollected)); then
    pass '容量淘汰后旧文件被再次采集（fp-1 出现第 2 份证据）——文档已记载的已知风险'
  else
    fail '没有复现重采（要么指纹没淘汰，要么没等到下一轮）'
  fi
  log_line "fingerprint fp-1 recollected=$recollected"

  step '移出策略：把已处理文件移出扫描目录（约定责任方的动作）'
  local node=drill-b raw_now stable=0 waited=0
  mkdir -p -- "$WORKDIR/removed/$node"
  find "$WORKDIR/inbox/$node" -maxdepth 1 -name 'fp-*.csv' -exec mv -t "$WORKDIR/removed/$node/" {} +
  raw_now=$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-b'")
  # 连续两个调度周期（约 2 分钟）没有新证据，才算重复被止住
  while ((waited < 240)); do
    sleep 60; waited=$((waited + 60))
    local now
    now=$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-b'")
    if [[ $now == "$raw_now" ]]; then
      stable=$((stable + 1))
    else
      stable=0
      raw_now=$now
    fi
    if ((stable >= 2)); then break; fi
  done
  check_ge '移出后连续两轮无新证据（重复被止住）' 2 "$stable"
  note "drill-b 证据总数稳定在 $raw_now；移出的文件在 $WORKDIR/removed/$node"
  verify_evidence
  finish_scenario scenario-fingerprint
}

# ---------------------------------------------------------------------------
# 场景 5：空间不足与积压（drill-c 跑在限额 tmpfs 的容器里）
# ---------------------------------------------------------------------------

cmd_scenario_space() {
  require_workdir
  step '场景 5：限额存储上复现写入失败，转移扩容后恢复'
  local tag agent_image
  tag=$(sed -n 's/^LABBRIDGE_IMAGE_TAG=//p' "$WORKDIR/production.env")
  agent_image="labbridge/agent-package:$tag"
  docker image inspect "$agent_image" >/dev/null 2>&1 \
    || die "没有 Agent 镜像 $agent_image（package-agent.sh 构建后会打这个 tag）"

  # 历史轮次留下的原始文件行还在中心库里，断言全部用"本轮增量"，不数全库
  local first_before big_before
  first_before=$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-c' AND rf.original_name LIKE 'space-first-%'")
  big_before=$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-c' AND rf.original_name LIKE 'space-big-%'")

  step '准备 drill-c 新现场（旧状态和输入改名留证，任务复用）'
  docker rm -f labbridge-drill-agent-c >/dev/null 2>&1 || true
  retain_drill_path "$WORKDIR/state/drill-c"
  retain_drill_path "$WORKDIR/space-captured"
  retain_drill_path "$WORKDIR/run/drill-c.log"
  retain_drill_path "$WORKDIR/inbox/drill-c"
  mkdir -p -- "$WORKDIR/etc/drill-c" "$WORKDIR/inbox/drill-c" "$WORKDIR/state/drill-c" "$WORKDIR/space-captured"
  if [[ -n $(psql_q "SELECT t.id FROM tasks t JOIN nodes n ON n.id = t.node_id WHERE n.node_code = 'drill-c' LIMIT 1") ]]; then
    # 上一轮收尾时停用过任务，重新启用，免得每轮演练重复建任务
    local tid
    tid=$(psql_q "SELECT t.id FROM tasks t JOIN nodes n ON n.id = t.node_id WHERE n.node_code = 'drill-c' LIMIT 1")
    api PATCH "/api/v1/tasks/$tid" '{"enabled":true}'
    note "复用 drill-c 任务 #$tid（已重新启用）"
  fi
  cat > "$WORKDIR/etc/drill-c/agent.yaml" <<EOF
agent:
  node_code: drill-c
  name: drill-c
  # 容器里走 compose 网络直达 web 入口，和现场"server_url 填 Web 入口"同一个口径
  server_url: http://web:8080
  request_timeout_seconds: 5
  heartbeat_interval_seconds: 5
  token_file: /etc/labbridge/auth/agent.token

storage:
  queue_db: /var/lib/labbridge/agent.db
  work_dir: /var/lib/labbridge/work
  max_pending_jobs: 1000
  processed_fingerprint_capacity_per_task: 10000

delivery:
  retry_initial_seconds: 1
  retry_max_seconds: 15

tasks:
  poll_interval_seconds: 5
  allowed_local_roots:
    - /drill/inbox
EOF
  # 用宿主机用户身份跑，tmpfs 和后面恢复用的绑定目录才写得进去
  docker run -d --name labbridge-drill-agent-c \
    --network "${PROJECT}_default" \
    --user "$(id -u):$(id -g)" \
    --tmpfs /var/lib/labbridge:size=2m,mode=0777 \
    --volume "$WORKDIR/etc/drill-c/agent.yaml:/etc/labbridge/agent.yaml:ro" \
    --volume "$WORKDIR/auth/drill-c.token:/etc/labbridge/auth/agent.token:ro" \
    --volume "$WORKDIR/inbox/drill-c:/drill/inbox:ro" \
    "$agent_image" >/dev/null
  if [[ -z $(psql_q "SELECT t.id FROM tasks t JOIN nodes n ON n.id = t.node_id WHERE n.node_code = 'drill-c' LIMIT 1") ]]; then
    setup_node_tasks drill-c /drill/inbox
  fi
  wait_sql "SELECT count(*) FROM nodes WHERE node_code = 'drill-c'" 1 60 \
    && pass 'drill-c 已注册在线' || fail 'drill-c 60s 内没有注册'

  step '小文件先正常跑一轮（限额内一切正常）'
  make_observation_csv "$WORKDIR/staging/space-small.csv" 200 0
  deliver drill-c "$WORKDIR/staging/space-small.csv" --prefix space-first
  wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-c' AND rf.original_name LIKE 'space-first-%'" $((first_before + 1)) 200 \
    && pass '第一个文件正常归档' || { fail '第一个文件没有被采集'; finish_scenario scenario-space; }
  docker exec labbridge-drill-agent-c df -h /var/lib/labbridge | tail -1 | sed 's/^/        限额存储: /'

  step '用填充文件把限额存储挤到只剩 ~64KiB，再交付一个 ~430KiB 的大文件'
  # 只留 64KiB：归档副本和报告正文都写不进去，但小的失败上报还能落，
  # 这样"如实 failed + 不虚假成功"的行为是确定的
  local avail_kb fill_kb
  avail_kb=$(docker exec labbridge-drill-agent-c df -k /var/lib/labbridge | tail -1 | awk '{print $4}')
  fill_kb=$((avail_kb - 64))
  ((fill_kb > 0)) || fill_kb=0
  docker exec labbridge-drill-agent-c \
    sh -c "dd if=/dev/zero of=/var/lib/labbridge/filler.bin bs=1024 count=$fill_kb >/dev/null 2>&1 || true"
  docker exec labbridge-drill-agent-c df -h /var/lib/labbridge | tail -1 | sed 's/^/        挤压后: /'
  make_observation_csv "$WORKDIR/staging/space-big.csv" 11000 3000
  note "大文件 $(stat -c %s "$WORKDIR/staging/space-big.csv") 字节，剩余空间装不下它的归档副本"
  deliver drill-c "$WORKDIR/staging/space-big.csv" --prefix space-big

  step '限额写满后：如实失败，不许虚假成功'
  # 预期的诚实行为：归档副本写不进限额存储 → 失败报告（很小，内存放得下）
  # 送达中心 → 运行标记 failed。同时留意两种意外：投递层失败（冻结现场）
  # 和 Agent 直接退出（队列库故障按既有语义传播到进程边界）。
  local failed_before trouble=0 waited=0 failed_now
  failed_before=$(psql_q "SELECT count(*) FROM task_runs tr JOIN nodes n ON n.id = tr.node_id WHERE n.node_code = 'drill-c' AND tr.status = 'failed'")
  while ((waited < 240)); do
    failed_now=$(psql_q "SELECT count(*) FROM task_runs tr JOIN nodes n ON n.id = tr.node_id WHERE n.node_code = 'drill-c' AND tr.status = 'failed'")
    if ((failed_now > failed_before)); then trouble=3; break; fi
    if docker logs --since 3s labbridge-drill-agent-c 2>&1 | grep -qE 'delivery failed|ERROR'; then
      trouble=1; break
    fi
    if ! docker inspect -f '{{.State.Running}}' labbridge-drill-agent-c 2>/dev/null | grep -q true; then
      trouble=2; break
    fi
    sleep 2; waited=$((waited + 2))
  done
  docker logs labbridge-drill-agent-c > "$WORKDIR/run/drill-c.log" 2>&1 || true
  case $trouble in
    3)
      pass '大文件运行被如实标记 failed（失败报告送达中心）'
      psql_q "SELECT left(error_summary, 100) FROM task_runs tr JOIN nodes n ON n.id = tr.node_id WHERE n.node_code = 'drill-c' AND tr.status = 'failed' ORDER BY tr.id DESC LIMIT 1" \
        | sed 's/^/        失败摘要: /'
      ;;
    1)
      pass '捕获到投递/写入失败'
      ;;
    2)
      note 'Agent 按队列库故障语义退出（现场由 systemd 接管重启）；限额存储内容随容器停止丢失'
      log_line 'space: agent exited on queue write failure; tmpfs destroyed with container stop'
      ;;
    *) fail '240s 内没有观察到失败' ;;
  esac
  # 能冻结就冻结：进程停住、tmpfs 还在，现场可以完整转移。
  # 报告写队列时直接 SQLITE_FULL 的情况 Agent 是瞬时退出的，冻结会扑空。
  if ((trouble == 1 || trouble == 3)); then
    docker kill --signal SIGSTOP labbridge-drill-agent-c >/dev/null 2>&1 || true
  fi
  if grep -iqE 'no space|ENOSPC|database or disk is full' "$WORKDIR/run/drill-c.log"; then
    pass 'Agent 日志留下写入失败记录'
    grep -iE 'no space|ENOSPC|database or disk is full' "$WORKDIR/run/drill-c.log" | tail -2 | sed 's/^/        /'
  else
    note 'Agent 日志没有 ENOSPC 字样（失败已折叠进 failed 报告，见中心侧失败摘要）'
  fi
  check_eq '失败文件没有虚假成功证据（本轮无新增）' "$big_before" \
    "$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-c' AND rf.original_name LIKE 'space-big-%'")"
  check_eq '空间不足期间旧证据仍可查' "$((first_before + 1))" \
    "$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-c' AND rf.original_name LIKE 'space-first-%'")"

  step '巡检指出异常（中心侧：运行推进停在故障前）'
  local check_out
  check_out=$(bash scripts/ops/check.sh center --env-file "$WORKDIR/production.env" \
    --max-run-age-seconds 30 2>&1 || true)
  if grep -q 'drill-c' <<<"$check_out"; then
    pass 'check.sh center 的输出点名 drill-c'
  else
    fail 'check.sh center 没有覆盖 drill-c'
    sed 's/^/        /' <<<"$check_out"
  fi

  step '冻结现场转移出来（对应"人工转移已核验数据"），并重放被拒的请求'
  local integrity='' captured=0
  if docker inspect -f '{{.State.Running}}' labbridge-drill-agent-c 2>/dev/null | grep -q true; then
    # docker cp 拿不走 tmpfs 的内容（overlay 归档接口看不见 tmpfs），
    # 容器一停 tmpfs 也会被清空；只能先冻结进程、容器活着时打 tar 流出来
    docker kill --signal SIGSTOP labbridge-drill-agent-c >/dev/null 2>&1 || true
    if docker exec labbridge-drill-agent-c tar -C /var/lib/labbridge -cf - . \
      | tar -C "$WORKDIR/space-captured" -xf - && [[ -f $WORKDIR/space-captured/agent.db ]]; then
      captured=1
      # 队列库连同 WAL 一起拷出来恢复读，冻结时刻的待投递请求原样可读
      local cap_db
      cap_db=$(mktemp "$WORKDIR/space-captured-copy.XXXXXX.db")
      cp -- "$WORKDIR/space-captured/agent.db" "$cap_db"
      for side in -wal -shm; do
        if [[ -f $WORKDIR/space-captured/agent.db$side ]]; then
          cp -- "$WORKDIR/space-captured/agent.db$side" "$cap_db$side"
        fi
      done
      integrity=$(sqlite3 -batch "$cap_db" 'PRAGMA integrity_check;' 2>/dev/null || true)
      [[ $integrity == ok ]] || die "冻结转移出的队列不完整，停止恢复并保留现场: $cap_db"
      pass '冻结转移出的队列库完整性 ok'
      local rejected_body
      rejected_body=$(sqlite3 -batch "$cap_db" \
        "SELECT request_json FROM pending_deliveries WHERE request_type = 'report' LIMIT 1" 2>/dev/null || true)
      if [[ -n $rejected_body ]]; then
        local probe
        probe=$(curl -s -X POST \
          --header "Authorization: Bearer $(cat "$WORKDIR/auth/drill-c.token")" \
          --header 'X-LabBridge-Node-Code: drill-c' \
          --header 'Content-Type: application/json' \
          --data "$rejected_body" \
          "$(web_entry)/api/v1/task-runs/report")
        note "重放冻结时刻待投递报告的响应: $(printf '%s' "$probe" | head -c 160)"
        log_line "space frozen-report replay: $(printf '%s' "$probe" | head -c 160)"
      else
        note '冻结时刻队列里没有待投递的报告（作业停在其他阶段）'
      fi
    fi
  fi
  if ((captured)); then
    pass '限额存储现场完整转移（SIGSTOP + 容器内 tar 流）'
  else
    note '没能冻结转移（Agent 已瞬时退出、tmpfs 已随容器销毁）：恢复从空状态开始'
  fi

  step '扩容恢复：状态转移到普通磁盘后重新拉起（inbox 里失败的大文件还在）'
  docker rm -f labbridge-drill-agent-c >/dev/null 2>&1 || true
  # 转移出的状态里删掉的是我们自己的填充物，证据一个不动
  rm -f -- "$WORKDIR/space-captured/filler.bin"
  # WAL 可能还保存已提交作业，和主库一起恢复，不能手工删除。
  retain_drill_path "$WORKDIR/state/drill-c"
  cp -a -- "$WORKDIR/space-captured" "$WORKDIR/state/drill-c"
  docker run -d --name labbridge-drill-agent-c \
    --network "${PROJECT}_default" \
    --user "$(id -u):$(id -g)" \
    --volume "$WORKDIR/state/drill-c:/var/lib/labbridge" \
    --volume "$WORKDIR/etc/drill-c/agent.yaml:/etc/labbridge/agent.yaml:ro" \
    --volume "$WORKDIR/auth/drill-c.token:/etc/labbridge/auth/agent.token:ro" \
    --volume "$WORKDIR/inbox/drill-c:/drill/inbox:ro" \
    "$agent_image" >/dev/null
  local recovered=0 waited=0
  while ((waited < 240)); do
    if (( $(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-c' AND rf.original_name LIKE 'space-big-%'") >= big_before + 1 )); then
      recovered=1; break
    fi
    sleep 10; waited=$((waited + 10))
  done
  ((recovered)) \
    && pass '获得空间后失败的大文件补采成功（实际恢复状态核对通过）' \
    || fail '扩容后 240s 内没有等到补采成功'
  local first_now
  first_now=$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-c' AND rf.original_name LIKE 'space-first-%'")
  if ((captured)); then
    check_eq '队列完整转移，space-first 没有被重采' "$((first_before + 1))" "$first_now"
  else
    # 空状态恢复：指纹随丢失的队列一起没了，已处理文件会被重新入链一次，
    # 这是"存储内容全丢"下的合理代价；空间释放后新数据从此正常去重
    if ((first_now <= first_before + 2)); then
      pass "限额存储内容丢失后的空状态恢复：space-first 重新入链一次（共 $first_now）"
    else
      fail "space-first 重新入链超过一次（$first_now > $((first_before + 2))）"
    fi
  fi
  check_eq '本轮 space-big 恰好一份证据' "$((big_before + 1))" \
    "$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-c' AND rf.original_name LIKE 'space-big-%'")"
  verify_evidence

  step '收尾：停 drill-c 容器并停用它的任务'
  docker stop labbridge-drill-agent-c >/dev/null
  docker rm labbridge-drill-agent-c >/dev/null
  local task_id
  task_id=$(psql_q "SELECT t.id FROM tasks t JOIN nodes n ON n.id = t.node_id WHERE n.node_code = 'drill-c' LIMIT 1")
  if [[ -n $task_id ]]; then
    api PATCH "/api/v1/tasks/$task_id" '{"enabled":false}'
    note "drill-c 任务 #$task_id 已停用"
  fi
  log_line 'space scenario done'
  finish_scenario scenario-space
}

# ---------------------------------------------------------------------------
# 场景 6：正常停止与强杀
# ---------------------------------------------------------------------------

cmd_scenario_stops() {
  require_workdir
  step '场景 6：正常停止与强杀的恢复核对'

  step '正常停止：SIGTERM 优雅退出，队列干净落盘'
  local node=drill-a integrity
  deliver "$node" tests/fixtures/ops/observation-normal.csv --prefix stop-idle
  agent_stop "$node" 60 && pass "$node 60s 内优雅退出"
  if [[ ! -e $WORKDIR/state/$node/agent.db-wal ]]; then
    pass 'WAL 已合并（无残留 -wal 文件）'
  else
    note '存在 -wal 文件（可能为空壳），以 integrity_check 为准'
  fi
  integrity=$(sqlite3 -batch "file:$WORKDIR/state/$node/agent.db?mode=ro&immutable=1" 'PRAGMA integrity_check;' 2>/dev/null || true)
  check_eq '停止后队列库 integrity_check' ok "$integrity"
  agent_start "$node"
  wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'stop-idle-%'" 1 180 \
    || fail '正常停止/重启后 stop-idle 文件没有入链'

  step '强杀（处理中）：SIGKILL 后恢复，不许重复证据'
  local round waited=0 kill_files
  for round in 1 2 3; do
    make_observation_csv "$WORKDIR/staging/kill-$round.csv" 400 $((round * 999 + RANDOM))
    deliver "$node" "$WORKDIR/staging/kill-$round.csv" --prefix kill-$round
    # 交付后等几秒再杀，争取落在"运行已创建、作业未收尾"的窗口里
    sleep $((5 + RANDOM % 8))
    agent_kill "$node" || true
    agent_start "$node"
  done
  while ((waited < 240)); do
    kill_files=$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'kill-%'")
    if ((kill_files >= 3)); then break; fi
    sleep 10; waited=$((waited + 10))
  done
  kill_files=$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'kill-%'")
  check_eq '三次强杀后 3 个文件都各有一份证据' 3 "$kill_files"
  integrity=$(queue_q "$node" 'PRAGMA integrity_check;')
  check_eq '强杀恢复后队列库 integrity_check' ok "$integrity"
  verify_evidence

  step '强杀（积压中）：断网积压时被杀，作业不丢'
  compose stop server web >/dev/null
  deliver "$node" tests/fixtures/ops/observation-normal.csv --prefix kill-pending
  local waited2=0
  while ((waited2 < 90)); do
    if (( $(queue_pending "$node") >= 1 )); then break; fi
    sleep 5; waited2=$((waited2 + 5))
  done
  agent_kill "$node" || true
  compose up -d --wait >/dev/null
  agent_start "$node"
  wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'kill-pending-%'" 1 240 \
    && pass '积压中被杀的作业恢复后照常完成' || fail '积压作业丢了'
  wait_queue_empty "$node" 120 || fail '队列还有残留'

  note '掉电场景不在本机演练（需要隔离 Linux 环境按方案另行验证），归入未验证项'
  log_line 'stops scenario done'
  finish_scenario scenario-stops
}

# ---------------------------------------------------------------------------
# 场景 7：备份恢复（灾后安装 + 原键续传）
# ---------------------------------------------------------------------------

cmd_scenario_backup_restore() {
  require_workdir
  step '场景 7：带未完成作业的完整备份 → 灾后恢复 → 原键续传'

  step '制造断网积压（备份里要带一个未完成作业）'
  compose stop server web >/dev/null
  deliver drill-a tests/fixtures/ops/observation-normal.csv --prefix restore-pending
  local waited=0
  while ((waited < 90)); do
    if (( $(queue_pending drill-a) >= 1 )); then break; fi
    sleep 5; waited=$((waited + 5))
  done
  check_ge '断网下有未完成作业待备份' 1 "$(queue_pending drill-a)"
  local pending_key
  pending_key=$(queue_q drill-a 'SELECT execution_key FROM pending_jobs LIMIT 1')
  note "待恢复作业执行键: ${pending_key:0:16}…"

  step '完整备份（停 Agent → 停 server/web → 中心 + 两节点 + 收集核对）'
  local restore_ms=$(now_ms)
  do_full_backup restore
  note "备份阶段总耗时 $(fmt_ms $(( $(now_ms) - restore_ms )) )"

  step '备份后继续交付并处理一个文件（制造恢复点之后的数据）'
  # 恢复点基线以备份 manifest 为准：备份后阶段里完成的不算恢复点内的数据
  local baseline_raw
  baseline_raw=$(sed -n 's/^  raw_files=\([0-9]*\)$/\1/p' "$BACKUP_ROOT/center/$BACKUP_ID/manifest.txt")
  compose up -d --wait >/dev/null
  local node
  for node in "${NODES[@]}"; do agent_start "$node"; done
  # 前缀带随机数：场景重跑时别和上一轮留在库里的同名文件互相干扰
  local ab_prefix="after-backup-$RANDOM"
  deliver drill-a tests/fixtures/ops/observation-normal.csv --prefix "$ab_prefix"
  wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE '$ab_prefix-%'" 1 180 \
    || note '备份后文件没入链（不影响恢复语义演示）'

  step '灾后恢复：拆掉整栈（等于中心数据全丢），只用备份批次重建'
  for node in "${NODES[@]}"; do
    if agent_running "$node"; then agent_stop "$node"; fi
  done
  local restore_start=$(now_ms)
  restore_center "$BACKUP_ID" "$BACKUP_ROOT"
  restore_agents "$BACKUP_ID" "$BACKUP_ROOT"
  note "恢复（中心 + 两节点 + 校验）耗时 $(fmt_ms $(( $(now_ms) - restore_start )) )"

  step '恢复后核对：数据库回到恢复点，after-backup 的旧证据不在'
  check_eq '恢复后没有 after-backup 的旧证据（恢复点之后不合并）' 0 \
    "$(psql_q "SELECT count(*) FROM raw_files WHERE original_name LIKE '$ab_prefix-%'")"
  check_eq '恢复点内的原始文件原样回来（含此前各轮的历史证据）' "$baseline_raw" \
    "$(psql_q "SELECT count(*) FROM raw_files WHERE original_name NOT LIKE '$ab_prefix-%'")"

  step '启动并原键续传；inbox 里 after-backup 文件作为新输入重新入库'
  for node in "${NODES[@]}"; do agent_start "$node"; done
  local started=$(now_ms)
  if wait_sql "SELECT count(*) FROM task_runs tr JOIN nodes n ON n.id = tr.node_id WHERE n.node_code = 'drill-a' AND tr.execution_key = '$pending_key'" 1 240; then
    pass "未完成作业按原执行键续传（耗时 $(fmt_ms $(( $(now_ms) - started )) )）"
  else
    fail '240s 内没有看到原执行键的运行'
  fi
  wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'restore-pending-%'" 1 240 \
    && pass 'restore-pending 文件归档在案' || fail 'restore-pending 文件没有归档'
  wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE '$ab_prefix-%'" 1 240 \
    && pass '恢复点之后交付的文件经上游缓冲重新入库（新证据，非合并旧数据）' \
    || note 'after-backup 文件未被重新采集（inbox 里已没有它时属正常）'
  wait_queue_empty drill-a 120 || fail 'drill-a 队列有残留'
  verify_evidence

  step '恢复后跑一个新 CSV，确认业务可用（恢复完成判定）'
  deliver drill-a tests/fixtures/ops/observation-normal.csv --prefix restored-live
  wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'restored-live-%'" 1 180 \
    && pass '恢复后新文件完整闭环' || fail '恢复后新文件没有闭环'
  verify_evidence
  log_line "backup-restore done backup_id=$BACKUP_ID"
  finish_scenario scenario-backup-restore
}

# ---------------------------------------------------------------------------
# 场景 8：代表规模实测
# ---------------------------------------------------------------------------

cmd_scenario_scale() {
  require_workdir
  local files=100 rows=8
  while (($#)); do
    case $1 in
      --files) files=$2; shift 2 ;;
      --rows) rows=$2; shift 2 ;;
      *) die "scenario-scale 未知选项: $1" ;;
    esac
  done
  # 单轮报告体要控制在 server 的 client_max_body_size（1M）以内：
  # 实测 100 文件 × 30 行（约 3000 条记录 ≈ 1.15MB）会被 413 拒绝，
  # 作业进 requires_attention，后续槽位还会重采放大存储。默认 100×8 行
  # 约 300KB，留了三倍余量；调大批量前先按单条记录 payload 估算。
  step "场景 8：代表规模实测（模拟规模：$files 个文件 × $rows 行，明确标注非真实现场）"

  step '生成规模数据集并一次性交付'
  # 断言用本轮增量：库里可能还有上一轮演练留下的 scale 记录
  local scale_before
  scale_before=$(psql_q "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'scale-%'")
  local t0=$(now_ms)
  local i
  for ((i = 1; i <= files; i++)); do
    make_observation_csv "$WORKDIR/staging/scale-f$i.csv" "$rows" $((i * 61 + RANDOM))
    mv -- "$WORKDIR/staging/scale-f$i.csv" \
      "$WORKDIR/inbox/drill-a/scale-$(printf '%04d' "$i")-$(date +%s%N).csv"
  done
  note "交付 $files 个文件耗时 $(fmt_ms $(( $(now_ms) - t0 )) )"

  step '等全部入证据链'
  local started=$(now_ms)
  if wait_sql "SELECT count(*) FROM raw_files rf JOIN nodes n ON n.id = rf.node_id WHERE n.node_code = 'drill-a' AND rf.original_name LIKE 'scale-%'" $((scale_before + files)) 900; then
    pass "$files 个文件全部入证据链（耗时 $(fmt_ms $(( $(now_ms) - started )) )）"
  else
    fail "900s 内没有集齐 $files 个文件"
  fi
  # requires_attention 是等人工处置的驻留作业，不算交付积压
  local pending_active
  pending_active=$(queue_q drill-a "SELECT count(*) FROM pending_jobs WHERE stage != 'requires_attention'")
  check_eq '队列无未完成作业（requires_attention 除外）' 0 "$pending_active"
  note "解析记录 $(psql_q "SELECT count(*) FROM parsed_records p JOIN raw_files rf ON rf.id = p.raw_file_id WHERE rf.original_name LIKE 'scale-%'") 条"

  step '维护窗口：停止交付 → 停 Agent → 停 server/web（计时）'
  local stop_ms=$(now_ms) node
  for node in "${NODES[@]}"; do
    if agent_running "$node"; then agent_stop "$node"; fi
  done
  compose stop server web >/dev/null
  local stopped_ms=$(( $(now_ms) - stop_ms ))
  note "停机耗时 $(fmt_ms $stopped_ms)"

  step '完整备份（计时）'
  local backup_ms=$(now_ms)
  do_full_backup scale
  note "备份总耗时 $(fmt_ms $(( $(now_ms) - backup_ms )) )（含中心导出 + 两节点打包 + 哈希核对）"

  step '空间占用盘点'
  local db_size backup_size state_size
  db_size=$(compose exec -T postgres psql -U labbridge -d labbridge -tAc \
    "SELECT pg_size_pretty(pg_database_size('labbridge'))" | tr -d '[:space:]')
  backup_size=$(du -sh "$BACKUP_ROOT" | cut -f1)
  state_size=$(du -sh "$WORKDIR/state" 2>/dev/null | cut -f1)
  note "数据库 $db_size；Agent 状态目录 $state_size；备份批次 $backup_size"
  log_line "scale files=$files stop=$(fmt_ms $stopped_ms) backup=$(fmt_ms $(( $(now_ms) - backup_ms ))) db=$db_size backups=$backup_size state=$state_size"

  step '恢复演练（计时）+ 恢复后核对'
  local restore_ms=$(now_ms)
  restore_center "$BACKUP_ID" "$BACKUP_ROOT"
  restore_agents "$BACKUP_ID" "$BACKUP_ROOT"
  note "恢复总耗时 $(fmt_ms $(( $(now_ms) - restore_ms )) )（中心重建 + 两节点落位 + 行数/完整性核对）"
  for node in "${NODES[@]}"; do agent_start "$node"; done
  wait_sql "SELECT count(*) FROM raw_files WHERE original_name LIKE 'scale-%'" "$files" 60 \
    && pass '恢复后规模数据原样回来' || fail '恢复后规模数据不齐'
  verify_evidence
  log_line "scale-restore done $(fmt_ms $(( $(now_ms) - restore_ms )))"
  finish_scenario scenario-scale
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

main() {
  if (($# == 0)) || [[ $1 == -h || $1 == --help ]]; then
    usage
    exit 0
  fi
  local cmd=$1
  shift
  # --workdir 可以出现在任何位置，先摘出来
  local -a args=()
  while (($#)); do
    if [[ $1 == --workdir && -n ${2:-} ]]; then
      WORKDIR=$2; shift 2
    else
      args+=("$1"); shift
    fi
  done

  case $cmd in
    up) cmd_up "${args[@]}" ;;
    down) cmd_down ;;
    clean) cmd_clean "${args[@]}" ;;
    status) cmd_status ;;
    api) cmd_api "${args[@]}" ;;
    deliver) cmd_deliver "${args[@]}" ;;
    agent-start|agent-stop|agent-kill) cmd_agent "$cmd" "${args[@]}" ;;
    verify) cmd_verify ;;
    scenario-e2e) cmd_scenario_e2e ;;
    scenario-upgrade) cmd_scenario_upgrade "${args[@]}" ;;
    scenario-offline) cmd_scenario_offline ;;
    scenario-fingerprint) cmd_scenario_fingerprint ;;
    scenario-space) cmd_scenario_space ;;
    scenario-stops) cmd_scenario_stops ;;
    scenario-backup-restore) cmd_scenario_backup_restore ;;
    scenario-scale) cmd_scenario_scale "${args[@]}" ;;
    *) printf '未知命令: %s\n' "$cmd" >&2; usage >&2; exit 2 ;;
  esac
}
main "$@"
