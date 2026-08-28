---
title: 写作指南
description: 用 Markdown 编写结构清晰的 OINK 文档。
weight: 20
---

## 创建页面 {#create-a-page}

在 `content/docs/` 中新增 `.md` 文件，并为页面添加 front matter：

```markdown
---
title: 页面标题
description: 一句话说明页面内容。
weight: 30
---
```

目录结构会自动成为左侧导航结构，`weight` 越小，页面越靠前。

## 常用组件 {#components}

### 提示框 {#callout}

```markdown
> [!NOTE]
> 这是一条补充说明。
```

> [!NOTE]
> OINK 的组件仍然是 Markdown，源文件在 GitHub、编辑器和 AI 工具中都容易阅读。

### 标签页 {#tabs}

````markdown
```bash {tab="macOS" group="os" value="macos"}
brew install hugo go
```

```powershell {tab="Windows" group="os" value="windows"}
winget install Hugo.Hugo.Extended
winget install GoLang.Go
```
````

```bash {tab="macOS" group="os-demo" value="macos"}
brew install hugo go
```

```powershell {tab="Windows" group="os-demo" value="windows"}
winget install Hugo.Hugo.Extended
winget install GoLang.Go
```

## 稳定链接 {#stable-anchors}

为标题显式添加 `{#anchor}`。即使以后修改标题文字或增加其他语言，链接也能保持稳定。
