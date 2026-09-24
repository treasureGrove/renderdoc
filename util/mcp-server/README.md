# RenderDoc MCP Server

把正在运行的 qrenderdoc 暴露给外部 AI 客户端（ZCode / Claude Code / Claude Desktop /
Cursor 等任何支持 MCP 的工具），让 AI 直接分析抓到的帧：列事件树、跳转事件、保存渲染
目标截图、甚至在 RenderDoc 里执行任意 python。

## 架构

```
AI 客户端 (MCP stdio) ──> rd_mcp.py ──TCP 127.0.0.1:39944──> rd_mcp_bridge.py (qrenderdoc 扩展)
     零依赖                    JSON 行协议，UI 线程执行                    ↑
                                                            用 pyrenderdoc API 干活
```

两个文件，都不需要第三方 python 包：

| 文件 | 跑在哪 | 作用 |
|------|--------|------|
| `rd_mcp_bridge.py` | qrenderdoc 进程内（python 扩展） | 起本地 TCP 监听，把工具请求封送到 UI 线程执行 |
| `rd_mcp.py` | AI 客户端拉起的子进程 | 标准输入/输出上的 MCP JSON-RPC，转发到 TCP 桥 |

## 安装

### 1. 安装 qrenderdoc 扩展

```powershell
cd util\mcp-server
powershell -ExecutionPolicy Bypass -File install.ps1
```

（手动方式：把 `rd_mcp_bridge.py` 和 `extension.json` 复制到
`%APPDATA%\qrenderdoc\extensions\renderdoc_mcp\`。）

### 2. 在 qrenderdoc 里启用

打开 qrenderdoc → **Tools → Manage Extensions**，勾选启用 **RenderDoc MCP Bridge**
（勾选后每次启动自动加载）。加载成功后，Python Shell 输出里会出现
`[rd_mcp_bridge] listening on 127.0.0.1:39944`。

### 3. 配置 AI 客户端

ZCode / Claude Code 的 MCP 配置（`~/.zcode/mcp.json` 或对应客户端的配置文件）：

```json
{
  "mcpServers": {
    "renderdoc": {
      "command": "python",
      "args": ["D:/projectGithub/renderdoc/util/mcp-server/rd_mcp.py"]
    }
  }
}
```

重启客户端后即可使用。多个 qrenderdoc 实例只有一个能占住 39944 端口；换端口用
环境变量 `RD_MCP_PORT`（扩展端和 server 端都要一致）。

## 工具一览

| 工具 | 说明 |
|------|------|
| `capture_info` | 当前加载的 capture：文件名、API、当前 EID、action 统计。先调它确认桥接活着 |
| `list_events` | 帧事件树文本（marker/draw/dispatch/clear/copy/present + EID），支持 `max_depth`/`max_lines` |
| `fetch_eid` | 跳转到指定事件并返回摘要：动作名、绘制参数、绑定的颜色输出和深度缓冲（尺寸/格式） |
| `get_texture` | 把指定事件的颜色输出/深度缓冲存成 PNG 并返回路径（AI 客户端随后可直接读图） |
| `run_python` | 在 RenderDoc UI 线程执行任意 python，可访问 `pyrenderdoc` / `renderdoc` / `qrenderdoc`；设置 `result` 变量返回值。万能兜底 |

典型用法（在 AI 客户端里）：

> "用 renderdoc 的 MCP 连一下，capture_info 看看加载了什么帧，然后 list_events
> 给我梳理整个渲染管线，对最重的 pass 用 fetch_eid 和 get_texture 深入看看。"

## 测试

桥接的线程/协议/分发链路有桩测试覆盖（假 renderdoc 模块 + 假 PySide2）：

```powershell
# 占位桩测试在会话中以脚本形式运行过；快速自检：
python -m py_compile util\mcp-server\rd_mcp.py util\mcp-server\rd_mcp_bridge.py
# MCP 协议握手：
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | python util\mcp-server\rd_mcp.py
```

## 已知限制

- `get_texture` / `fetch_eid` 会切换 UI 当前事件（副作用：AI 会"带着你看"它关注的事件）。
- 桥接一次只处理一个请求（串行），UI 线程忙时请求排队，默认 120s 超时。
- 扩展只在 qrenderdoc 运行时可用；无头（headless）批量分析请走 `renderdoc` python 模块的
  命令行接口，不经过本桥。
