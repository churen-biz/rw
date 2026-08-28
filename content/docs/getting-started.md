---
title: 开始使用
description: 在本地启动并浏览 RW 文档站。
weight: 10
---

## 环境要求 {#requirements}

> [!IMPORTANT]
> 必须使用 **Hugo Extended 0.160.1 或更高版本**。标准版 Hugo 不包含主题所需的 Sass 编译器。

确认本地工具：

```bash
hugo version
go version
```

`hugo version` 的输出中应包含 `extended`。

## 启动预览 {#preview}

1. 克隆仓库。
2. 在仓库根目录运行 `hugo server`。
3. 打开 <http://localhost:1313/>。
4. 修改 `content/docs/` 中的 Markdown，浏览器会自动刷新。
{.steps}

```bash
git clone https://github.com/churen-biz/rw.git
cd rw
hugo server
```

## 生产构建 {#production-build}

提交前运行严格构建：

```bash
hugo --gc --minify --printPathWarnings --panicOnWarning
```

出现任何警告时，构建会立即失败，避免把断链或错误配置发布到线上。
