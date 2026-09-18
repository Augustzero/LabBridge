#!/usr/bin/env bash
#
# LabBridge 只读巡检脚本，两种模式：
#   center —— 在中心机检查 Compose 服务状态、认证、节点心跳、运行推进、
#             数据库磁盘与最近备份；
#   agent  —— 在节点机检查 systemd 服务、SQLite 队列积压与失败原因、
#             输入/队列/归档目录磁盘与最近备份。
# 只做读取和查询，不改任何业务状态；发现问题返回非零，并指出节点、路径或作业。
# 面向 Ubuntu 24.04（依赖 GNU date/df/find/stat）；center 模式还需要
# docker、python3，agent 模式还需要 sqlite3 命令。
#
# 用法:
#   bash scripts/ops/check.sh center [--env-file F] [--compose P] [选项]
#   bash scripts/ops/check.sh agent  [--config /etc/labbridge/agent.yaml] [选项]
#
# 通用选项:
#   --min-free-mb N          磁盘可用空间阈值，默认 512（MiB）
#   --min-free-inodes N      磁盘可用 inode 阈值，默认 1000
#   --backup-dir PATH        备份目录；不指定则跳过最近备份检查
#   --max-backup-age-hours N 最近备份超龄判定；默认 0 只报告不判定
# center 选项:
#   --env-file F             环境文件，默认 /etc/labbridge/production.env
#   --compose P              Compose 文件，默认 deploy/production/compose.yaml
#   --max-heartbeat-age-seconds N   心跳超时阈值，默认 120（与 server 配置对齐）
#   --max-run-age-seconds N  最近成功运行超龄判定；默认 0 只报告不判定
# agent 选项:
#   --config P               Agent 配置文件，默认 /etc/labbridge/agent.yaml
#   --max-pending-age-seconds N     队列最老作业超龄判定；默认 0 只报告不判定

set -Eeuo pipefail

usage() { awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }

failures=0
warns=0

ok()   { printf '  [OK]   %s\n' "$*"; }
warn() { printf '  [WARN] %s\n' "$*"; warns=$((warns + 1)); }
fail() { printf '  [FAIL] %s\n' "$*"; failures=$((failures + 1)); }
info() { printf '        %s\n' "$*"; }
header() { printf -- '--- %s ---\n' "$*"; }

now_epoch=$(date +%s)

# ISO 时间串 -> 距今秒数；解析失败输出空串，调用方自行降级为警告
age_of() {
  local epoch
  epoch=$(date -u -d "$1" +%s 2>/dev/null) || return 0
  printf '%s' $((now_epoch - epoch))
}

fmt_age() {
  local s=$1 d h m
  d=$((s / 86400)); s=$((s % 86400))
  h=$((s / 3600)); s=$((s % 3600))
  m=$((s / 60)); s=$((s % 60))
  if ((d > 0)); then printf '%dd%dh%dm' "$d" "$h" "$m"
  elif ((h > 0)); then printf '%dh%dm' "$h" "$m"
  elif ((m > 0)); then printf '%dm%ds' "$m" "$s"
  else printf '%ds' "$s"; fi
}

# 磁盘可用空间与 inode；路径不存在按 FAIL 处理，其余只报数并按阈值判定
check_disk() {
  local path=$1 label=$2
  if [[ ! -e $path ]]; then
    fail "$label 路径不存在: $path"
    return
  fi
  local avail_mb avail_inodes used_pct
  if ! avail_mb=$(df -B1M --output=avail "$path" 2>/dev/null | tail -n 1 | tr -d ' ') \
    || ! avail_inodes=$(df --output=iavail "$path" 2>/dev/null | tail -n 1 | tr -d ' ') \
    || ! used_pct=$(df --output=pcent "$path" 2>/dev/null | tail -n 1 | tr -d ' '); then
    fail "$label 磁盘信息查询失败: $path"
    return
  fi
  info "$label（$path）: 已用 ${used_pct}，可用 ${avail_mb}MiB、inode ${avail_inodes}"
  if ((avail_mb < min_free_mb)); then
    fail "$label 可用空间不足: ${avail_mb}MiB < ${min_free_mb}MiB（$path）；先暂停文件交付并扩容，不自动删数据"
  fi
  if ((avail_inodes < min_free_inodes)); then
    fail "$label 可用 inode 不足: ${avail_inodes} < ${min_free_inodes}（$path）"
  fi
}

