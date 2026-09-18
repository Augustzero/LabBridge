#!/usr/bin/env bash
#
# 构建并导出中心部署镜像（Server / Web / PostgreSQL），连同配置模板和
# SHA-256 清单一起放进一个产物目录，供离线现场 docker load 使用。
# 产物目录必须不存在，一批产物一个新目录，避免和旧批次混放。
#
# 用法:
#   bash scripts/ops/export-images.sh <output-dir> [--skip-build]
#
# --skip-build 跳过构建，直接导出本地已有的同名 tag 镜像。
# 要求工作区已提交的改动是干净的（git diff 为空），保证产物能对上源码 revision。

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"

skip_build=0
if [[ $# -eq 2 && $2 == --skip-build ]]; then
  skip_build=1
elif [[ $# -ne 1 ]]; then
  printf 'Usage: %s <output-dir> [--skip-build]\n' "${BASH_SOURCE[0]}" >&2
  exit 1
fi
output_dir=$1

if ! command -v git >/dev/null 2>&1; then
  printf 'git is required to record the source revision.\n' >&2
  exit 1
fi
if ! command -v docker >/dev/null 2>&1; then
  printf 'Docker CLI is required. Install Docker Engine first.\n' >&2
  exit 1
fi
if ! docker info >/dev/null 2>&1; then
  printf 'Docker daemon is unavailable. Start Docker and retry.\n' >&2
  exit 1
fi
cd "$repo_root"

# 产物要能对上源码 revision；有未提交改动就先提交或暂存，别导出说不清来源的镜像。
if ! git diff --quiet || ! git diff --cached --quiet; then
  printf 'Working tree has uncommitted changes; commit or stash them before exporting.\n' >&2
  exit 1
fi
if [[ -e $output_dir ]]; then
  printf 'Output directory %s already exists; use a fresh one per batch.\n' "$output_dir" >&2
  exit 1
fi

# 未跟踪文件也会进构建上下文；tracked diff 检查拦不住这种情况，
# 数量记进清单，交接产物时能看出来这批镜像不是纯 revision 构建的。
untracked_count=$(git status --porcelain | wc -l)
if (( untracked_count > 0 )); then
  printf 'Note: %s untracked file(s) present; they enter the build context and are recorded in the manifest.\n' "$untracked_count"
fi

revision=$(git rev-parse HEAD)
# tag 默认用短 revision，离线现场照着清单填 LABBRIDGE_IMAGE_TAG 即可。
tag="${LABBRIDGE_IMAGE_TAG:-$(git rev-parse --short HEAD)}"
postgres_image="${LABBRIDGE_POSTGRES_IMAGE:-postgres:16.15-bookworm}"
server_image="labbridge/server:$tag"
web_image="labbridge/web:$tag"

mkdir -p -- "$output_dir/images" "$output_dir/config"

if (( skip_build == 0 )); then
  printf '[1/3] building server and web images (tag %s)\n' "$tag"
  # 直接走 deploy/production/compose.yaml 的 build 定义：和现场部署用同一条
  # 构建路径与缓存，导出的镜像就是 compose 会启动的镜像。
  # 构建本身用不到秘密，插值需要的环境变量用哑值满足即可。
  build_env=$(mktemp)
  cleanup() { rm -f -- "$build_env"; }
  trap cleanup EXIT
  {
    printf 'LABBRIDGE_POSTGRES_PASSWORD=export-only-dummy\n'
    printf 'LABBRIDGE_IMAGE_TAG=%s\n' "$tag"
    printf 'LABBRIDGE_AUTH_DIR=/nonexistent\n'
    printf 'LABBRIDGE_AUTH_GROUP=0\n'
  } > "$build_env"
  docker compose --env-file "$build_env" -f deploy/production/compose.yaml build server web
  # PostgreSQL 镜像本地已有就直接用；全新构建机才需要联网拉取。
  if ! docker image inspect "$postgres_image" >/dev/null 2>&1; then
    docker pull "$postgres_image"
  fi
else
  printf '[1/3] skipping build; using local images with tag %s\n' "$tag"
  docker image inspect "$server_image" "$web_image" "$postgres_image" >/dev/null
fi

printf '[2/3] saving images and copying config templates\n'
docker save -o "$output_dir/images/labbridge-server_$tag.tar" "$server_image"
docker save -o "$output_dir/images/labbridge-web_$tag.tar" "$web_image"
# 镜像名里的 / 和 : 换成 _ ，当文件名用。
postgres_tar_name=${postgres_image//\//_}
postgres_tar_name=${postgres_tar_name//:/_}
docker save -o "$output_dir/images/$postgres_tar_name.tar" "$postgres_image"

# 离线现场可能只拿到这个产物目录，把部署要用的配置模板和 schema 一起带上。
cp -a --parents \
  deploy/production/server.yaml \
  deploy/production/nginx.conf \
  deploy/production/compose.yaml \
  deploy/production/generate-credentials.sh \
  deploy/env/production.env.example \
  deploy/schema.sql \
  deploy/migrations \
  "$output_dir/config/"

printf '[3/3] writing manifest and SHA-256 checksums\n'
image_id() { docker image inspect --format '{{.Id}}' "$1"; }

manifest="$output_dir/manifest.txt"
{
  printf 'LabBridge center deployment artifacts\n'
  printf 'exported_at_utc: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'source_revision: %s\n' "$revision"
  printf 'image_tag: %s\n' "$tag"
  printf 'untracked_files_in_context: %s\n' "$untracked_count"
  printf 'docker_version: %s\n\n' "$(docker version --format '{{.Server.Version}}')"

  printf 'image_id labbridge/server: %s\n' "$(image_id "$server_image")"
  printf 'image_id labbridge/web:    %s\n' "$(image_id "$web_image")"
  printf 'image_id %s: %s\n\n' "$postgres_image" "$(image_id "$postgres_image")"

  printf 'offline install order:\n'
  printf '  docker load -i images/labbridge-server_%s.tar\n' "$tag"
  printf '  docker load -i images/labbridge-web_%s.tar\n' "$tag"
  printf '  docker load -i images/%s.tar\n' "$postgres_tar_name"
  printf '  then follow docs/operations/deployment.md; set LABBRIDGE_IMAGE_TAG=%s\n' "$tag"
  printf '  in the env file to the tag above.\n'
} > "$manifest"

# 校验和覆盖镜像包和配置模板，恢复现场时先校再装。
(
  cd "$output_dir"
  sha256sum images/*.tar config/deploy/schema.sql \
    config/deploy/production/* config/deploy/env/* config/deploy/migrations/* \
    > SHA256SUMS
)

printf 'done: %s\n' "$output_dir"
printf 'verify with: (cd %q && sha256sum -c SHA256SUMS)\n' "$output_dir"
