# logs/ — AI Coding 日志

本目录存放 **086 队（地铁口野狗联盟）kid_buddy 作品开发过程中与 AI 工具的完整对话日志**，
与作品代码一并提交。**这不是示例，是真实开发记录**（示例的 `your-github-login/` 目录已删除）。

## 本队日志内容

| 项 | 值 |
| --- | --- |
| 提交者 | `Rustypudding` |
| 工具 | Claude Code（`claude-code`） |
| 时间跨度 | 2026-07-25 08:30 UTC → 2026-09-20 04:04 UTC |
| 活跃开发日 | 32 天 |
| 事件总数 | 17,680 条 |
| 其中工具调用 | 11,340 条 |
| 文件大小 | 约 42 MB |

整个开发过程在一个 Claude Code 会话中连续完成，所以只有一个 `.jsonl`：
界面重做、上游 5 个 PR、全部上板排障都在里面，没有分段或筛选。

## 目录结构

```text
logs/
└── Rustypudding/                                          # GitHub 用户名
    ├── manifest.json                                      # 会话清单（含事件总数）
    └── 2026-09-12/
        └── claude-code__583e52cf-e30b-4189-8ae1-9ef1194a8a82.jsonl
```

- 目录名 `2026-09-12` 是归集工具写入该文件的日期，**不是会话开始日期**；会话本身从 07-25 持续到 09-20。
- 每个 `.jsonl` 每行一个事件，字段定义见官方手册；只提交 JSONL 本身。

## 怎么核对

用组委会归集工具里的校验脚本跑一遍即可，期望输出 `✅ ALL OK`：

```bash
python3 tools/validate-log.py logs/
```

它会比对 `manifest.json` 里 `event_count` 与 JSONL 实际行数、检查 `seq` 是否连续、字段是否符合 schema。
本队当前提交的版本校验结果为 `ALL OK`（1 个文件 / 17,680 条事件）。

导出与提交的完整步骤、字段定义见
[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)。
