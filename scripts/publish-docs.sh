#!/usr/bin/env bash

set -Eeuo pipefail

MIN_HUGO_VERSION="0.160.1"
MIN_GO_VERSION="1.27.0"
REMOTE="origin"
WORKFLOW="pages.yml"
INSTALL=false
CHECK_ONLY=false
MAKE_PUBLIC=false
YES=false
COMMIT_MESSAGE=""

usage() {
  cat <<'EOF'
检查、构建并发布 Oink 文档站到 GitHub Pages。

用法：
  ./scripts/publish-docs.sh [选项]

选项：
  --install              使用 Homebrew 安装或升级缺失的 git/go/hugo/gh
  --check-only           只检查环境和执行严格构建，不提交或发布
  --commit MESSAGE       暂存文档站文件并以 MESSAGE 创建提交
  --make-public          仓库为私有时，将其改为公开（GitHub Pages 免费方案需要）
  --remote NAME          Git 远端名称，默认 origin
  --workflow FILE        Pages 工作流文件名，默认 pages.yml
  --yes                  跳过公开仓库前的交互确认
  -h, --help             显示帮助

示例：
  # 首次完整发布
  ./scripts/publish-docs.sh --install --commit "Publish docs" --make-public

  # 日常发布已有提交
  ./scripts/publish-docs.sh

  # CI 前的本地检查
  ./scripts/publish-docs.sh --check-only

前提：
  - GitHub CLI 已通过 `gh auth login` 登录；
  - origin 指向 GitHub 仓库；
  - `.github/workflows/pages.yml` 支持 workflow_dispatch。
EOF
}

log() {
  printf '\n\033[1;34m==>\033[0m %s\n' "$*"
}

ok() {
  printf '\033[1;32m✓\033[0m %s\n' "$*"
}

die() {
  printf '\033[1;31m错误：\033[0m %s\n' "$*" >&2
  exit 1
}

version_ge() {
  awk -v actual="$1" -v required="$2" 'BEGIN {
    split(actual, a, "."); split(required, r, ".");
    for (i = 1; i <= 3; i++) {
      av = (a[i] == "" ? 0 : a[i]) + 0;
      rv = (r[i] == "" ? 0 : r[i]) + 0;
      if (av > rv) exit 0;
      if (av < rv) exit 1;
    }
    exit 0;
  }'
}

brew_install_or_upgrade() {
  local formula="$1"
  command -v brew >/dev/null 2>&1 || die "未找到 Homebrew。请先安装：https://brew.sh/"
  if brew list --versions "$formula" >/dev/null 2>&1; then
    brew upgrade "$formula"
  else
    brew install "$formula"
  fi
}

ensure_command() {
  local command_name="$1"
  local formula="$2"
  if command -v "$command_name" >/dev/null 2>&1; then
    return
  fi
  if [[ "$INSTALL" != true ]]; then
    die "未找到 $command_name。重新运行并添加 --install，或先手动安装。"
  fi
  [[ "$(uname -s)" == "Darwin" ]] || die "--install 当前仅自动支持 macOS/Homebrew；请手动安装 $formula。"
  log "安装 $formula"
  brew_install_or_upgrade "$formula"
  hash -r
}

refresh_homebrew_path() {
  if command -v brew >/dev/null 2>&1; then
    local brew_prefix
    brew_prefix="$(brew --prefix)"
    PATH="$brew_prefix/bin:$PATH"
    export PATH
  fi
}

check_hugo() {
  local output version
  output="$(hugo version)"
  [[ "$output" == *extended* ]] || die "必须使用 Hugo Extended；当前输出：$output"
  version="$(printf '%s\n' "$output" | sed -E 's/^hugo v([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
  if ! version_ge "$version" "$MIN_HUGO_VERSION"; then
    if [[ "$INSTALL" == true && "$(uname -s)" == "Darwin" ]]; then
      log "升级 Hugo Extended（需要 >= $MIN_HUGO_VERSION）"
      brew_install_or_upgrade hugo
      hash -r
      output="$(hugo version)"
      version="$(printf '%s\n' "$output" | sed -E 's/^hugo v([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
    fi
  fi
  version_ge "$version" "$MIN_HUGO_VERSION" || die "Hugo $version 过旧，需要 >= $MIN_HUGO_VERSION。"
  ok "$output"
}