# 每批备份是备份目录下的一个子目录、内含 manifest.txt（028-04 备份工具的约定）。
# 这里找最新的那份报时间；给了 --max-backup-age-hours 才做超龄判定。
report_backup() {
  local label=$1
  if [[ -z $backup_dir ]]; then
    info '未指定 --backup-dir，跳过最近备份检查'
    return
  fi
  if [[ ! -d $backup_dir ]]; then
    fail "$label 备份目录不存在: $backup_dir"
    return
  fi
  local newest mtime age_h
  newest=$(find "$backup_dir" -mindepth 2 -maxdepth 2 -name manifest.txt \
    -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -n 1 | cut -d' ' -f2-)
  if [[ -z $newest ]]; then
    if ((max_backup_age_hours > 0)); then
      fail "$label 在 $backup_dir 下没有找到带 manifest.txt 的备份批次"
    else
      warn "$label 在 $backup_dir 下没有找到带 manifest.txt 的备份批次"
    fi
    return
  fi
  mtime=$(stat -c %Y "$newest")
  age_h=$(((now_epoch - mtime) / 3600))
  info "$label 最近完整备份批次: $newest（${age_h} 小时前）"
  if ((max_backup_age_hours > 0 && age_h > max_backup_age_hours)); then
    fail "$label 最近备份已 ${age_h} 小时（超过 ${max_backup_age_hours} 小时，$newest），按约定交负责人处理"
  fi
}

# ---------------------------------------------------------------------------
# center 模式
# ---------------------------------------------------------------------------

