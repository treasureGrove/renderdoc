# setup.ps1
#
# One-shot setup for the "Decompile to HLSL" tool chain used by RenderDoc's
# shader edit / disassembly menus.
#
# What it does:
#   1. Creates <repo>\util\decompile-tools\bin\
#   2. Downloads SPIRV-Cross (spirv-cross.exe) - the HLSL generator
#   3. Tries to fetch dxil-spirv.exe (DXIL->SPIR-V) and dxbc2dxil.exe (DXBC->DXIL)
#      from optional URLs, otherwise prints build instructions
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File setup.ps1
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -DxilSpirvUrl "https://..." -Dxbc2DxilUrl "https://..."
#
# After setup, RenderDoc picks the tools up automatically (the wrapper script is
# auto-registered when it is found). If you installed the .exe files somewhere
# else, set these env vars instead:
#   RD_DXIL2SPIRV, RD_DXBC2DXIL, RD_SPIRV2HLSL
#   RD_DXIL2SPIRV_ARGS, RD_DXBC2DXIL_ARGS, RD_SPIRV2HLSL_ARGS

param(
  [string]$SpirvCrossZipUrl = 'https://renderdoc.org/plugins.zip',
  [string]$DxilSpirvUrl = '',
  [string]$Dxbc2DxilUrl = ''
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$binDir = Join-Path $scriptDir 'bin'
$tmpDir = Join-Path $env:TEMP ('rd_decompile_setup_' + [guid]::NewGuid().ToString('N'))

New-Item -ItemType Directory -Path $binDir -Force | Out-Null
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null

function Write-Step {
  param([string]$Msg)
  Write-Host "`n=== $Msg ==="
}

function Download-File {
  param([string]$Url, [string]$Dest)
  Write-Host "Downloading $Url"
  $ProgressPreference = 'SilentlyContinue'
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
  Write-Host "  -> $Dest ($((Get-Item -LiteralPath $Dest).Length) bytes)"
}

try {
  # -------------------------------------------------------------------------
  # 1. SPIR-V tools from RenderDoc's official plugin package
  #    (spirv-cross for HLSL generation, plus spirv-as / spirv-dis / glslang)
  # -------------------------------------------------------------------------
  Write-Step 'SPIR-V tools (spirv-cross / spirv-as / spirv-dis / glslangValidator)'
  $sc = Join-Path $binDir 'spirv-cross.exe'
  if (Test-Path -LiteralPath $sc) {
    Write-Host '  spirv-cross.exe already present, skipping download.'
  }
  else {
    $zip = Join-Path $tmpDir 'plugins.zip'
    Download-File $SpirvCrossZipUrl $zip
    Expand-Archive -LiteralPath $zip -DestinationPath $tmpDir -Force

    # install every SPIR-V tool that ships in the plugin package
    $found = Get-ChildItem -Path $tmpDir -Recurse -Include 'spirv-cross.exe','spirv-as.exe','spirv-dis.exe','glslangValidator.exe' -File
    foreach ($f in $found) {
      Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $binDir $f.Name) -Force
      Write-Host "  installed: $($f.Name)"
    }

    if (-not $found) {
      Write-Host '  WARNING: could not find any SPIR-V tools inside the downloaded archive.'
      Write-Host "  The archive was extracted to: $tmpDir - copy the tools to $binDir manually."
    }
  }

  # -------------------------------------------------------------------------
  # 2. dxil-spirv.exe (DXIL -> SPIR-V)
  # -------------------------------------------------------------------------
  Write-Step 'dxil-spirv (DXIL -> SPIR-V)'
  $ds = Join-Path $binDir 'dxil-spirv.exe'
  if (Test-Path -LiteralPath $ds) {
    Write-Host '  already present, skipping.'
  }
  elseif ($DxilSpirvUrl) {
    Download-File $DxilSpirvUrl $ds
  }
  else {
    Write-Host @'
  No URL provided (-DxilSpirvUrl). dxil-spirv is built from the DXC source tree:

      git clone https://github.com/microsoft/DirectXShaderCompiler.git
      cd DirectXShaderCompiler
      # build tools/clang/tools/dxil-spirv (see docs/SPIR-V.rst / CMake)

  Then copy the resulting dxil-spirv.exe (and dxcompiler.dll / dxil.dll next to
  it) into: $binDir
'@
  }

  # -------------------------------------------------------------------------
  # 3. dxbc2dxil.exe (DXBC -> DXIL, optional, only needed for D3D11 shaders)
  # -------------------------------------------------------------------------
  Write-Step 'dxbc2dxil (DXBC -> DXIL, optional)'
  $bc = Join-Path $binDir 'dxbc2dxil.exe'
  if (Test-Path -LiteralPath $bc) {
    Write-Host '  already present, skipping.'
  }
  elseif ($Dxbc2DxilUrl) {
    Download-File $Dxbc2DxilUrl $bc
  }
  else {
    Write-Host @'
  No URL provided (-Dxbc2DxilUrl). dxbc2dxil is built from the DXC source tree
  (projects/dxilconv). Build it with CMake from the DXC checkout and copy
  dxbc2dxil.exe into: $binDir
'@
  }

  # -------------------------------------------------------------------------
  # summary
  # -------------------------------------------------------------------------
  Write-Step 'Summary'
  foreach ($n in @('spirv-cross.exe', 'spirv-as.exe', 'spirv-dis.exe',
                   'glslangValidator.exe', 'dxil-spirv.exe', 'dxbc2dxil.exe')) {
    $p = Join-Path $binDir $n
    if (Test-Path -LiteralPath $p) { Write-Host "  [OK]   $n" }
    else { Write-Host "  [MISS] $n" }
  }
  Write-Host ''
  Write-Host 'Done. Restart RenderDoc - the shader Edit menu / ShaderViewer'
  Write-Host 'Disassembly dropdown will then offer "Decompile to HLSL".'
  Write-Host ''
  Write-Host 'Missing DXIL/DXBC tools? See util/decompile-tools/README.md for'
  Write-Host 'build instructions, or override per-tool exe/args via env vars.'
}
finally {
  Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
}
