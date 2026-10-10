# RenderDoc · Pipeline Agent 分支

本仓库在 [RenderDoc](https://github.com/baldurk/renderdoc) 上游基础上，集成了 **Pipeline Agent**：在图形调试器内直接连接大模型，针对**当前已加载的 Capture** 提问，辅助阅读事件树、管线状态、Shader 反汇编，并支持让模型通过内置指令拉取指定 EID 的数据以梳理渲染管线。

> **默认主题 Grove Glass**：对齐 [TAWEBTOOL](https://github.com/) 设计语言的明亮玻璃主题（白磨砂卡片 + 翡翠 `#26a884` 强调 + 楷体风格文字）。所用 **RD WenKai** 字体（LXGW WenKai Screen 的 GB2312 常用字子集，仅 3.5MB，SIL OFL 1.1，见 `qrenderdoc/Resources/fonts/`）已内嵌进程序，无需系统安装；罕见字自动回退系统楷体/雅黑。

> **说明**：以下为该 Agent 功能的用法说明。完整 RenderDoc 通用文档仍见上游 [renderdoc.org/docs](https://renderdoc.org/docs/) 与仓库内 [`docs/`](docs/)。

---

## Pipeline Agent 能做什么

- 在 **Tools → Pipeline Agent**（或主工具区对应页签）中打开面板。
- 选择 **Provider**（如 OpenAI、Anthropic、Gemini、Azure、OpenAI 兼容 URL、OpenRouter、智谱 GLM、GitHub Models、DeepSeek、阿里云百炼 Qwen、月之暗面 Kimi、百度千帆 ERNIE、火山引擎豆包 Doubao 等），填写 **API Token**；**Model** 请从下拉项中选择各平台当前可用模型。
- 可选勾选 **Include shader disassembly**，在上下文中附带当前绑定阶段的反汇编（会走回放线程，可能略慢）。
- 可选勾选 **Attach texture thumbnails**（默认开启），把当前事件的渲染目标与深度缓冲截图（最长边 512，PNG）以**多模态图片**发给模型，OpenAI / Anthropic / Gemini 三协议均支持，让模型"看见"画面再分析。
- **Shader Viewer → Disassembly** 工具栏的 **Send to Agent** 按钮：一键把当前反汇编（含你通过 Shader Processing Tool 反编译出的 HLSL）送进 Pipeline Agent 提问。
- **Export report** 按钮：一键导出 Markdown 拆解报告（事件树、逐 pass 快照、当前事件管线状态、渲染目标/深度截图、最近的 AI 结论），适合产出 graphics-study 风格的渲染分析文章。
- 在底部输入框用自然语言提问（例如：梳理本帧渲染管线、某 Pass 在做什么）。面板通过**原生 function calling** 向模型暴露 `list_events` / `scan_pass` / `fetch_eid` 三个工具，模型按需调用，本地自动取数并以结构化 tool result 多轮回填（对话上下文在工具循环内完整保留）。对不支持 tools 的 Provider/模型会自动降级为 **`[FETCH_EID nnnn]`** / **`[SCAN_PASS nnnn]`** / **`[LIST_EVENTS]`** 文本协议。相邻几轮问答（仅问题与结论，不含大快照）会作为轻量历史带上，支持追问。
- 对话区支持滚动历史；回复里的 **`EID xxxx`** 可点击跳转到对应事件。
- **Send** 发送，**Stop** 中断长请求或多轮工具循环；**Ctrl+F** 在聊天中搜索；界面会显示 **Token 用量** 与当前**状态**（避免长时间无反馈）。
- **Pipeline snapshot** 可展开查看当前上下文字符串；**Copy context / Copy content** 便于复制到剪贴板。

API Key 会写入本机 RenderDoc 配置文件（**未加密**），请使用权限尽量小的密钥。

---

## 界面一览

<p align="center">
  <img src="docs/images/pipeline_agent.png" alt="Pipeline Agent 面板" width="920" />
</p>

上图为典型布局：**上方**为 Provider / Token / Model 与选项；**中部**可折叠 Pipeline 快照与对话区（含状态与 Token 统计）；**底部**为输入与发送、停止等按钮。

---

## 使用案例

**场景**：已加载一帧 Capture，左侧 **Event Browser** 可见 Copy/Clear、Colour、多段 **Depth-only Pass** 等；时间线展示 Frame 内各阶段。向 Agent 提问（例如「帮我解析下渲染管线」），模型会先拉取事件树、再扫描主要 Pass，最后给出结构化总结（含 EID、资源与 Token 统计）。

**界面示意**（OpenRouter + Claude、对话区含管线总览与 ASCII 数据流图；本例 Token：**88470 prompt + 4728 completion = 93198 total**）：

<p align="center">
  <img src="docs/images/pipeline_agent_usage_case.png" alt="Pipeline Agent 使用案例：解析渲染管线" width="920" />
</p>

**示例：Agent 回复全文（与上图同一次对话；`<function_calls>` 内为面板代填的工具调用占位）**

<details>
<summary>点击展开</summary>

````text
Agent:
I'll analyze the rendering pipeline for you. Let me start by getting the full event tree to understand the structure.
<function_calls>

</function_calls>

Now let me scan the major render passes to understand what each one does:
<function_calls>




</function_calls>

Based on the comprehensive event tree analysis, here's a breakdown of your rendering pipeline:

## **渲染管线结构总览**

This is a **modern AAA game rendering pipeline** (likely using deferred rendering with multiple depth passes). The frame consists of **1099 draws, 19 dispatches, 22 clears, 45 copies**.

---

## **主要渲染阶段**

### **1️⃣ 初始化与计算 (EID 8836-206)**
- **EID 8836**: Copy/Clear Pass #1 - 初始化缓冲区
- **EID 195**: Dispatch 14x1x1 - 计算处理
- **EID 206**: Dispatch 8x8x4 - 计算处理

---

### **2️⃣ 颜色预处理 (EID 8837-8838)**
- **EID 8837 - Colour Pass #1** (2 Targets)
- 3 draws with 48/48/48 indices
- 输出: ResourceId::276032, ResourceId::276033 (256x256 R32G32B32A32_FLOAT)

- **EID 8838 - Colour Pass #2** (2 Targets)
- 3 draws, 1 clear
- 输出: ResourceId::276034, ResourceId::276035 (256x256 R32G32B32A32_FLOAT)

**目的**: 预计算辅助纹理（可能是光照探针或某种LUT）

---

### **3️⃣ 深度预处理 (EID 8839-8846) - 8个Depth-only Pass**

这是流程的核心，使用**8个连续的深度通道**来处理不同的几何体或LOD级别：

| Pass | EID | Draws | 特点 |
|------|-----|-------|------|
| **Depth Pass #1** | 8839 | 42 | 主要几何体（索引 79728-6105） |
| **Depth Pass #2** | 8840 | 42 | 中等复杂度（索引 2973-864） |
| **Depth Pass #3** | 8841 | 42 | 复杂实例化（6144x96, 6144x24 inst） |
| **Depth Pass #4** | 8842 | 42 | 小物体与植被（114-123 idx） |
| **Depth Pass #5** | 8843 | 42 | 细节几何（84-573 idx） |
| **Depth Pass #6** | 8844 | 42 | 小细节（24 idx） |
| **Depth Pass #7** | 8845 | 42 | 更小几何（1371-1806 idx） |
| **Depth Pass #8** | 8846 | 40 | 最细小物体（6-186 idx） |

**深度目标**: ResourceId::27088 (D32S8, 2560x1440)

**用途**：
- 准备深度金字塔 (Depth Pyramid / Hi-Z)
- 遮挡剔除准备 (Occlusion Culling)
- Shadow mapping 或其他深度相关效果

---

### **4️⃣ 计算与后处理 (EID 1789-1938)**

- **EID 1789**: Dispatch 1840x1x1
- **EID 1798**: Dispatch 10x6x8
- **EID 8848 - Compute Pass #1**: 3 dispatches + 34 copies
- **256x128x1**, **16x8x1**, **1x1x1** 三个计算着色器
- 大量拷贝操作（可能是数据readback或转码）

- **EID 8850-8851**: 多个深度通道与计算处理

---

### **5️⃣ 最终颜色渲染 (EID 8855-8862) - 4个Colour Pass**

这些是**最终的可见性渲染通道**：

| Pass | EID | Targets | Draws | 用途 |
|------|-----|---------|-------|------|
| **Colour Pass #3** | 8855 | 3 RT + Depth | 13 | 初始颜色渲染 |
| **Colour Pass #4** | 8857 | 7 RT + Depth | 50 | 完整GBuffer或中间处理 |
| **Colour Pass #5** | 8858 | 7 RT + Depth | 50 | 继续GBuffer或光照处理 |
| **Colour Pass #6** | 8859 | 7 RT + Depth | 50 | 更多细节渲染 |
| **Colour Pass #7** | 8860 | 7 RT + Depth | 50 | 继续处理 |
| **Colour Pass #8** | 8862 | 7 RT + Depth | 50 | 最后的着色 |

**渲染目标**:
- ResourceId::281117, 281127, 281135 (颜色/法线/材质信息)
- ResourceId::281139, 281078, 281084, 281086, 281051, 281088, 281089 (7个RT - 完整GBuffer)
- 深度: ResourceId::27088 (D32S8)

---

### **6️⃣ 最终输出**
- **EID 8835**: Present(ResourceId::405)
- 输出到交换链 (2560x1440, R10G10B10A2_UNORM)

---

## **管线数据流总结**

（ASCII 示意）

┌─────────────────────────────────────┐
│ 初始化 & 计算 (Copy/Dispatch) │
└────────────┬────────────────────────┘
             │
┌────────────▼────────────────────────┐
│ 预处理颜色 Pass (2 Targets) │
│ 256x256 FLOAT16 颜色LUT │
└────────────┬────────────────────────┘
             │
┌────────────▼────────────────────────┐
│ 8 x Depth-Only Pass │
│ 构建深度金字塔 / Hi-Z │
│ (准备遮挡剔除和深度信息) │
└────────────┬────────────────────────┘
             │
┌────────────▼────────────────────────┐
│ Compute Pass (降低采样/编码) │
│ Dispatch + 34 Copy操作 │
└────────────┬────────────────────────┘
             │
┌────────────▼────────────────────────┐
│ 6 x Colour Pass (GBuffer Build) │
│ 7个渲染目标 + 深度 │
│ (颜色 + 法线 + 材质 + 辅助信息) │
│ 2560x1440, 总计 300+ draws │
└────────────┬────────────────────────┘
             │
┌────────────▼────────────────────────┐
│ Present 到交换链 │
│ 输出: 2560x1440 R10G10B10A2 │
└─────────────────────────────────────┘

---

## **关键特点**

✅ **Deferred Rendering**: 使用多个 GBuffer 目标 
✅ **深度预处理**: 8 个连续深度通道 → Hi-Z 金字塔 
✅ **实例化渲染**: 大量使用实例化(inst count 最高 9601) 
✅ **计算着色器**: 用于降采样、编码或后处理 
✅ **高分辨率**: 2560x1440 主渲染分辨率 
✅ **复杂光照**: 7个GBuffer目标表明复杂的PBR或特效系统
````

</details>

---

## Decompile to HLSL（D3D 着色器反编译）

本分支在 RenderDoc 内集成了 **DXBC/DXIL → 可读 HLSL** 工具链，适用于 **Unity 等未保留原始 shader 源码** 的场景。安装外部工具后，在 **Pipeline State → Edit** 或 **Shader Viewer → Disassembly** 中即可选择 **Decompile to HLSL**。

| 步骤 | 说明 |
|------|------|
| 编译本 fork | 见下方 [构建与运行](#构建与运行) |
| 安装反编译工具 | `util\decompile-tools\setup.ps1`（需额外构建 `dxil-spirv`） |
| 在 Capture 中反编译 | 打开 Shader Viewer → Disassembly → **Decompile to HLSL (DXIL)** |

完整 **Setup / Usage / Troubleshooting** 见 [`util/decompile-tools/README.md`](util/decompile-tools/README.md)。

---

## MCP Server（外部 AI 客户端接入）

通过 **MCP（Model Context Protocol）** 把运行中的 qrenderdoc 暴露给 ZCode / Claude Code 等
AI 客户端：`capture_info` / `list_events` / `fetch_eid` / `get_texture` / `run_python` 五个工具，
让 AI 直接分析抓到的帧、保存渲染目标截图、执行任意 python。

```powershell
# 一键安装 qrenderdoc 内的桥接扩展，再到 Tools → Manage Extensions 启用
powershell -ExecutionPolicy Bypass -File util\mcp-server\install.ps1
```

架构与客户端配置见 [`util/mcp-server/README.md`](util/mcp-server/README.md)。

---

## Android（GLES / Vulkan）捕获

Pipeline Agent 与 API 无关，**Android 设备抓的帧开箱即用**：上游 RenderDoc 自带 adb 连接
Android 设备回放（Capture → Launch Application → Remote Android Device），抓到 GLES 或
Vulkan 帧后，Pipeline Agent 的事件树 / pass 扫描 / 管线快照 / 纹理缩略图全部正常工作。

Shader 还原路径按 API 自动区分：

| 捕获 API | 反编译路径 |
|----------|-----------|
| Android Vulkan | 内置 **spirv-cross** 直接 SPIR-V → HLSL/GLSL（无需额外安装） |
| D3D12 (DXIL) | `util\decompile-tools`（dxil-spirv → spirv-cross） |
| D3D11 (DXBC) | `util\decompile-tools`（dxbc2dxil → dxil-spirv → spirv-cross，保真度略低） |

**一键全帧解析**：Pipeline Agent 面板的 **Analyze frame** 按钮会把事件树、每个顶层 pass 的
采样扫描、shader 反汇编和纹理缩略图一次性打包发给模型，产出整帧管线的完整拆解——对
Android 游戏的卡通渲染/后处理链逆向尤其方便。

---

## 构建与运行

从源码编译方式与上游一致，请参阅 [`docs/CONTRIBUTING/Compiling.md`](docs/CONTRIBUTING/Compiling.md)。Windows 下通常使用 Visual Studio 打开生成目录中的解决方案并按说明配置 Qt 等依赖。

---

## 许可与上游

RenderDoc 本体以 **MIT** 许可发布，见 [`LICENSE.md`](LICENSE.md)。本分支的修改在相同许可前提下分发；使用时请遵守 RenderDoc 官方关于**仅调试自有程序**等政策说明。