# 管理查询：经 server 容器内回环发起，不依赖 Web 对外绑定地址。
# 成功时把响应体和 HTTP 状态码放进 api_body / api_code；失败时错误文本放
# api_err 并返回非零。
center_api() {
  local path=$1 out code
  if ! out=$("${compose_cmd[@]}" exec -T server curl -sS --max-time 10 \
      -w '\n%{http_code}' \
      --header "Authorization: Bearer $management_token" \
      "http://127.0.0.1:18080$path" 2>&1); then
    api_err=${out%%$'\n'*}
    return 1
  fi
  code=${out##*$'\n'}
  api_body=${out%$'\n'*}
  api_code=$code
}

run_center() {
  local env_file=/etc/labbridge/production.env
  local compose_file=deploy/production/compose.yaml
  local max_heartbeat_age=120
  local max_run_age=0
  local backup_dir=''
  local max_backup_age_hours=0
  local min_free_mb=512
  local min_free_inodes=1000

  while (($#)); do
    case $1 in
      --env-file) env_file=$2; shift 2 ;;
      --compose) compose_file=$2; shift 2 ;;
      --max-heartbeat-age-seconds) max_heartbeat_age=$2; shift 2 ;;
      --max-run-age-seconds) max_run_age=$2; shift 2 ;;
      --backup-dir) backup_dir=$2; shift 2 ;;
      --max-backup-age-hours) max_backup_age_hours=$2; shift 2 ;;
      --min-free-mb) min_free_mb=$2; shift 2 ;;
      --min-free-inodes) min_free_inodes=$2; shift 2 ;;
      *) printf '未知选项: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
  done

  for tool in docker python3; do
    command -v "$tool" >/dev/null 2>&1 || { printf '缺少 %s，无法巡检中心\n' "$tool" >&2; exit 2; }
  done
  if [[ ! -r $env_file ]]; then
    printf '环境文件不可读: %s（用 --env-file 指定）\n' "$env_file" >&2
    exit 2
  fi
  if [[ ! -r $compose_file ]]; then
    printf 'Compose 文件不可读: %s（用 --compose 指定）\n' "$compose_file" >&2
    exit 2
  fi

  printf '=== LabBridge 中心巡检（%s） ===\n' "$(date '+%F %T')"
  local compose_cmd=(docker compose --env-file "$env_file" -f "$compose_file")

  header 'Compose 服务状态'
  local services_ok=1 ps_json
  if ! ps_json=$("${compose_cmd[@]}" ps --all --format json 2>&1); then
    fail "docker compose ps 执行失败: ${ps_json%%$'\n'*}"
    services_ok=0
  else
    # compose 新版输出 JSON 数组，老版逐行对象，两种都认
    while IFS=$'\t' read -r service state health; do
      [[ -n $service ]] || continue
      if [[ $state != running ]]; then
        fail "服务 $service 状态为 $state（应为 running）"
        services_ok=0
      elif [[ $health != healthy ]]; then
        fail "服务 $service 健康状态为 ${health:-无}（应为 healthy）"
        services_ok=0
      else
        ok "服务 $service：running / healthy"
      fi
    done < <(printf '%s' "$ps_json" | python3 -c '
import json, sys
raw = sys.stdin.read().strip()
if not raw:
    raise SystemExit
try:
    data = json.loads(raw)
except ValueError:
    data = [json.loads(line) for line in raw.splitlines() if line.strip()]
if isinstance(data, dict):
    data = [data]
for item in data:
    print("\t".join((item.get("Service") or "", item.get("State") or "",
                     item.get("Health") or "")))
')
    if ((services_ok)); then :; else
      warn '有服务不在运行，后面的认证与查询检查可能一并失败'
    fi
  fi

  header '认证与管理查询'
  local auth_dir management_token token_file
  auth_dir=$(sed -n 's/^LABBRIDGE_AUTH_DIR=//p' "$env_file" | tail -n 1)
  auth_dir=${auth_dir%\"}; auth_dir=${auth_dir#\"}
  token_file=$auth_dir/management.token
  if [[ -z $auth_dir || ! -r $token_file ]]; then
    fail "管理 token 不可读: $token_file（检查环境文件 LABBRIDGE_AUTH_DIR）"
    services_ok=0
  else
    management_token=$(cat "$token_file")
    if ! center_api '/api/v1/nodes?limit=1'; then
      fail "认证请求无法送达 server: ${api_err:-容器未运行或网络异常}"
      services_ok=0
    elif [[ $api_code != 200 ]]; then
      fail "带管理 token 的查询返回 HTTP $api_code（应为 200），认证或服务异常"
      services_ok=0
    else
      ok '带管理 token 的认证查询通过'
    fi
  fi

  header '节点心跳与运行推进'
  if ((services_ok)); then
    local -a node_codes=()
    local cursor=''
    while :; do
      local path='/api/v1/nodes?limit=100'
      [[ -n $cursor ]] && path="${path}&cursor=${cursor}"
      center_api "$path" || { fail "节点列表查询失败: ${api_err:-$path}"; break; }
      [[ $api_code == 200 ]] || { fail "节点列表查询返回 HTTP $api_code"; break; }
      local nc status hb
      while IFS=$'\t' read -r nc status hb; do
        [[ $nc == '@cursor' ]] && { cursor=$status; continue; }
        [[ -n $nc ]] || continue
        node_codes+=("$nc")
        if [[ $status == offline ]]; then
          fail "节点 $nc 中心判定 offline（最后心跳 ${hb:-从未上报}）"
        else
          ok "节点 $nc：$status，最后心跳 ${hb:-无记录}"
        fi
        # 心跳超时独立再判一次：中心 offline 阈值与这里阈值不一致时也能暴露
        local hb_age
        hb_age=$(age_of "${hb:-}")
        if [[ -z ${hb:-} ]]; then
          fail "节点 $nc 从未上报心跳"
        elif [[ -z $hb_age ]]; then
          warn "节点 $nc 心跳时间无法解析: $hb"
        elif ((hb_age > max_heartbeat_age)); then
          fail "节点 $nc 心跳已是 $(fmt_age "$hb_age") 前（阈值 ${max_heartbeat_age}s）"
        fi
      done < <(printf '%s' "$api_body" | python3 -c '
import json, sys
data = json.loads(sys.stdin.read())
payload = data.get("data") or {}
for item in payload.get("items") or []:
    print("\t".join((item.get("node_code") or "",
                     item.get("effective_status") or "",
                     item.get("last_heartbeat_at") or "")))
print("\t".join(("@cursor", payload.get("next_cursor") or "")))
')
      [[ -n $cursor ]] || break
    done

    if ((${#node_codes[@]} == 0)); then
      warn '中心还没有任何注册节点'
    fi

    for nc in "${node_codes[@]}"; do
      center_api "/api/v1/nodes/$nc" || { fail "节点 $nc 概要查询失败"; continue; }
      [[ $api_code == 200 ]] || { fail "节点 $nc 概要查询返回 HTTP $api_code"; continue; }
      local enabled_tasks open_alerts latest_started latest_status
      read -r enabled_tasks open_alerts latest_started latest_status < <(
        printf '%s' "$api_body" | python3 -c '
import json, sys
n = (json.loads(sys.stdin.read()).get("data")) or {}
lr = n.get("latest_task_run") or {}
print(" ".join((str(n.get("enabled_task_count") or 0),
                str(n.get("open_alert_count") or 0),
                lr.get("started_at") or "-",
                lr.get("status") or "-")))
')
      if ((open_alerts > 0)); then
        warn "节点 $nc 有 $open_alerts 条未处置告警（管理台确认处置）"
      fi
      if ((enabled_tasks == 0)); then
        info "节点 $nc 没有启用任务，跳过运行推进检查"
        continue
      fi
      if [[ $latest_started == '-' ]]; then
        info "节点 $nc 有启用任务但还没有任何运行记录（可能是刚接入）"
      else
        local latest_age
        latest_age=$(age_of "$latest_started")
        info "节点 $nc 最近一次运行: $latest_status，开始于 ${latest_started}$([[ -n $latest_age ]] && printf '（%s前）' "$(fmt_age "$latest_age")")"
      fi
      center_api "/api/v1/task-runs?node_code=$nc&status=succeeded&limit=1" \
        || { warn "节点 $nc 成功运行查询失败"; continue; }
      [[ $api_code == 200 ]] || { warn "节点 $nc 成功运行查询返回 HTTP $api_code"; continue; }
      local succeeded_at
      succeeded_at=$(printf '%s' "$api_body" | python3 -c '
import json, sys
items = ((json.loads(sys.stdin.read()).get("data")) or {}).get("items") or []
if not items:
    print("")
else:
    print(items[0].get("finished_at") or items[0].get("started_at") or "")
')
      if [[ -z $succeeded_at ]]; then
        warn "节点 $nc 还没有成功完成的运行"
        continue
      fi
      local succ_age
      succ_age=$(age_of "$succeeded_at")
      if [[ -z $succ_age ]]; then
        warn "节点 $nc 最近成功运行时间无法解析: $succeeded_at"
      elif ((max_run_age > 0 && succ_age > max_run_age)); then
        fail "节点 $nc 最近成功运行已是 $(fmt_age "$succ_age") 前（阈值 $(fmt_age "$max_run_age")），持续输入场景下数据可能停止推进"
      else
        ok "节点 $nc 最近成功运行: ${succeeded_at}$([[ -n $succ_age ]] && printf '（%s前）' "$(fmt_age "$succ_age")")"
      fi
    done
  else
    info '服务不可用，跳过节点与运行检查'
  fi

  header '数据库磁盘与备份'
  if ((services_ok)); then
    local db_avail_mb db_avail_inodes
    if ! db_avail_mb=$("${compose_cmd[@]}" exec -T postgres \
        df -B1M --output=avail /var/lib/postgresql/data | tail -n 1 | tr -d ' ') \
      || ! db_avail_inodes=$("${compose_cmd[@]}" exec -T postgres \
        df --output=iavail /var/lib/postgresql/data | tail -n 1 | tr -d ' '); then
      fail '数据库磁盘信息查询失败（postgres 容器内 df）'
    else
      info "数据库（postgres 容器内 /var/lib/postgresql/data）: 可用 ${db_avail_mb}MiB、inode ${db_avail_inodes}"
      if ((db_avail_mb < min_free_mb)); then
        fail "数据库可用空间不足: ${db_avail_mb}MiB < ${min_free_mb}MiB；先暂停文件交付再扩容"
      fi
      if ((db_avail_inodes < min_free_inodes)); then
        fail "数据库可用 inode 不足: ${db_avail_inodes} < ${min_free_inodes}"
      fi
    fi
  else
    info '服务不可用，跳过数据库磁盘检查'
  fi
  report_backup '中心'
}

# ---------------------------------------------------------------------------
# agent 模式
# ---------------------------------------------------------------------------

# 只认仓库 agent 配置模板的扁平两级结构：顶层无缩进是 section，缩进的是
# 键值；缩进的空值键（allowed_local_roots:）是列表头，其后 “- 值” 是列表项。
# 靠缩进区分层级，行内注释仅按“空格 + #”剥离，避免误伤 URL 一类取值。
parse_agent_config() {
  awk '
    {
      line = $0
      sub(/\r$/, "", line)
      sub(/[ \t]+#.*/, "", line)
      if (line ~ /^[ \t]*$/) next
      indent = match(line, /[^ \t]/) - 1
      content = substr(line, indent + 1)
      if (content ~ /^#/) next
      if (indent == 0 && content ~ /^[^:]*:[ \t]*$/) {
        section = content; sub(/:.*$/, "", section)
        rootkey = ""
        next
      }
      if (content ~ /^- /) {
        value = content; sub(/^- /, "", value); gsub(/^[ \t]+|[ \t]+$/, "", value)
        if (section != "" && rootkey != "")
          print section "." rootkey "\t" value
        next
      }
      split(content, kv, ":")
      key = kv[1]; gsub(/^[ \t]+|[ \t]+$/, "", key)
      value = content; sub(/^[^:]*:[ \t]*/, "", value)
      gsub(/^[ \t]+|[ \t]+$/, "", value)
      if (value == "") { rootkey = key; next }
      if (section != "" && key != "")
        print section "." key "\t" value
      rootkey = key
    }' "$1"
}

run_agent() {
  local config=/etc/labbridge/agent.yaml
  local max_pending_age=0
  local backup_dir=''
  local max_backup_age_hours=0
  local min_free_mb=512
  local min_free_inodes=1000

  while (($#)); do
    case $1 in
      --config) config=$2; shift 2 ;;
      --max-pending-age-seconds) max_pending_age=$2; shift 2 ;;
      --backup-dir) backup_dir=$2; shift 2 ;;
      --max-backup-age-hours) max_backup_age_hours=$2; shift 2 ;;
      --min-free-mb) min_free_mb=$2; shift 2 ;;
      --min-free-inodes) min_free_inodes=$2; shift 2 ;;
      *) printf '未知选项: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
  done

  command -v sqlite3 >/dev/null 2>&1 \
    || { printf '缺少 sqlite3 命令，无法巡检队列（先安装: apt install sqlite3）\n' >&2; exit 2; }
  if [[ ! -r $config ]]; then
    printf 'Agent 配置不可读: %s（用 --config 指定）\n' "$config" >&2
    exit 2
  fi

  printf '=== LabBridge Agent 巡检（%s） ===\n' "$(date '+%F %T')"

  header 'Agent 配置'
  local node_code='' server_url='' queue_db='' work_dir='' max_pending_jobs=1000
  local -a allowed_roots=()
  local key value
  while IFS=$'\t' read -r key value; do
    case $key in
      agent.node_code) node_code=$value ;;
      agent.server_url) server_url=$value ;;
      storage.queue_db) queue_db=$value ;;
      storage.work_dir) work_dir=$value ;;
      storage.max_pending_jobs) max_pending_jobs=$value ;;
      tasks.allowed_local_roots) allowed_roots+=("$value") ;;
    esac
  done < <(parse_agent_config "$config")
  if [[ -z $node_code || -z $queue_db || -z $work_dir ]]; then
    fail "配置缺少必填项: node_code/queue_db/work_dir（$config）"
    info "已解析: node_code=${node_code:-<空>} queue_db=${queue_db:-<空>} work_dir=${work_dir:-<空>}"
  else
    ok "节点编号 $node_code，中心入口 ${server_url:-<未配置>}"
  fi

  header 'systemd 服务'
  local service_state=''
  if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
    service_state=$(systemctl is-active labbridge-agent 2>/dev/null || true)
    case $service_state in
      active)
        ok "labbridge-agent 运行中（启动于 $(systemctl show -p ActiveEnterTimestamp --value labbridge-agent)）"
        ;;
      activating)
        warn "labbridge-agent 正在启动: $service_state"
        ;;
      *)
        fail "labbridge-agent 状态为 ${service_state:-unknown}（journalctl -u labbridge-agent 查原因）"
        ;;
    esac
    local enabled
    enabled=$(systemctl is-enabled labbridge-agent 2>/dev/null || true)
    if [[ $enabled == enabled ]]; then
      ok '开机自启已启用'
    else
      warn "开机自启未启用（systemctl is-enabled 报 ${enabled:-unknown}），现场节点应 enable"
    fi
  else
    warn '本机没有 systemd（开发环境常见），跳过服务状态检查'
  fi

  header '队列状态（SQLite）'
  local queue_uri=''
  if [[ ! -f $queue_db ]]; then
    fail "队列库不存在: $queue_db"
  elif sqlite3 -batch "file:$queue_db?mode=ro" 'SELECT 1 FROM queue_metadata' >/dev/null 2>&1; then
    queue_uri="file:$queue_db?mode=ro"
  elif [[ $service_state != active ]] \
    && sqlite3 -batch "file:$queue_db?mode=ro&immutable=1" 'SELECT 1 FROM queue_metadata' >/dev/null 2>&1; then
    # Agent 停止时 WAL 的 -shm 不在了，普通只读打不开；immutable 只读打开
    # 读到的是已落盘内容，对巡检够用，但要说明这个前提
    queue_uri="file:$queue_db?mode=ro&immutable=1"
    warn '队列库以 immutable 只读方式打开（Agent 未运行，读数不含未落盘 WAL）'
  else
    fail "队列库无法只读打开: $queue_db（文件权限或库损坏）"
  fi

  if [[ -n $queue_uri ]]; then
    q() { sqlite3 -batch -noheader "$queue_uri" "$1"; }

    local meta_node
    meta_node=$(q 'SELECT node_code FROM queue_metadata WHERE singleton_id = 1')
    if [[ $meta_node == "$node_code" ]]; then
      ok "队列身份一致: $meta_node"
    else
      fail "队列身份($meta_node)与配置节点编号($node_code)不一致，Agent 无法启动"
    fi

    local pending delivery_count stage_line
    pending=$(q 'SELECT count(*) FROM pending_jobs')
    delivery_count=$(q 'SELECT count(*) FROM pending_deliveries')
    info "待处理作业 $pending 个（待投递请求 $delivery_count 个）"
    if ((pending >= max_pending_jobs)); then
      fail "待处理作业已达上限 max_pending_jobs=$max_pending_jobs，新任务会被拒绝"
    fi
    while IFS='|' read -r stage_line; do
      [[ -n $stage_line ]] && info "  ${stage_line%%|*}: ${stage_line##*|}"
    done < <(q "SELECT stage || '|' || count(*) FROM pending_jobs GROUP BY stage ORDER BY stage")

    local oldest oldest_age
    oldest=$(q 'SELECT min(created_at) FROM pending_jobs')
    if [[ -z $oldest ]]; then
      ok '队列为空，没有积压'
    else
      oldest_age=$(age_of "$oldest")
      if [[ -z $oldest_age ]]; then
        warn "最老作业时间无法解析: $oldest（本机时钟与库内时间可能不一致）"
      else
        info "最老作业等待 $(fmt_age "$oldest_age")（创建于 $oldest）"
        if ((max_pending_age > 0 && oldest_age > max_pending_age)); then
          fail "最老作业已等待 $(fmt_age "$oldest_age")，超过阈值 $(fmt_age "$max_pending_age")（中心不可达或投递受阻）"
        fi
      fi
    fi

    local ra_count ra_line
    ra_count=$(q "SELECT count(*) FROM pending_jobs WHERE stage = 'requires_attention'")
    if ((ra_count == 0)); then
      ok '没有 requires_attention 作业'
    else
      fail "有 $ra_count 个作业处于 requires_attention，需要人工处理（最多列 5 条）:"
      while IFS='|' read -r ra_line; do
        info "  ${ra_line%%|*}: ${ra_line##*|}"
      done < <(q "SELECT execution_key || '|' || COALESCE(last_error, '') \
        FROM pending_jobs WHERE stage = 'requires_attention' \
        ORDER BY updated_at DESC LIMIT 5")
    fi

    local fail_count fail_line
    fail_count=$(q "SELECT count(*) FROM (SELECT 1 FROM delivery_attempts \
      WHERE outcome != 'success' ORDER BY id DESC LIMIT 5)")
    if ((fail_count == 0)); then
      ok '最近的投递尝试没有失败记录'
    else
      warn "最近有失败的投递尝试（最多列 5 条）:"
      while IFS='|' read -r fail_line; do
        IFS='|' read -r ts rtype outcome kind status msg <<<"$fail_line"
        info "  $ts $rtype $outcome http=$status $kind: ${msg:0:100}"
      done < <(q "SELECT attempted_at || '|' || request_type || '|' || outcome \
        || '|' || COALESCE(error_kind, '-') || '|' || COALESCE(http_status, 0) \
        || '|' || COALESCE(message, '') \
        FROM delivery_attempts WHERE outcome != 'success' ORDER BY id DESC LIMIT 5")
    fi
  fi

  header '磁盘与输入目录'
  if [[ -n $queue_db ]]; then
    check_disk "$queue_db" '队列库所在文件系统'
  fi
  if [[ -n $work_dir ]]; then
    check_disk "$work_dir" '工作/归档目录'
  fi
  local root oldest_entry oldest_ts file_count
  for root in "${allowed_roots[@]}"; do
    check_disk "$root" "输入目录"
    if [[ -d $root ]]; then
      # 采集只看目录顶层文件；数量和最老文件是“交付停止/积压”的直接线索
      oldest_entry=$(find "$root" -maxdepth 1 -type f -printf '%T@ %f\n' 2>/dev/null | sort -n | head -n 1)
      file_count=$(find "$root" -maxdepth 1 -type f 2>/dev/null | wc -l)
      if [[ -n $oldest_entry ]]; then
        oldest_ts=${oldest_entry%% *}
        info "输入目录 $root 顶层文件 ${file_count} 个，最老 $(fmt_age $((now_epoch - ${oldest_ts%.*})))（$(date -d "@${oldest_ts%.*}" '+%F %T')）"
      else
        info "输入目录 $root 顶层没有待处理文件"
      fi
    fi
  done
  report_backup '本节点'
}

# ---------------------------------------------------------------------------
main() {
  if (($# == 0)) || [[ $1 == -h || $1 == --help ]]; then
    usage
    exit 0
  fi
  local mode=$1
  shift
  case $mode in
    center) run_center "$@" ;;
    agent) run_agent "$@" ;;
    *) printf '未知模式: %s（只支持 center / agent）\n' "$mode" >&2; exit 2 ;;
  esac

  printf '\n=== 巡检结果: '
  if ((failures == 0)); then
    printf '通过（警告 %d 项） ===\n' "$warns"
    exit 0
  fi
  printf '发现 %d 个问题（另有警告 %d 项） ===\n' "$failures" "$warns"
  exit 1
}
main "$@"
