# Local web server for the browser build (Windows, no Python needed).  Serves the folder given with -Dir (default: where this
# script is).  The game needs two special headers ("cross-origin isolation") that opening the file directly cannot give.
param([string]$Dir = $PSScriptRoot, [int]$Port = 8080)
$Dir = (Resolve-Path $Dir).Path
$types = @{ ".html" = "text/html"; ".js" = "text/javascript"; ".wasm" = "application/wasm"; ".css" = "text/css"; ".json" = "application/json"; ".png" = "image/png"; ".ico" = "image/x-icon" }
$l = New-Object System.Net.HttpListener
$l.Prefixes.Add("http://localhost:$Port/")
try { $l.Start() } catch { Write-Host "Could not start on port $Port ($($_.Exception.Message)). Try -Port 8081." -ForegroundColor Red; exit 1 }
Write-Host "Ridge Racer (browser build): open http://localhost:$Port/   (close this window to stop)" -ForegroundColor Green
Start-Process "http://localhost:$Port/"
while ($l.IsListening) {
  $c = $l.GetContext()
  try {
    $rel = [Uri]::UnescapeDataString($c.Request.Url.AbsolutePath.TrimStart('/'))
    if ($rel -eq "") { $rel = "index.html" }
    $path = [System.IO.Path]::GetFullPath((Join-Path $Dir $rel))
    $r = $c.Response
    $r.Headers.Add("Cross-Origin-Opener-Policy", "same-origin")
    $r.Headers.Add("Cross-Origin-Embedder-Policy", "require-corp")
    $r.Headers.Add("Cache-Control", "no-cache")
    if (-not $path.StartsWith($Dir) -or -not (Test-Path $path -PathType Leaf)) { $r.StatusCode = 404 }
    else {
      $ext = [System.IO.Path]::GetExtension($path).ToLower()
      $r.ContentType = if ($types.ContainsKey($ext)) { $types[$ext] } else { "application/octet-stream" }
      $fs = [System.IO.File]::OpenRead($path)
      $r.ContentLength64 = $fs.Length
      $fs.CopyTo($r.OutputStream, 1048576)
      $fs.Close()
    }
    $r.OutputStream.Close()
  } catch { try { $c.Response.Abort() } catch {} }
}
