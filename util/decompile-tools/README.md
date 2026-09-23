# Decompile to HLSL

RenderDoc fork extension that reverse-engineers **readable HLSL** from a captured
D3D shader when no original source is available (very common with Unity).

It works as an extra **Shader Processing Tool** so it shows up automatically in:

- Pipeline State → shader **Edit** menu → "Decompile with …"
- Shader Viewer → **Disassembly** dropdown

## How it works

The captured shader bytecode is converted through a chain of external tools:

```
DXIL  ──(dxil-spirv)────> SPIR-V ──(spirv-cross --hlsl)──> HLSL
DXBC  ──(dxbc2dxil)────> DXIL   ──(dxil-spirv)─────────> SPIR-V ──(spirv-cross)──> HLSL
```

- `spirv-cross` (Khronos): SPIR-V → HLSL. Also the tool RenderDoc ships for
  Vulkan SPIR-V decompilation.
- `dxil-spirv` (from the Microsoft DXC tree): DXIL → SPIR-V. Required for D3D12.
- `dxbc2dxil` (from the Microsoft DXC tree): DXBC → DXIL. Only needed for D3D11.

> Quality note: DXBC → DXIL → SPIR-V → HLSL loses some fidelity. D3D12 (DXIL)
> output is generally much more readable than D3D11 (DXBC).

## Files

| File | Purpose |
|------|---------|
| `decompile_to_hlsl.ps1` | Chain wrapper invoked by RenderDoc as the "tool" |
| `setup.ps1` | One-shot download/setup of the external tools into `bin\` |
| `bin\` | Where the external `.exe` tools are installed (created by setup, git-ignored) |

## Setup

```powershell
cd util\decompile-tools
powershell -ExecutionPolicy Bypass -File setup.ps1
```

`setup.ps1` downloads RenderDoc's official SPIR-V plugin package and installs
`spirv-cross.exe` (the HLSL generator) plus `spirv-as.exe`, `spirv-dis.exe` and
`glslangValidator.exe` into `bin\`.

`dxil-spirv.exe` and `dxbc2dxil.exe` are **not** in any official prebuilt release
archive, so pass URLs if you have them, otherwise build them from the DXC source
tree:

```powershell
git clone https://github.com/microsoft/DirectXShaderCompiler.git
cd DirectXShaderCompiler
# dxil-spirv:  tools/clang/tools/dxil-spirv
# dxbc2dxil:   projects/dxilconv/tools/dxbc2dxil
# (build with CMake; see docs/SPIR-V.rst)
```

Copy the resulting `dxil-spirv.exe` / `dxbc2dxil.exe` (plus `dxcompiler.dll` /
`dxil.dll` if required) into `util\decompile-tools\bin\`.

## Alternative: manual per-tool configuration

If you installed the tools somewhere else, you don't need to edit any code —
set environment variables before starting RenderDoc:

| Var | Default |
|-----|---------|
| `RD_DXIL2SPIRV` | `dxil-spirv.exe` (searched in `bin\` then PATH) |
| `RD_DXBC2DXIL` | `dxbc2dxil.exe` |
| `RD_SPIRV2HLSL` | `spirv-cross.exe` |
| `RD_DXIL2SPIRV_ARGS` | `-o {output} {input}` |
| `RD_DXBC2DXIL_ARGS` | `/o {output} {input}` |
| `RD_SPIRV2HLSL_ARGS` | `--hlsl --output {output} {input}` |

You can also register the wrapper manually under
**Tools → Settings → Shader Processors → Add** (Tool Type = Custom Tool):

- Name: `Decompile to HLSL`
- Executable: `powershell`
- Command line:
  `-NoProfile -ExecutionPolicy Bypass -File "<abs path to decompile_to_hlsl.ps1>" {input_file} {output_file}`
- Input: `DXIL` (add a second entry with `DXBC` for D3D11), Output: `HLSL`

## How RenderDoc finds the wrapper

`PersistantConfig::Load` auto-registers the tool when it can locate
`decompile_to_hlsl.ps1`. Search order:

1. `RENDERDOC_DECOMPILE_SCRIPT` environment variable
2. `<qrenderdoc.exe dir>\decompile-tools\decompile_to_hlsl.ps1`
3. `<qrenderdoc.exe dir>\decompile_to_hlsl.ps1`
4. `<qrenderdoc.exe dir>\..\..\util\decompile-tools\decompile_to_hlsl.ps1` (source build layout)
