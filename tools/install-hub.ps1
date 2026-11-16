# Sets up (or updates) an always-on hashstash hub from this build.
#
#   cmake -B build -G Ninja; cmake --build build      # build first
#   .\tools\install-hub.ps1                            # -> C:\Projects\hashstash-hub
#   .\tools\install-hub.ps1 -HubDir D:\hshub -NoStart
#
# The hub runs from copies of hshub.exe / hs.exe in <HubDir>\bin, so rebuilding
# hashstash never needs it stopped. Its data lives in <HubDir>\data and is kept
# when updating. Pages: http://localhost:7420/ui (this machine only by default).
param(
  [string]$HubDir = "C:\Projects\hashstash-hub",
  [switch]$NoStart
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$build = Join-Path $repo "build"
foreach ($exe in "hshub.exe", "hs.exe") {
  if (-not (Test-Path (Join-Path $build $exe))) { throw "build\$exe not found: build hashstash first (cmake --build build)" }
}
New-Item -ItemType Directory -Force (Join-Path $HubDir "bin"), (Join-Path $HubDir "data") | Out-Null
Copy-Item (Join-Path $repo "tools\hub\start-hub.ps1"), (Join-Path $repo "tools\hub\stop-hub.ps1") $HubDir -Force
$running = Get-Process hshub -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$HubDir\bin\*" }
if ($running) { & (Join-Path $HubDir "stop-hub.ps1") }
Copy-Item (Join-Path $build "hshub.exe"), (Join-Path $build "hs.exe") (Join-Path $HubDir "bin") -Force
Write-Output "hub installed in $HubDir (data kept in $HubDir\data)"
if (-not $NoStart) { & (Join-Path $HubDir "start-hub.ps1") }
Write-Output "Add yourself as a person who can sign in to the pages:"
Write-Output "  $HubDir\bin\hshub.exe user add <your name> --root $HubDir\data"
