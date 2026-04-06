# deploy_target.ps1 — build and flash a single target, then exit
param(
    [Parameter(Mandatory)] [string]   $Target,
    [Parameter(Mandatory)] [string]   $BuildDir,
    [Parameter(Mandatory)] [string]   $SdkConfig,
    [Parameter(Mandatory)] [string[]] $Ports
)

# Start-Process passes Ports as a single comma-separated string, split it back into an array
$Ports = $Ports -split ','
$cmakeArgs = "--define-cache-entry SDKCONFIG=$SdkConfig"

$ErrorActionPreference = "Stop"
Remove-Item Env:IDF_TARGET -ErrorAction SilentlyContinue

Write-Host "=== [$Target] Build ===" -ForegroundColor Cyan
idf.py -B $BuildDir $cmakeArgs build
if ($LASTEXITCODE -ne 0) { Write-Host "Build FAILED" -ForegroundColor Red; Read-Host "Press Enter to exit"; exit 1 }

Write-Host "=== [$Target] Flash ===" -ForegroundColor Cyan
foreach ($p in $Ports) {
    Write-Host "  Flashing $p..."
    idf.py -B $BuildDir $cmakeArgs -p $p flash
    if ($LASTEXITCODE -ne 0) { Write-Host "Flash FAILED on $p" -ForegroundColor Red; Read-Host "Press Enter to exit"; exit 1 }
}

Write-Host "=== [$Target] Done ===" -ForegroundColor Green
