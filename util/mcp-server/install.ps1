# Installs the rd_mcp_bridge.py extension into qrenderdoc's extensions directory
# (%APPDATA%/qrenderdoc/extensions/renderdoc_mcp/). After installing, open
# qrenderdoc -> Tools -> Manage Extensions and enable it, then see
# util/mcp-server/README.md to connect an AI client.

$ErrorActionPreference = "Stop"

$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$dst = Join-Path $env:APPDATA "qrenderdoc\extensions\renderdoc_mcp"

New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item (Join-Path $src "rd_mcp_bridge.py") $dst -Force
Copy-Item (Join-Path $src "extension.json") $dst -Force

Write-Host "Installed bridge to $dst"
Write-Host "Next: open qrenderdoc -> Tools -> Manage Extensions, and enable 'RenderDoc MCP Bridge'."
Write-Host "Then configure your AI client to run: python `"$src\rd_mcp.py`""
