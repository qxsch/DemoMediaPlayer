# ──────────────────────────────────────────────────────────────
#  build.ps1 – Build DemoMediaPlayer for Windows via Docker
#
#  Usage:
#    .\build.ps1
#    .\build.ps1 -ExtraArgs "--no-cache"
#    .\build.ps1 -ExtraArgs '--build-arg MPV_DEV_URL="https://…"'
# ──────────────────────────────────────────────────────────────
param(
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

Write-Host "=== Building DemoMediaPlayer for Windows (x86_64) ===" -ForegroundColor Cyan
Write-Host ""

$dockerArgs = @(
    "build"
    "--target", "dist"
    "--output", "type=local,dest=$scriptDir\dist"
) + $ExtraArgs + @($scriptDir)

& docker @dockerArgs

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "BUILD FAILED." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Build complete!  Output:" -ForegroundColor Green
Get-ChildItem "$scriptDir\dist" | Format-Table Name, Length -AutoSize
Write-Host "Run dist\mediaplayer.exe to start the player."