check_go() {
  local output version
  output="$(go version)"
  version="$(printf '%s\n' "$output" | sed -E 's/.* go([0-9]+\.[0-9]+(\.[0-9]+)?).*/\1/')"
  if ! version_ge "$version" "$MIN_GO_VERSION"; then
    if [[ "$INSTALL" == true && "$(uname -s)" == "Darwin" ]]; then
      log "升级 Go（Oink v0.8.0 需要 >= $MIN_GO_VERSION）"
      brew_install_or_upgrade go
      refresh_homebrew_path
      hash -r
      output="$(go version)"
      version="$(printf '%s\n' "$output" | sed -E 's/.* go([0-9]+\.[0-9]+(\.[0-9]+)?).*/\1/')"
    fi
  fi
  version_ge "$version" "$MIN_GO_VERSION" || die "Go $version 过旧，需要 >= $MIN_GO_VERSION。"
  ok "$output"
}

github_slug_from_remote() {
  local url="$1"
  url="${url%.git}"
  case "$url" in
    https://github.com/*)
      printf '%s\n' "${url#https://github.com/}"
      ;;
    git@github.com:*)
      printf '%s\n' "${url#git@github.com:}"
      ;;
    ssh://git@github.com/*)
      printf '%s\n' "${url#ssh://git@github.com/}"
      ;;
    *)
      die "远端不是可识别的 GitHub 地址：$url"
      ;;
  esac
}

confirm_public_change() {
  if [[ "$YES" == true ]]; then
    return
  fi
  printf '仓库将从私有改为公开。输入 public 继续：' >&2
  local answer
  read -r answer
  [[ "$answer" == "public" ]] || die "已取消公开仓库。"
}

stage_docs_site() {
  local candidates=(
    .github/workflows
    .gitignore
    README.md
    assets
    content
    data
    examples
    go.mod
    go.sum
    hugo.yml
    layouts
    scripts
    static
  )
  local existing=()
  local path
  for path in "${candidates[@]}"; do
    if [[ -e "$path" ]] || [[ -n "$(git ls-files -- "$path")" ]]; then
      existing+=("$path")
    fi
  done
  git add --all -- "${existing[@]}"
}

