#!/usr/bin/env bash
# 中心巡检；请求经 Web 反代，调用所需的 Compose 命令显式传入。
center_api() {
  local path=$1
  shift
  "$@" exec -T web sh -c '
    wget -q -T 10 -O - \
      --header "Authorization: Bearer $(cat /etc/labbridge/auth/management.token)" \
      "http://127.0.0.1:8080$1"
  ' sh "$path"
}

# 各类响应只在这里解码；解析失败由调用方记录 FAIL，不把错误当空列表。
center_fields() {
  python3 -c '
import json, sys
from urllib.parse import quote

def row(*values):
    values = [str(v) if v is not None and v != "" else "-" for v in values]
    if any(any(c in v for c in "\t\r\n") for v in values):
        raise ValueError("response contains invalid control characters")
    print("\t".join(values))

try:
    mode = sys.argv[1]
    raw = sys.stdin.read()
    if mode == "services":
        try:
            items = json.loads(raw or "[]")
        except ValueError:
            items = [json.loads(line) for line in raw.splitlines() if line.strip()]
        if isinstance(items, dict):
            items = [items]
        for name in ("postgres", "server", "web"):
            matches = [x for x in items if x["Service"] == name]
            if not matches:
                row(name, "missing", "missing")
            for item in matches:
                row(name, item.get("State"), item.get("Health"))
    else:
        data = json.loads(raw)["data"]
        if not isinstance(data, dict):
            raise ValueError("data is not an object")
        if mode == "nodes":
            for item in data["items"]:
                row(item["node_code"], item["effective_status"], item.get("last_heartbeat_at"))
            row("@cursor", quote(data.get("next_cursor") or "", safe=""))
        elif mode == "summary":
            latest = data.get("latest_task_run") or {}
            row(data["enabled_task_count"], data["open_alert_count"],
                latest.get("started_at"), latest.get("status"), data.get("created_at"))
        elif mode == "succeeded":
            items = data["items"]
            row((items[0].get("finished_at") or items[0]["started_at"]) if items else None)
except (ValueError, KeyError, TypeError, AttributeError) as error:
    sys.exit(f"中心响应解析失败: {error}")
' "$1"
}

check_node_progress() {
  local nc=$1 max_run_age=$2
  shift 2
  local body summary enabled_tasks open_alerts latest_started latest_status created_at
  local succeeded_at age reference
  local encoded_nc
  encoded_nc=$(python3 -c 'import sys; from urllib.parse import quote; print(quote(sys.argv[1], safe=""))' "$nc")
  if ! body=$(center_api "/api/v1/nodes/$encoded_nc" "$@") \
    || ! summary=$(center_fields summary <<<"$body"); then
    fail "节点 $nc 概要查询失败"
    return
  fi
  IFS=$'\t' read -r enabled_tasks open_alerts latest_started latest_status created_at <<<"$summary"
  if ((open_alerts > 0)); then warn "节点 $nc 有 $open_alerts 条未处置告警"; fi
  if ((enabled_tasks == 0)); then
    info "节点 $nc 没有启用任务，跳过运行推进检查"
    return
  fi
  info "节点 $nc 最近一次运行: $latest_status，开始于 $latest_started"
  if ! body=$(center_api "/api/v1/task-runs?node_code=$encoded_nc&status=succeeded&limit=1" "$@") \
    || ! succeeded_at=$(center_fields succeeded <<<"$body"); then
    fail "节点 $nc 成功运行查询失败"
    return
  fi
  if [[ $succeeded_at == '-' ]]; then
    # 没成功过也需要判异常；失败运行明确报警，新节点按注册时间留等待窗口。
    reference=$latest_started
    [[ $reference != '-' ]] || reference=$created_at
    age=$(age_of "$reference")
    if ((max_run_age > 0)) && { [[ $latest_status == failed || -z $age ]] || ((age > max_run_age)); }; then
      fail "节点 $nc 有启用任务但一直没有成功运行（最近状态 $latest_status），检查采集和调度"
    else
      warn "节点 $nc 还没有成功完成的运行"
    fi
    return
  fi
  age=$(age_of "$succeeded_at")
  if [[ -z $age ]]; then
    fail "节点 $nc 最近成功运行时间无法解析: $succeeded_at"
  elif ((max_run_age > 0 && age > max_run_age)); then
    fail "节点 $nc 最近成功运行已是 $(fmt_age "$age") 前（阈值 $(fmt_age "$max_run_age")），持续输入场景下数据可能停止推进"
  else
    ok "节点 $nc 最近成功运行: $succeeded_at"
  fi
}

