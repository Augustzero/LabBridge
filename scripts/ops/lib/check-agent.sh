#!/usr/bin/env bash
# 本机 Agent 巡检；由 check.sh 加载。
run_agent() {
  local config=/etc/labbridge/agent.yaml
  local max_pending_age=0
  local backup_dir=''
  local max_backup_age_hours=0
  local min_free_mb=512
  local min_free_inodes=1000

  while (($#)); do
    (($# >= 2)) || { printf "选项缺少取值: %s\n" "$1" >&2; exit 2; }
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

  require_uint "$max_pending_age" --max-pending-age-seconds
  require_uint "$max_backup_age_hours" --max-backup-age-hours
  require_uint "$min_free_mb" --min-free-mb
  require_uint "$min_free_inodes" --min-free-inodes

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
  local key value config_values
  config_values=$(python3 "$script_dir/lib/agent-config.py" "$config")
  while IFS=$'\t' read -r key value; do
    case $key in
      agent.node_code) node_code=$value ;;
      agent.server_url) server_url=$value ;;
      storage.queue_db) queue_db=$value ;;
      storage.work_dir) work_dir=$value ;;
      storage.max_pending_jobs) max_pending_jobs=$value ;;
      tasks.allowed_local_roots) allowed_roots+=("$value") ;;
    esac
  done <<<"$config_values"
  ok "节点编号 $node_code，中心入口 ${server_url:-<未配置>}"

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
  else
    fail "队列库无法只读打开: $queue_db（文件权限或库损坏）"
  fi

  if [[ -n $queue_uri ]]; then
    local query_queue=(sqlite3 -batch -noheader "$queue_uri")
    local meta_node
    meta_node=$("${query_queue[@]}" 'SELECT node_code FROM queue_metadata WHERE singleton_id = 1')
    if [[ $meta_node == "$node_code" ]]; then
      ok "队列身份一致: $meta_node"
    else
      fail "队列身份($meta_node)与配置节点编号($node_code)不一致，Agent 无法启动"
    fi

    local pending delivery_count stage_line
    pending=$("${query_queue[@]}" 'SELECT count(*) FROM pending_jobs')
    delivery_count=$("${query_queue[@]}" 'SELECT count(*) FROM pending_deliveries')
    info "待处理作业 $pending 个（待投递请求 $delivery_count 个）"
    if ((pending >= max_pending_jobs)); then
      fail "待处理作业已达上限 max_pending_jobs=$max_pending_jobs，新任务会被拒绝"
    fi
    while IFS='|' read -r stage_line; do
      [[ -n $stage_line ]] && info "  ${stage_line%%|*}: ${stage_line##*|}"
    done < <("${query_queue[@]}" "SELECT stage || '|' || count(*) FROM pending_jobs GROUP BY stage ORDER BY stage")

    local oldest oldest_age
    oldest=$("${query_queue[@]}" 'SELECT min(created_at) FROM pending_jobs')
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
    ra_count=$("${query_queue[@]}" "SELECT count(*) FROM pending_jobs WHERE stage = 'requires_attention'")
    if ((ra_count == 0)); then
      ok '没有 requires_attention 作业'
    else
      fail "有 $ra_count 个作业处于 requires_attention，需要人工处理（最多列 5 条）:"
      while IFS='|' read -r ra_line; do
        info "  ${ra_line%%|*}: ${ra_line##*|}"
      done < <("${query_queue[@]}" "SELECT execution_key || '|' || COALESCE(last_error, '') \
        FROM pending_jobs WHERE stage = 'requires_attention' \
        ORDER BY updated_at DESC LIMIT 5")
    fi

    local fail_count fail_line
    fail_count=$("${query_queue[@]}" "SELECT count(*) FROM (SELECT 1 FROM delivery_attempts \
      WHERE outcome != 'success' ORDER BY id DESC LIMIT 5)")
    if ((fail_count == 0)); then
      ok '最近的投递尝试没有失败记录'
    else
      warn "最近有失败的投递尝试（最多列 5 条）:"
      while IFS='|' read -r fail_line; do
        IFS='|' read -r ts rtype outcome kind status msg <<<"$fail_line"
        info "  $ts $rtype $outcome http=$status $kind: ${msg:0:100}"
      done < <("${query_queue[@]}" "SELECT attempted_at || '|' || request_type || '|' || outcome \
        || '|' || COALESCE(error_kind, '-') || '|' || COALESCE(http_status, 0) \
        || '|' || COALESCE(message, '') \
        FROM delivery_attempts WHERE outcome != 'success' ORDER BY id DESC LIMIT 5")
    fi
  fi

  header '磁盘与输入目录'
  if [[ -n $queue_db ]]; then
    check_disk "$queue_db" '队列库所在文件系统' "$min_free_mb" "$min_free_inodes"
  fi
  if [[ -n $work_dir ]]; then
    check_disk "$work_dir" '工作/归档目录' "$min_free_mb" "$min_free_inodes"
  fi
  local root oldest_entry oldest_ts file_count
  for root in "${allowed_roots[@]}"; do
    check_disk "$root" "输入目录" "$min_free_mb" "$min_free_inodes"
    if [[ -d $root ]]; then
      # 采集只看目录顶层文件；数量和最老文件是“交付停止/积压”的直接线索
      oldest_entry=$(find "$root" -maxdepth 1 -type f -printf '%T@ %f\n' 2>/dev/null | sort -n | sed -n '1p')
      file_count=$(find "$root" -maxdepth 1 -type f 2>/dev/null | wc -l)
      if [[ -n $oldest_entry ]]; then
        oldest_ts=${oldest_entry%% *}
        info "输入目录 $root 顶层文件 ${file_count} 个，最老 $(fmt_age $((now_epoch - ${oldest_ts%.*})))（$(date -d "@${oldest_ts%.*}" '+%F %T')）"
      else
        info "输入目录 $root 顶层没有待处理文件"
      fi
    fi
  done
  if [[ -n $backup_dir ]]; then check_disk "$backup_dir" '备份目录' "$min_free_mb" "$min_free_inodes"; fi
  report_backup '本节点' "$backup_dir" "$max_backup_age_hours"
}
