---
title: 部署到 GitHub Pages
description: 通过 GitHub Actions 自动构建并发布文档站。
weight: 30
---

## 首次设置 {#first-time-setup}

1. 打开 GitHub 仓库的 **Settings → Pages**。
2. 在 **Build and deployment** 中将 **Source** 设为 **GitHub Actions**。
3. 将代码推送到 `main` 分支。
4. 在 **Actions** 页面等待 “Deploy Oink site to GitHub Pages” 完成。
{.steps}

## 自动发布 {#automatic-deployment}

每次推送到 `main`，工作流都会：

1. 安装固定版本的 Go 与 Hugo Extended。
2. 下载 `go.mod` 固定的 OINK 主题。
3. 使用严格模式构建站点。
4. 将 `public/` 作为 Pages artifact 发布。
{.steps}

站点地址：<https://churen-biz.github.io/rw/>

> [!TIP]
> 也可以在 GitHub 的 Actions 页面手动运行工作流，无需额外提交。
