"""qrenderdoc loads the extensions folder as a package and calls register()."""

from .rd_mcp_bridge import register, unregister
