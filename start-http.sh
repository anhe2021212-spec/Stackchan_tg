#!/bin/bash
# Start Stack-chan MCP server in HTTP mode + cloudflared tunnel
# For Chat/Cowork access via https://stackchan.migratorybird.xyz
#
# Usage: ./start-http.sh        (start both)
#        ./start-http.sh stop   (stop both)

STACKCHAN_PORT=8002
MCP_PYTHON="/Users/Isa/Kokoro-TTS-Local/venv/bin/python"
MCP_SCRIPT="/Users/Isa/Projects/stackchan/mcp-server/server.py"

if [ "$1" = "stop" ]; then
    echo "Stopping..."
    kill $(lsof -ti:$STACKCHAN_PORT) 2>/dev/null
    pkill -f "cloudflared tunnel run" 2>/dev/null
    echo "Done."
    exit 0
fi

# Start MCP HTTP server
if lsof -ti:$STACKCHAN_PORT >/dev/null 2>&1; then
    echo "⚠️  Port $STACKCHAN_PORT already in use, killing..."
    kill $(lsof -ti:$STACKCHAN_PORT) 2>/dev/null
    sleep 1
fi

echo "🐋 Starting Stack-chan MCP HTTP server on port $STACKCHAN_PORT..."
nohup "$MCP_PYTHON" "$MCP_SCRIPT" --http --port $STACKCHAN_PORT > /tmp/stackchan_mcp_http.log 2>&1 &
echo "   PID=$!"

sleep 2

# Start cloudflared (if not already running)
if pgrep -f "cloudflared tunnel run" >/dev/null 2>&1; then
    echo "☁️  cloudflared already running"
else
    echo "☁️  Starting cloudflared tunnel..."
    nohup cloudflared tunnel run > /tmp/cloudflared.log 2>&1 &
    echo "   PID=$!"
fi

sleep 3

# Verify
echo ""
echo "=== Status ==="
if curl -s --max-time 5 "https://stackchan.migratorybird.xyz/mcp" -X POST \
    -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
    -d '{"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"0.1"}}}' 2>&1 | grep -q "serverInfo"; then
    echo "✅ https://stackchan.migratorybird.xyz → Streamable HTTP OK"
else
    echo "❌ Tunnel not responding yet (may need a few more seconds)"
fi
echo ""
echo "Claude.ai MCP config:"
echo "  URL: https://stackchan.migratorybird.xyz/mcp"
