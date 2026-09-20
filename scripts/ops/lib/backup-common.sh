#!/usr/bin/env bash
# 只统一批次校验和完成标记，中心与节点的导出步骤各自保留。
backup_checksums() (
  local batch_dir=$1
  shift
  cd -- "$batch_dir"
  sha256sum -- "$@" > SHA256SUMS
  sha256sum -c SHA256SUMS > /dev/null
)

publish_backup() {
  local batch_dir=$1
  # 数据和完整清单先落盘，最终名字只在全部成功后出现。
  sync -f -- "$batch_dir"
  mv -T -- "$batch_dir/manifest.pending" "$batch_dir/manifest.txt"
  if ! sync -f -- "$batch_dir"; then
    rm -- "$batch_dir/manifest.txt"
    return 1
  fi
}
