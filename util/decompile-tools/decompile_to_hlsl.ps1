# decompile_to_hlsl.ps1
#
# RenderDoc fork extension: reverse-engineer readable HLSL from a captured D3D
# shader when no original source is available.
#
# It chains up to three external tools:
#
#   DXIL  --(dxil-spirv)----> SPIR-V --(spirv-cross --hlsl)--> HLSL
#   DXBC  --(dxbc2dxil)----> DXIL   --(dxil-spirv)---------> SPIR-V --(spirv-cross)--> HLSL
#
# Tool resolution order (each step):
#   1. environment variable override (RD_DXIL2SPIRV / RD_DXBC2DXIL / RD_SPIRV2HLSL)
#   2. <this script dir>/bin/<tool>
#   3. PATH
#
# Step command lines are also overridable via environment variables, so you can
# adapt to a particular build of dxil-spirv / dxbc2dxil without editing this file:
#   RD_DXIL2SPIRV_ARGS  (default "-o {output} {input}")
#   RD_DXBC2DXIL_ARGS   (default "/o {output} {input}")
#   RD_SPIRV2HLSL_ARGS  (default "--hlsl --output {output} {input}")
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File decompile_to_hlsl.ps1 <input> <output>
#
# Input:  raw DXBC or DXIL container bytes as written by RenderDoc
# Output: readable HLSL text

param(
  [Parameter(Mandatory = $true, Position = 0)][string]$InputFile,
  [Parameter(Mandatory = $true, Position = 1)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$binDir = Join-Path $scriptDir 'bin'

function Write-Log {
  param([string]$Msg)
  Write-Host "[decompile] $Msg"
}

# ---------------------------------------------------------------------------
# tool / arg resolution
# ---------------------------------------------------------------------------
function Resolve-Tool {
  param([string]$EnvName, [string]$FileName)

  $envVal = [Environment]::GetEnvironmentVariable($EnvName)
  if (-not [string]::IsNullOrWhiteSpace($envVal)) {
    if (Test-Path -LiteralPath $envVal) { return $envVal }
    $cmd = Get-Command $envVal -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
  }

  $local = Join-Path $binDir $FileName
  if (Test-Path -LiteralPath $local) { return $local }

  $cmd = Get-Command $FileName -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

  return $null
}

function Get-ArgTemplate {
  param([string]$EnvName, [string]$Default)
  $v = [Environment]::GetEnvironmentVariable($EnvName)
  if ([string]::IsNullOrWhiteSpace($v)) { return $Default }
  return $v
}

# ---------------------------------------------------------------------------
# run a single tool step; substitutes {input}/{output} and passes args as an array
# ---------------------------------------------------------------------------
function Invoke-Step {
  param([string]$Exe, [string]$ArgTemplate, [string]$StepIn, [string]$StepOut, [string]$What)

  if (-not $Exe) {
    throw "Missing tool for step '$What'. Run util\decompile-tools\setup.ps1 or see the README."
  }

  $argList = @()
  foreach ($t in ($ArgTemplate -split ' ')) {
    if ($t -eq '') { continue }
    $t = $t.Replace('{input}', $StepIn).Replace('{output}', $StepOut)
    $argList += $t
  }

  Write-Log "RUN  ${What}: $Exe $($argList -join ' ')"

  $output = & $Exe @argList 2>&1
  $code = $LASTEXITCODE

  foreach ($line in $output) { Write-Log "     $line" }

  if ($code -ne 0) {
    throw "Step '$What' failed with exit code $code."
  }

  if (-not (Test-Path -LiteralPath $StepOut) -or (Get-Item -LiteralPath $StepOut).Length -eq 0) {
    throw "Step '$What' produced no output file."
  }
}

# ---------------------------------------------------------------------------
# detect whether a DXBC container holds DXIL (vs classic DXBC bytecode)
# ---------------------------------------------------------------------------
function Test-FourCC {
  param([byte[]]$Bytes, [string]$FourCC)
  $needle = [System.Text.Encoding]::ASCII.GetBytes($FourCC)
  $last = $Bytes.Length - $needle.Length
  for ($i = 0; $i -le $last; $i++) {
    $match = $true
    for ($j = 0; $j -lt $needle.Length; $j++) {
      if ($Bytes[$i + $j] -ne $needle[$j]) { $match = $false; break }
    }
    if ($match) { return $true }
  }
  return $false
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $InputFile)) {
  throw "Input file '$InputFile' does not exist."
}

$bytes = [System.IO.File]::ReadAllBytes($InputFile)

$hasDxil = Test-FourCC $bytes 'DXIL'
$hasDxbc = (Test-FourCC $bytes 'SHDR') -or (Test-FourCC $bytes 'SHDA')

if (-not ($hasDxil -or $hasDxbc)) {
  throw "Input '$InputFile' is not a DXBC/DXIL container (no DXIL/SHDR part found)."
}

$dxil2spirv = Resolve-Tool 'RD_DXIL2SPIRV' 'dxil-spirv.exe'
$dxbc2dxil  = Resolve-Tool 'RD_DXBC2DXIL'  'dxbc2dxil.exe'
$spirv2hlsl = Resolve-Tool 'RD_SPIRV2HLSL' 'spirv-cross.exe'

$work = Join-Path $env:TEMP ("rd_decompile_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

try {
  if ($hasDxil) {
    $spv = Join-Path $work 'shader.spv'
    Invoke-Step $dxil2spirv (Get-ArgTemplate 'RD_DXIL2SPIRV_ARGS' '-o {output} {input}') `
                $InputFile $spv 'DXIL -> SPIR-V (dxil-spirv)'
  }
  else {
    $dxilOut = Join-Path $work 'shader.dxil'
    $spv = Join-Path $work 'shader.spv'
    Invoke-Step $dxbc2dxil (Get-ArgTemplate 'RD_DXBC2DXIL_ARGS' '/o {output} {input}') `
                $InputFile $dxilOut 'DXBC -> DXIL (dxbc2dxil)'
    Invoke-Step $dxil2spirv (Get-ArgTemplate 'RD_DXIL2SPIRV_ARGS' '-o {output} {input}') `
                $dxilOut $spv 'DXIL -> SPIR-V (dxil-spirv)'
  }

  Invoke-Step $spirv2hlsl (Get-ArgTemplate 'RD_SPIRV2HLSL_ARGS' '--hlsl --output {output} {input}') `
              $spv $OutputFile 'SPIR-V -> HLSL (spirv-cross)'

  Write-Log "OK: wrote HLSL to $OutputFile"
}
finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
