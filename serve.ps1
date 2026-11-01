# Othello - tiny local web server for docs/ (no Python needed)
# Usage: powershell -ExecutionPolicy Bypass -File serve.ps1 [-Kiosk]   (-Kiosk = fullscreen mode, Esc to exit fullscreen)
param([switch]$Kiosk)

$ErrorActionPreference = 'Stop'
$base = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Join-Path $base 'docs'
$logFile = Join-Path $base 'server_log.txt'
function Log($m) { $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m; Write-Host $line; Add-Content -Path $logFile -Value $line -Encoding UTF8 }

$mime = @{
  '.html' = 'text/html; charset=utf-8'; '.js' = 'text/javascript; charset=utf-8'; '.mjs' = 'text/javascript; charset=utf-8'
  '.wasm' = 'application/wasm'; '.task' = 'application/octet-stream'; '.css' = 'text/css'; '.json' = 'application/json'
  '.png' = 'image/png'; '.svg' = 'image/svg+xml'; '.csv' = 'text/csv; charset=utf-8'; '.ico' = 'image/x-icon'; '.md' = 'text/plain; charset=utf-8'
}

# find a free port
$listener = $null
foreach ($p in 8801..8809) {
  try {
    $l = New-Object System.Net.HttpListener
    $l.Prefixes.Add("http://localhost:$p/")
    $l.Start(); $listener = $l; $port = $p; break
  } catch { }
}
if (-not $listener) { Log 'ERROR: no free port in 8801-8809'; Read-Host 'Press Enter to exit'; exit 1 }
$url = "http://localhost:$port/index.html"
Log "Serving $root at $url"

# open Chrome (or Edge, or default browser)
$chromes = @(
  "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
  "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
  "$env:LocalAppData\Google\Chrome\Application\chrome.exe",
  "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
  "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
)
$browser = $chromes | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
$flags = @('--autoplay-policy=no-user-gesture-required')
if ($Kiosk) {
  # app window without address bar; the page goes fullscreen on the first click/key (Esc exits fullscreen)
  $url = "$url`?fs=1"
  $flags += @("--app=$url", '--start-maximized')
} else { $flags += @('--new-window', $url) }
if ($browser) { Start-Process -FilePath $browser -ArgumentList $flags; Log "Browser: $browser" }
else { Start-Process $url; Log 'Chrome/Edge not found. Opened default browser (Chrome or Edge recommended).' }

Log 'Close this window to stop the server.'
try {
  while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    $req = $ctx.Request; $res = $ctx.Response
    try {
      $rel = [Uri]::UnescapeDataString($req.Url.AbsolutePath).TrimStart('/')
      if ($rel -eq '') { $rel = 'index.html' }
      $full = [IO.Path]::GetFullPath((Join-Path $root $rel))
      if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path $full -PathType Leaf)) {
        $res.StatusCode = 404; $b = [Text.Encoding]::UTF8.GetBytes('404'); $res.OutputStream.Write($b, 0, $b.Length)
        if ($rel -ne 'favicon.ico') { Log "404 $rel" }
      } else {
        $ext = [IO.Path]::GetExtension($full).ToLower()
        $res.ContentType = if ($mime.ContainsKey($ext)) { $mime[$ext] } else { 'application/octet-stream' }
        $res.Headers.Add('Cache-Control', 'no-cache')
        $bytes = [IO.File]::ReadAllBytes($full)
        $res.ContentLength64 = $bytes.Length
        $res.OutputStream.Write($bytes, 0, $bytes.Length)
      }
    } catch { Log "ERROR $($_.Exception.Message)" }
    finally { try { $res.OutputStream.Close() } catch { } }
  }
} finally { $listener.Stop() }
