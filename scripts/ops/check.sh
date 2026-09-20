#!/usr/bin/env bash
#
# LabBridge 只读巡检脚本，两种模式：
#   center —— 在中心机检查 Compose 服务状态、认证、节点心跳、运行推进、
#             数据库磁盘与最近备份；
#   agent  —— 在节点机检查 systemd 服务、SQLite 队列积压与失败原因、
#             输入/队列/归档目录磁盘与最近备份。
# 只做读取和查询，不改任何业务状态；发现问题返回非零，并指出节点、路径或作业。
# 面向 Ubuntu 24.04（依赖 GNU date/df/find/stat）；center 模式还需要
# docker、python3、curl，agent 模式还需要 sqlite3、python3-yaml。
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

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/lib/check-common.sh"

main() {
  if (($# == 0)) || [[ $1 == -h || $1 == --help ]]; then
    usage
    exit 0
  fi
  local mode=$1
  shift
  case $mode in
    center) source "$script_dir/lib/check-center.sh"; run_center "$@" ;;
    agent) source "$script_dir/lib/check-agent.sh"; run_agent "$@" ;;
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
