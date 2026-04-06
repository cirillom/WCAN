# deploy.ps1 — build & flash in separate terminals, then open monitors for all ports
$scriptDir = $PSScriptRoot

$esp32Ports   = @("COM24")
$esp32c3Ports = @("COM10", "COM11")

# --- BUILD & FLASH (two terminals, wait for both to finish) ---
Write-Host "==> Launching build & flash terminals..."
$p1 = Start-Process powershell -ArgumentList "-File `"$scriptDir\deploy_target.ps1`" -Target esp32   -BuildDir build_esp32   -SdkConfig sdkconfig_esp32   -Ports $($esp32Ports -join ',')"   -PassThru
$p2 = Start-Process powershell -ArgumentList "-File `"$scriptDir\deploy_target.ps1`" -Target esp32c3 -BuildDir build_esp32c3 -SdkConfig sdkconfig_esp32c3 -Ports $($esp32c3Ports -join ',')" -PassThru

Write-Host "Waiting for build & flash to finish..."
$p1.WaitForExit()
$p2.WaitForExit()

if ($p1.ExitCode -ne 0 -or $p2.ExitCode -ne 0) {
    Write-Host "Build/flash failed. Aborting." -ForegroundColor Red
    exit 1
}
Write-Host "Build & flash complete!" -ForegroundColor Green

# --- MONITOR (one terminal per port, all stay open) ---
Write-Host "==> Opening monitors..."
$cmakeEsp32   = "--define-cache-entry SDKCONFIG=sdkconfig_esp32"
$cmakeEsp32c3 = "--define-cache-entry SDKCONFIG=sdkconfig_esp32c3"

foreach ($p in $esp32Ports)   { Start-Process cmd -ArgumentList "/k title ESP32 $p && idf.py -B build_esp32 $cmakeEsp32 -p $p monitor" }
foreach ($p in $esp32c3Ports) { Start-Process cmd -ArgumentList "/k title ESP32C3 $p && idf.py -B build_esp32c3 $cmakeEsp32c3 -p $p monitor" }

Write-Host "All monitors opened." -ForegroundColor Green