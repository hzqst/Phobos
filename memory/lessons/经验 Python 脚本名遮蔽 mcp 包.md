---
title: 经验 Python 脚本名遮蔽 mcp 包
type: note
permalink: phobos/lessons/经验-python-脚本名遮蔽-mcp-包
tags:
- lesson
- python
- mcp
---

# 经验 Python 脚本名遮蔽 mcp 包

- **触发信号**：客户端脚本报 `ImportError: cannot import name 'Client' from partially initialized module 'mcp' (most likely due to a circular import)`。
- **根因/约束**：脚本自己叫 `mcp.py`，`from mcp import Client` 导入的是脚本本身，而不是 SDK 包。
- **正确做法**：改名（例如 `mcpcall.py`），删除旁边的 `__pycache__`。运行时加 `-I`，避免从脚本目录或当前目录意外导入模块。
- **验证方法**：`python -I mcpcall.py ... game_status` 能返回状态 JSON。
- **适用范围**：所有用 `Debug/AutomationTests/sdk2` 环境驱动 Automation MCP 的临时脚本。

## Relations

- part_of [[Automation MCP]]
