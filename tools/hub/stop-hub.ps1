# Stops the hub started by start-hub.ps1 (other hshub processes are left alone).
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Get-Process hshub -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$here\bin\*" } | Stop-Process -Force
Write-Output "hub stopped"
