# Starts the hashstash hub in the background (if it isn't already running).
# Pages: http://localhost:7420/ui   API: http://localhost:7420/api/projects
# It runs from bin\ (a copy), so rebuilding hashstash doesn't need it stopped.
# To update it: .\stop-hub.ps1, copy the new hshub.exe/hs.exe into bin\, .\start-hub.ps1
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (Get-Process hshub -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$here\bin\*" }) {
  Write-Output "hub already running: http://localhost:7420/ui"
  exit 0
}
Start-Process -FilePath "$here\bin\hshub.exe" -ArgumentList @('serve', '--root', "$here\data", '--port', '7420') `
  -WorkingDirectory $here -WindowStyle Hidden `
  -RedirectStandardOutput "$here\hub.log" -RedirectStandardError "$here\hub-errors.log"
Start-Sleep -Seconds 1
Write-Output "hub started: http://localhost:7420/ui"