run_center() {
  local env_file=/etc/labbridge/production.env compose_file=deploy/production/compose.yaml
  local max_heartbeat_age=120 max_run_age=0 backup_dir='' max_backup_age_hours=0
  local min_free_mb=512 min_free_inodes=1000
  while (($#)); do
    (($# >= 2)) || { printf '选项缺少取值: %s\n' "$1" >&2; exit 2; }
    case $1 in
      --env-file) env_file=$2 ;;
      --compose) compose_file=$2 ;;
      --max-heartbeat-age-seconds) max_heartbeat_age=$2 ;;
      --max-run-age-seconds) max_run_age=$2 ;;
      --backup-dir) backup_dir=$2 ;;
      --max-backup-age-hours) max_backup_age_hours=$2 ;;
      --min-free-mb) min_free_mb=$2 ;;
      --min-free-inodes) min_free_inodes=$2 ;;
      *) printf '未知选项: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift 2
  done
  require_uint "$max_heartbeat_age" --max-heartbeat-age-seconds
  require_uint "$max_run_age" --max-run-age-seconds
  require_uint "$max_backup_age_hours" --max-backup-age-hours
  require_uint "$min_free_mb" --min-free-mb
  require_uint "$min_free_inodes" --min-free-inodes
  local tool
  for tool in docker python3 curl; do
    command -v "$tool" >/dev/null || { printf '缺少 %s\n' "$tool" >&2; exit 2; }
  done
  [[ -r $env_file && -r $compose_file ]] || { printf '环境文件或 Compose 文件不可读\n' >&2; exit 2; }
  local compose_cmd=(docker compose --env-file "$env_file" -f "$compose_file")
  printf '=== LabBridge 中心巡检（%s） ===\n' "$(date '+%F %T')"
  header 'Compose 服务状态'
  local body rows service state health services_ok=1
  if ! body=$("${compose_cmd[@]}" ps --all --format json) \
    || ! rows=$(center_fields services <<<"$body"); then
    fail '无法获取完整的 Compose 服务状态'
    services_ok=0
  else
    while IFS=$'\t' read -r service state health; do
      if [[ $state == running && $health == healthy ]]; then
        ok "服务 $service：running / healthy"
      else
        fail "服务 $service：$state / $health（应为 running / healthy）"
        services_ok=0
      fi
    done <<<"$rows"
  fi

  header 'Web 入口与认证'
  # 从宿主机访问实际发布端口，不用容器健康状态代替外部入口检查。
  local binding
  if ! binding=$("${compose_cmd[@]}" port web 8080) || [[ -z $binding ]]; then
    fail 'Web 未发布 8080 端口'
  else
    binding=${binding%%$'\n'*}
    binding=${binding/#0.0.0.0:/127.0.0.1:}
    binding=${binding/#\[::\]:/[::1]:}
    if curl --fail --silent --show-error --max-time 10 -o /dev/null "http://$binding/"; then
      ok "Web 发布入口 http://$binding/ 可访问"
    else
      fail "Web 发布入口 http://$binding/ 无法访问"
    fi
  fi
  if ! body=$(center_api '/api/v1/nodes?limit=1' "${compose_cmd[@]}") \
    || ! rows=$(center_fields nodes <<<"$body"); then
    fail '经 Web 反代的认证查询失败'
    services_ok=0
  else
    ok '经 Web 反代的认证查询通过'
  fi

  header '节点心跳与运行推进'
  if ((services_ok)); then
    local cursor='' path nc status hb hb_age next_cursor
    local -a node_codes=()
    while :; do
      path='/api/v1/nodes?limit=100'
      [[ -z $cursor ]] || path+="&cursor=$cursor"
      if ! body=$(center_api "$path" "${compose_cmd[@]}") \
        || ! rows=$(center_fields nodes <<<"$body"); then
        fail '节点列表查询失败'
        break
      fi
      next_cursor=''
      while IFS=$'\t' read -r nc status hb; do
        if [[ $nc == '@cursor' ]]; then
          [[ $status == '-' ]] || next_cursor=$status
          continue
        fi
        node_codes+=("$nc")
        hb_age=$(age_of "$hb")
        if [[ $status == offline || -z $hb_age ]] || ((hb_age > max_heartbeat_age)); then
          fail "节点 $nc 心跳异常: $status，最后心跳 $hb"
        else
          ok "节点 $nc：$status，最后心跳 $hb"
        fi
      done <<<"$rows"
      [[ -n $next_cursor ]] || break
      if [[ $next_cursor == "$cursor" ]]; then fail '节点列表分页未推进'; break; fi
      cursor=$next_cursor
    done
    if ((${#node_codes[@]} == 0)); then warn '中心还没有任何注册节点'; fi
    for nc in "${node_codes[@]}"; do
      check_node_progress "$nc" "$max_run_age" "${compose_cmd[@]}"
    done
  else
    info '服务不可用，跳过节点与运行检查'
  fi

  header '数据库磁盘与备份'
  local db_avail_mb db_avail_inodes
  if ! db_avail_mb=$("${compose_cmd[@]}" exec -T postgres df -B1M --output=avail /var/lib/postgresql/data | tail -n 1 | tr -d ' ') \
    || ! db_avail_inodes=$("${compose_cmd[@]}" exec -T postgres df --output=iavail /var/lib/postgresql/data | tail -n 1 | tr -d ' '); then
    fail '数据库磁盘信息查询失败'
  else
    info "数据库: 可用 ${db_avail_mb}MiB、inode $db_avail_inodes"
    if ((db_avail_mb < min_free_mb)); then fail '数据库可用空间不足，先暂停文件交付再扩容'; fi
    if ((db_avail_inodes < min_free_inodes)); then fail '数据库可用 inode 不足'; fi
  fi
  if [[ -n $backup_dir ]]; then check_disk "$backup_dir" '备份目录' "$min_free_mb" "$min_free_inodes"; fi
  report_backup '中心' "$backup_dir" "$max_backup_age_hours"
}
