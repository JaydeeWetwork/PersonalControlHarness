# Runs every LithoKraft bench back to back and collects the JSON results into one summary.
# Windowed: each run opens the game window for -Seconds (+3 s warm-up). Run it from the repo root
# after a Release build (tools\build.ps1). Results and per-run logs land in -Out (default
# bench-results\<timestamp>); nothing is deleted.
#   powershell -ExecutionPolicy Bypass -File tools\run-bench-suite.ps1 [-Seconds 30] [-SoakMinutes 10] [-SkipSoak]
#     [-DisplayModes [-Monitor 0] [-Resolution 1920x1080] [-Refresh 144]]
#   -ArenaAB: Vulkan --vk-arena host|device|rebar and GL --gl-arena off|on, orbit + stream, fullscreen.
#   -DisplayModes adds Vulkan + GL orbit runs in windowed, maximized, borderless and exclusive
#   fullscreen (-Resolution / -Refresh pick the fullscreen video mode; default: desktop mode).
param(
  [int]$Seconds = 30,
  [int]$SoakMinutes = 10,
  [switch]$SkipSoak,
  [switch]$DisplayModes,
  [switch]$ArenaAB,
  [int]$Monitor = 0,
  [string]$Resolution = "",
  [int]$Refresh = 0,
  [string]$Exe = "build\Release\lithokraft.exe",
  [string]$Out = ""
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Exe)) { throw "missing $Exe (build Release first: tools\build.ps1)" }
if (-not $Out) { $Out = Join-Path "bench-results" (Get-Date -Format "yyyy-MM-dd_HHmmss") }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$runs = @(
  @{ name = "vulkan-orbit-mailbox";   args = @("--renderer", "vulkan", "--present", "mailbox") },
  @{ name = "vulkan-orbit-immediate"; args = @("--renderer", "vulkan", "--present", "immediate") },
  @{ name = "vulkan-orbit-fifo";      args = @("--renderer", "vulkan", "--present", "fifo") },
  @{ name = "gl33-orbit";             args = @("--renderer", "gl33") },
  @{ name = "vulkan-stream";          args = @("--renderer", "vulkan", "--bench-path", "stream") },
  @{ name = "gl33-stream";            args = @("--renderer", "gl33", "--bench-path", "stream") }
)
foreach ($tps in 32, 64, 128, 256, 512) {
  $runs += @{ name = "vulkan-circuits-$tps"; args = @("--renderer", "vulkan", "--bench-scene", "circuits", "--tps", "$tps") }
}
if ($DisplayModes) {
  foreach ($mode in "windowed", "maximized", "borderless", "fullscreen") {
    $extra = @("--display", $mode, "--monitor", "$Monitor")
    if ($mode -eq "fullscreen" -and $Resolution) { $extra += @("--resolution", $Resolution) }
    if ($mode -eq "fullscreen" -and $Refresh -gt 0) { $extra += @("--refresh", "$Refresh") }
    $runs += @{ name = "vulkan-orbit-$mode"; args = @("--renderer", "vulkan") + $extra }
    $runs += @{ name = "gl33-orbit-$mode";   args = @("--renderer", "gl33") + $extra }
  }
}
if ($ArenaAB) {  # vertex-memory A/B: orbit + stream, exclusive fullscreen, both renderers
  foreach ($path in "orbit", "stream") {
    foreach ($a in "host", "device", "rebar") {
      $runs += @{ name = "vulkan-$path-fullscreen-arena-$a"; args = @("--renderer", "vulkan", "--vk-arena", $a, "--display", "fullscreen", "--monitor", "$Monitor", "--bench-path", $path) }
    }
    foreach ($a in "off", "on") {
      $runs += @{ name = "gl33-$path-fullscreen-arena-$a"; args = @("--renderer", "gl33", "--gl-arena", $a, "--display", "fullscreen", "--monitor", "$Monitor", "--bench-path", $path) }
    }
  }
}

$results = @()
foreach ($r in $runs) {
  $json = Join-Path $Out "$($r.name).json"
  $log = Join-Path $Out "$($r.name).log"
  Write-Host "== $($r.name)"
  & $Exe @($r.args + @("--bench", "$Seconds", "--bench-json", $json, "--log", $log)) | Write-Host
  if (Test-Path $json) { $j = Get-Content $json -Raw | ConvertFrom-Json; $j | Add-Member name $r.name; $results += $j }
  else { Write-Warning "$($r.name): no result (see $log)" }
}
if (-not $SkipSoak) {
  $json = Join-Path $Out "vulkan-soak.json"; $log = Join-Path $Out "vulkan-soak.log"
  Write-Host "== vulkan-soak ($SoakMinutes min)"
  & $Exe --renderer vulkan --soak $SoakMinutes --bench-json $json --log $log | Write-Host
  if (Test-Path $json) { $j = Get-Content $json -Raw | ConvertFrom-Json; $j | Add-Member name "vulkan-soak"; $results += $j }
}
Write-Host "== headless (thread scaling, save / load, circuits)"
& $Exe --perf-headless (Join-Path $Out "headless.json") | Tee-Object -FilePath (Join-Path $Out "headless.txt") | Write-Host

$results | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Out "summary.json")
$md = @("| run | present | display | frames | avg ms | p99 ms | max ms | upload ms | render ms | tick ms | cols/s | RAM peak MB |", "|---|---|---|---|---|---|---|---|---|---|---|---|")
foreach ($j in $results) {
  $md += "| $($j.name) | $($j.present) | $($j.display) | $($j.frames) | $($j.frame_ms.avg) | $($j.frame_ms.p99) | $($j.frame_ms.max) | $($j.main_thread_ms.upload) | $($j.main_thread_ms.render) | $($j.sim_tick_ms.avg) | $($j.stream.columns_per_s) | $($j.ram_mb.peak) |"
}
$md | Set-Content (Join-Path $Out "summary.md")
$md | Write-Host
Write-Host "results: $Out"
