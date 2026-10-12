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

（手动方式：把 `__init__.py`、`rd_mcp_bridge.py`、`rd_frame_stats.py` 和 `extension.json`
复制到 `%APPDATA%\qrenderdoc\extensions\renderdoc_mcp\`。）

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
| `read_texture_b64` | 同上但直接返回 base64 PNG 内容（可指定输出序号/深度、最长边限制） |
| `pixel_history` | 查询单个像素的完整历史：每帧内每次修改的前后值与 rejection 标志（discard/深度/模板失败等） |
| `frame_hotspots` | 一页报告：按 pass 汇总三角形开销、最重的 draw、最大的纹理和 buffer。若已经采样过 GPU 计数器，报告里会带上毫秒和调用次数；`refresh=true` 才重新回放采样 |
| `pass_timings` | 每个 pass 的 GPU 毫秒、占帧比例、draw/dispatch、估算三角形，以及驱动返回的 PS/CS/VS、samples passed、IA primitives。`refresh=true` 会回放整帧（可能较慢，结果不在 .rdc 里）；不传则复用 UI 线程上的上次采样 |
| `pass_costs` | 每个 marker pass 的 draw/dispatch 数、估算三角形、计算线程组，按开销排序 |
| `heaviest_draws` | 按 `索引数 × 实例数` 排序的 draw，返回可跳转的 EID |
| `search_events` | 按名字子串找事件（如 `shadow` / `gbuffer` / `bloom`） |
| `resource_census` | 按字节大小列出最大的纹理和 buffer，并给出显存合计 |
| `get_shader` | 指定事件上绑定的 shader：入口、常量缓冲、贴图、采样器，以及截断后的反汇编 |
| `run_python` | 在 RenderDoc UI 线程执行任意 python，可访问 `pyrenderdoc` / `renderdoc` / `qrenderdoc`；设置 `result` 变量返回值。万能兜底 |

启用扩展后，qrenderdoc 菜单 **Tools → Frame Hotspots** 打开同一份几何/显存报告（不经过 MCP）。
选中表格里的一行再点 **Jump to EID** 会跳到对应事件；**Find** 按 pass/draw 名字过滤；
**Copy** 把 Markdown 拷进剪贴板。三角形按 `索引数 / 3 × 实例数` 估算，triangle strip 和
报告 0 索引的 indirect draw 会在报告开头注明。

**Tools → Pass Timings** 是单独的可停靠面板，用来管每个 pass 的 GPU 时间：

- 打开后先看到三角形、draw、dispatch 估算，状态是 Idle。不会在加载 capture 时自动采样。
- **Sample GPU counters** 通过 `FetchCounters` 回放整帧，读取 `EventGPUDuration`（秒，面板显示为毫秒）以及驱动实际支持的 PS/CS/VS invocations、samples passed、IA primitives。回放在后台进行，其余界面可以继续用。
- 上表是 pass：GPU 毫秒、占整帧比例、draw、dispatch、估算三角形。选中一行后，下表列出构成这个 pass 的事件，毫秒相加等于该 pass，不含 marker 自己的时间戳。
- 表头给出整帧 GPU 毫秒、三角形和调用次数合计。默认按 GPU 毫秒从高到低排。
- 双击 pass 跳到该 marker；双击下面的事件，或选中它再点 **Jump**，会 `SetEventID` 到那个 draw。**Copy as markdown** 同时复制 pass 表和当前 pass 的事件表。
- Marker 的时间是子事件之和（和 Event Browser 的 Duration 列一样），父级自己的时间戳不会再加一遍，避免重复计算。
- 驱动不支持 `EventGPUDuration` 时，面板会写明毫秒不可用，三角形估算仍然在。
- 这些毫秒和计数器不在 `.rdc` 里，只存在这次 qrenderdoc 会话的内存缓存中。MCP 的 `pass_timings` / `frame_hotspots` 会读这份缓存；只有 `refresh=true` 才再回放一次。

典型用法（在 AI 客户端里）：

> "用 renderdoc 的 MCP 连一下，capture_info 看看加载了什么帧，然后 list_events
> 给我梳理整个渲染管线，对最重的 pass 用 fetch_eid 和 get_texture 深入看看。"

## 测试

桥接的线程/协议/分发链路有桩测试覆盖（假 renderdoc 模块 + 假 PySide2）：

```powershell
# 占位桩测试在会话中以脚本形式运行过；快速自检：
python -m py_compile util\mcp-server\rd_mcp.py util\mcp-server\rd_mcp_bridge.py util\mcp-server\rd_frame_stats.py
python util\mcp-server\test_rd_frame_stats.py
# MCP 协议握手：
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | python util\mcp-server\rd_mcp.py
```

## 已知限制

- `get_texture` / `fetch_eid` 会切换 UI 当前事件（副作用：AI 会"带着你看"它关注的事件）。
- GPU 毫秒和 invocation 计数来自回放采样，不写进 capture。`pass_timings` / `frame_hotspots` 的 `refresh=true` 会占住 UI 线程直到 `FetchCounters` 结束；驱动不支持 `EventGPUDuration` 时没有毫秒，只有三角形估算和驱动返回的计数器。
- 桥接一次只处理一个请求（串行），UI 线程忙时请求排队。采样类工具默认等 180s，其余默认 60s。
- 扩展只在 qrenderdoc 运行时可用；无头（headless）批量分析请走 `renderdoc` python 模块的
  命令行接口，不经过本桥。
