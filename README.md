# RW 文档

基于 [OINK](https://oink.pgsty.com/) 与 Hugo Extended 构建的中文文档站，使用 GitHub Actions 发布到 GitHub Pages。

## 本地预览

前置依赖：

- Hugo Extended 0.160.1 或更高版本
- Go

```bash
hugo server
```

访问 <http://localhost:1313/>。修改 Markdown 后页面会自动刷新。

## 新增文档

在 `content/docs/` 下新增 Markdown 文件。目录结构就是左侧导航结构，页面的 `weight` 越小越靠前。

```markdown
---
title: 页面标题
description: 一句话说明页面内容。
weight: 30
---

正文从这里开始。
```

## 构建与发布

本地执行严格生产构建：

```bash
hugo --gc --minify --printPathWarnings --panicOnWarning
```

推送到 `main` 后，`.github/workflows/pages.yml` 会构建并发布 `public/`。首次发布前，在 GitHub 仓库中打开 **Settings → Pages → Build and deployment**，将 **Source** 设为 **GitHub Actions**。

也可以使用仓库内的一键发布脚本。首次使用先登录 GitHub CLI：

```bash
gh auth login
./scripts/publish-docs.sh --install --commit "Publish docs" --make-public
```

日常发布已有提交：

```bash
./scripts/publish-docs.sh
```

只做环境与文档检查，不访问或修改 GitHub：

```bash
./scripts/publish-docs.sh --check-only
```

运行 `./scripts/publish-docs.sh --help` 可查看所有参数与安全保护。
