#!/usr/bin/env bash
#
# 打包 Agent 现场安装产物：用 deploy/demo/Dockerfile 的 agent-runtime 阶段
# 构建二进制（与中心 Server 镜像同一条构建定义、同一个 pinned 基础镜像），
# 连同 systemd service、正式配置模板、运行依赖清单和 SHA-256 校验放进
# 一个新目录。现场（含灾后恢复）只用这包安装，不从源码重新构建。
#
# 用法:
#   bash scripts/ops/package-agent.sh <output-dir>
#
# 产物目录必须不存在，一批一个新目录。要求工作区已提交的改动干净
# （git diff 为空），保证产物能对上源码 revision。

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"

if [[ $# -ne 1 ]]; then
  printf 'Usage: %s <output-dir>\n' "${BASH_SOURCE[0]}" >&2
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

# 和镜像导出同一条规矩：有未提交改动就先提交或暂存，别打出说不清来源的包。
if ! git diff --quiet || ! git diff --cached --quiet; then
  printf 'Working tree has uncommitted changes; commit or stash them before packaging.\n' >&2
  exit 1
fi
if [[ -e $output_dir ]]; then
  printf 'Output directory %s already exists; use a fresh one per batch.\n' "$output_dir" >&2
  exit 1
fi

# 未跟踪文件会进构建上下文，tracked diff 检查拦不住，数量如实记进清单。
untracked_count=$(git status --porcelain | wc -l)
if (( untracked_count > 0 )); then
  printf 'Note: %s untracked file(s) present; they enter the build context and are recorded in the manifest.\n' "$untracked_count"
fi

revision=$(git rev-parse HEAD)
tag="$(git rev-parse --short HEAD)"
image="labbridge/agent-package:$tag"

printf '[1/4] building agent binary (target agent-runtime, image %s)\n' "$image"
# 只构建 agent-runtime 阶段（它会连带编译 builder 阶段的 C++ 目标），
# 和中心镜像用同一份 Dockerfile、同一个 pinned Ubuntu 基础镜像。
# 有 buildx 就走 BuildKit；没有 buildx 的机器退回 legacy builder，
# 代价是另起一层构建缓存（重复拉基础镜像和依赖），多花时间和流量。
if docker buildx version >/dev/null 2>&1; then
  docker buildx build \
    --file deploy/demo/Dockerfile \
    --target agent-runtime \
    --tag "$image" \
    .
else
  printf 'Note: buildx unavailable, falling back to the legacy builder (slower, separate cache).\n'
  docker build \
    --file deploy/demo/Dockerfile \
    --target agent-runtime \
    --tag "$image" \
    .
fi

printf '[2/4] extracting binary and runtime dependency versions\n'
mkdir -p -- "$output_dir/bin" "$output_dir/systemd" "$output_dir/config"
# 镜像里二进制在 /usr/local/bin；用一次性容器拷出来，不动镜像。
cid=$(docker create "$image")
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
docker cp "$cid":/usr/local/bin/labbridge_agent "$output_dir/bin/labbridge_agent"
docker rm "$cid" >/dev/null
trap - EXIT

# 把构建环境里实际生效的运行依赖版本记下来，现场核对 apt 装的版本不低于这些。
# Agent 运行只需要这四个包（含被依赖的 glibc 等，apt 会自动带上）。
deps_file="$output_dir/dependencies.txt"
{
  printf '# Agent 运行依赖（构建环境实际版本，现场安装版本不应低于此）\n'
  docker run --rm --entrypoint /usr/bin/dpkg-query "$image" \
    --show --showformat='${Package} ${Version}\n' \
    libsqlite3-0 libssl3t64 libyaml-cpp0.8 ca-certificates
  printf 'glibc '
  docker run --rm --entrypoint /usr/bin/ldd "$image" --version | head -1
} > "$deps_file"

printf '[3/4] copying service unit and config template\n'
cp -- deploy/systemd/labbridge-agent.service "$output_dir/systemd/"
cp -- deploy/env/agent.production.example.yaml "$output_dir/config/"

printf '[4/4] writing manifest and SHA-256 checksums\n'
manifest="$output_dir/manifest.txt"
{
  printf 'LabBridge agent deployment artifacts\n'
  printf 'exported_at_utc: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'source_revision: %s\n' "$revision"
  printf 'untracked_files_in_context: %s\n' "$untracked_count"
  printf 'binary_sha256: %s\n' "$(sha256sum "$output_dir/bin/labbridge_agent" | cut -d' ' -f1)"
  printf 'binary_bytes: %s\n\n' "$(stat -c %s "$output_dir/bin/labbridge_agent")"

  printf 'runtime dependencies: dependencies.txt\n'
  printf 'install guide: docs/operations/agent-deployment.md (same source revision)\n\n'

  printf 'install order:\n'
  printf '  verify with: sha256sum -c SHA256SUMS\n'
  printf '  install runtime deps (see dependencies.txt)\n'
  printf '  binary  -> /opt/labbridge/releases/<revision>/labbridge_agent, symlink /opt/labbridge/current\n'
  printf '  unit    -> /etc/systemd/system/labbridge-agent.service\n'
  printf '  config  -> /etc/labbridge/agent.yaml (from config/agent.production.example.yaml)\n'
  printf '  then follow docs/operations/agent-deployment.md\n'
} > "$manifest"

(
  cd "$output_dir"
  sha256sum bin/labbridge_agent systemd/labbridge-agent.service \
    config/agent.production.example.yaml dependencies.txt > SHA256SUMS
)

printf 'done: %s\n' "$output_dir"
printf 'verify with: (cd %q && sha256sum -c SHA256SUMS)\n' "$output_dir"
