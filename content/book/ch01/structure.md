---
title: 目录与页面
linkTitle: 目录与页面
description: 用目录树和 front matter 组织内容。
book_number: '1.1'
weight: 10
---

## 推荐结构 {#recommended-structure}

每个目录中的 `_index.md` 负责描述这一节，普通 Markdown 文件负责具体内容。

| 文件 | 作用 | 是否进入侧栏 |
| --- | --- | --- |
| `_index.md` | 章节首页 | 是 |
| `topic.md` | 普通主题 | 是 |
| `draft.md` | 草稿主题 | 是 |
{#tbl-1-1 num="1-1" caption="Book 示例的基本文件类型。"}

## 创建页面 {#create-page}

```bash {num="1-1" caption="创建一个新的教程页面。" #eg-1-1}
mkdir -p content/book/ch03
touch content/book/ch03/_index.md
```

如[表 1-1](#tbl-1-1)所示，目录首页本身也属于阅读顺序。
