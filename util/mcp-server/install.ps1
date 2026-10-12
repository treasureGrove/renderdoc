# Installs the rd_mcp_bridge.py extension into qrenderdoc's extensions directory
# (%APPDATA%/qrenderdoc/extensions/renderdoc_mcp/). After installing, open
# qrenderdoc -> Tools -> Manage Extensions and enable it, then see
# util/mcp-server/README.md to connect an AI client.

$ErrorActionPreference = "Stop"

$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$dst = Join-Path $env:APPDATA "qrenderdoc\extensions\renderdoc_mcp"

New-Item -ItemType Directory -Force -Path $dst | Out-Null
foreach ($name in @("__init__.py", "rd_mcp_bridge.py", "rd_frame_stats.py", "extension.json")) {
    Copy-Item (Join-Path $src $name) $dst -Force
}

Write-Host "Installed bridge to $dst"
Write-Host "Next: open qrenderdoc -> Tools -> Manage Extensions, and enable 'RenderDoc MCP Bridge'."
Write-Host "Reload the extension if it was already enabled. Tools -> Frame Hotspots opens the cost report."
Write-Host "Then configure your AI client to run: python `"$src\rd_mcp.py`""
