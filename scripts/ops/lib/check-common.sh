#!/usr/bin/env bash
# 巡检的输出计数集中在这里；检查函数的阈值和路径显式传入。
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
  local path=$1 label=$2 min_free_mb=$3 min_free_inodes=$4
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
  local label=$1 backup_dir=$2 max_backup_age_hours=$3
  if [[ -z $backup_dir ]]; then
    info '未指定 --backup-dir，跳过最近备份检查'
    return
  fi
  if [[ ! -d $backup_dir ]]; then
    fail "$label 备份目录不存在: $backup_dir"
    return
  fi
  local newest='' newest_age='' candidate created_at age_s age_h
  # 复制文件会改变 mtime，备份新旧只按清单里的完成时间判断。
  while IFS= read -r -d '' candidate; do
    created_at=$(sed -n 's/^created_at_utc: //p' "$candidate")
    age_s=$(age_of "$created_at")
    if [[ -z $created_at || -z $age_s || ! -s ${candidate%/*}/SHA256SUMS ]]; then
      fail "$label 备份清单不完整: $candidate"
      continue
    fi
    if [[ -z $newest ]] || ((age_s < newest_age)); then
      newest=$candidate
      newest_age=$age_s
    fi
  done < <(find "$backup_dir" -mindepth 2 -maxdepth 2 -type f -name manifest.txt -print0)
  if [[ -z $newest ]]; then
    if ((max_backup_age_hours > 0)); then
      fail "$label 在 $backup_dir 下没有找到完整备份批次"
    else
      warn "$label 在 $backup_dir 下没有找到完整备份批次"
    fi
    return
  fi
  age_s=$newest_age
  age_h=$((age_s / 3600))
  info "$label 最近完整备份批次: $newest（${age_h} 小时前）"
  if ((max_backup_age_hours > 0 && age_s > max_backup_age_hours * 3600)); then
    fail "$label 最近备份已 ${age_h} 小时（超过 ${max_backup_age_hours} 小时，$newest），按约定交负责人处理"
  fi

}


# 阈值来自命令行，只在这个外部边界校验。
require_uint() {
  local value=$1 option=$2
  [[ $value =~ ^(0|[1-9][0-9]{0,8})$ ]] || {
    printf '%s 必须是 0～999999999 的整数: %s\n' "$option" "$value" >&2
    exit 2
  }
}
