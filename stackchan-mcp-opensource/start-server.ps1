# 启动 stackchan-mcp server（stdio 模式给 CC 用）
# 用法: powershell -ExecutionPolicy Bypass -File .\start-server.ps1

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Refresh PATH from registry so newly-installed tools (ffmpeg via winget) are visible
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")

# 读 .env 注入环境变量
$envFile = Join-Path $here ".env"
if (Test-Path $envFile) {
    Get-Content $envFile | ForEach-Object {
        $line = $_.Trim()
        if ($line -and -not $line.StartsWith("#") -and $line.Contains("=")) {
            $parts = $line.Split("=", 2)
            $key = $parts[0].Trim()
            $val = $parts[1].Trim()
            Set-Item -Path "env:$key" -Value $val
        }
    }
}

python (Join-Path $here "mcp-server\server.py")