wait_for_run() {
  local repo="$1"
  local branch="$2"
  local sha="$3"
  local run_id=""
  local attempt

  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    run_id="$(gh run list --repo "$repo" --workflow "$WORKFLOW" --branch "$branch" --commit "$sha" --limit 1 --json databaseId --jq '.[0].databaseId // empty')"
    if [[ -n "$run_id" ]]; then
      printf '%s\n' "$run_id"
      return
    fi
    sleep 3
  done
  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install)
      INSTALL=true
      shift
      ;;
    --check-only)
      CHECK_ONLY=true
      shift
      ;;
    --commit)
      [[ $# -ge 2 ]] || die "--commit 需要提交信息。"
      COMMIT_MESSAGE="$2"
      shift 2
      ;;
    --make-public)
      MAKE_PUBLIC=true
      shift
      ;;
    --remote)
      [[ $# -ge 2 ]] || die "--remote 需要远端名称。"
      REMOTE="$2"
      shift 2
      ;;
    --workflow)
      [[ $# -ge 2 ]] || die "--workflow 需要文件名。"
      WORKFLOW="$2"
      shift 2
      ;;
    --yes)
      YES=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "未知参数：$1（使用 --help 查看帮助）"
      ;;
  esac
done

refresh_homebrew_path
ensure_command git git
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || die "当前目录不在 Git 仓库中。"

log "检查环境"
ensure_command go go
ensure_command hugo hugo
ensure_command gh gh
ensure_command curl curl
ensure_command cc llvm
check_go
check_hugo
ok "Git $(git --version | awk '{print $3}')"
ok "GitHub CLI $(gh --version | awk 'NR == 1 {print $3}')"

[[ -f hugo.yml ]] || die "仓库根目录缺少 hugo.yml。"
[[ -f go.mod ]] || die "仓库根目录缺少 go.mod。"
[[ -d content ]] || die "仓库根目录缺少 content/。"
[[ -f ".github/workflows/$WORKFLOW" ]] || die "缺少 .github/workflows/$WORKFLOW。"

log "检查文档并执行严格生产构建"
git diff --check
make -C examples/stack-vm clean test demo
go mod download github.com/pgsty/oink
hugo --cleanDestinationDir --gc --minify --environment production \
  --printPathWarnings --panicOnWarning
ok "文档构建通过"

if [[ "$CHECK_ONLY" == true ]]; then
  ok "检查完成；--check-only 未修改 GitHub 状态。"
  exit 0
fi

log "检查 Git 与 GitHub"
gh auth status >/dev/null 2>&1 || die "GitHub CLI 尚未登录。请先运行：gh auth login"
git remote get-url "$REMOTE" >/dev/null 2>&1 || die "不存在 Git 远端：$REMOTE"
REMOTE_URL="$(git remote get-url "$REMOTE")"
REPO="$(github_slug_from_remote "$REMOTE_URL")"
BRANCH="$(git symbolic-ref --quiet --short HEAD)" || die "当前处于 detached HEAD，无法发布。"
[[ "$BRANCH" == "main" ]] || die "Pages 工作流只监听 main；当前分支是 $BRANCH。"
ok "仓库 $REPO，分支 $BRANCH"

if [[ -n "$COMMIT_MESSAGE" ]]; then
  log "暂存并提交文档站文件"
  stage_docs_site
  git diff --cached --check
  if git diff --cached --quiet; then
    ok "没有需要提交的文档站改动"
  else
    git commit -m "$COMMIT_MESSAGE"
    ok "已创建提交 $(git rev-parse --short HEAD)"
  fi
elif [[ -n "$(git status --porcelain)" ]]; then
  git status --short
  die "工作区存在未提交改动。使用 --commit MESSAGE 提交文档站文件，或先自行处理。"
fi

VISIBILITY="$(gh repo view "$REPO" --json visibility --jq .visibility)"
if [[ "$VISIBILITY" == "PRIVATE" ]]; then
  [[ "$MAKE_PUBLIC" == true ]] || die "仓库为私有。免费 Pages 需要公开仓库；确认后添加 --make-public。"
  confirm_public_change
  gh repo edit "$REPO" --visibility public --accept-visibility-change-consequences
  ok "仓库已改为公开"
fi

log "启用 GitHub Pages"
if gh api "repos/$REPO/pages" >/dev/null 2>&1; then
  gh api --method PUT "repos/$REPO/pages" -f build_type=workflow >/dev/null
else
  gh api --method POST "repos/$REPO/pages" -f build_type=workflow >/dev/null
fi
PAGES_URL="$(gh api "repos/$REPO/pages" --jq .html_url)"
ok "Pages 地址：$PAGES_URL"

log "推送代码"
git push --set-upstream "$REMOTE" "$BRANCH"
SHA="$(git rev-parse HEAD)"

RUN_ID="$(wait_for_run "$REPO" "$BRANCH" "$SHA" || true)"
if [[ -z "$RUN_ID" ]]; then
  log "当前提交没有自动触发工作流，执行 workflow_dispatch"
  gh workflow run "$WORKFLOW" --repo "$REPO" --ref "$BRANCH"
  RUN_ID="$(wait_for_run "$REPO" "$BRANCH" "$SHA" || true)"
fi
[[ -n "$RUN_ID" ]] || die "未找到当前提交对应的 Pages 工作流。"

log "等待 GitHub Actions 完成"
gh run watch "$RUN_ID" --repo "$REPO" --exit-status
ok "GitHub Actions 发布成功"

log "验证公网站点"
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12; do
  if curl --fail --silent --show-error --location --output /dev/null "$PAGES_URL"; then
    ok "站点可以访问：$PAGES_URL"
    printf '\n发布完成。\n仓库：https://github.com/%s\n站点：%s\n工作流：https://github.com/%s/actions/runs/%s\n' \
      "$REPO" "$PAGES_URL" "$REPO" "$RUN_ID"
    exit 0
  fi
  sleep 5
done

die "工作流成功，但站点在等待期内仍不可访问：$PAGES_URL"
