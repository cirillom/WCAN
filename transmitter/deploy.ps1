# deploy.ps1
$esp32Ports   = @("COM3", "COM4", "COM5")
$esp32c3Ports = @("COM6", "COM7")

# --- BUILD ---
Write-Host "==> Setting targets..."
idf.py -B build_esp32   set-target esp32
idf.py -B build_esp32c3 set-target esp32c3

Write-Host "==> Building in parallel..."
$j1 = Start-Process "idf.py" -ArgumentList "-B build_esp32 build"   -PassThru -NoNewWindow
$j2 = Start-Process "idf.py" -ArgumentList "-B build_esp32c3 build" -PassThru -NoNewWindow
$j1.WaitForExit(); $j2.WaitForExit()
Write-Host "Build complete!"

# --- FLASH ---
Write-Host "==> Flashing all devices..."
$jobs = @()
foreach ($p in $esp32Ports)   { $jobs += Start-Process "idf.py" -ArgumentList "-B build_esp32   -p $p flash" -PassThru -NoNewWindow }
foreach ($p in $esp32c3Ports) { $jobs += Start-Process "idf.py" -ArgumentList "-B build_esp32c3 -p $p flash" -PassThru -NoNewWindow }
$jobs | ForEach-Object { $_.WaitForExit() }
Write-Host "All devices flashed!"

# --- MONITOR ---
Write-Host "==> Opening monitors..."
wt `
  new-tab --title "ESP32 $($esp32Ports[0])"   cmd /k "idf.py -B build_esp32   -p $($esp32Ports[0]) monitor" `; `
  new-tab --title "ESP32 $($esp32Ports[1])"   cmd /k "idf.py -B build_esp32   -p $($esp32Ports[1]) monitor" `; `
  new-tab --title "ESP32 $($esp32Ports[2])"   cmd /k "idf.py -B build_esp32   -p $($esp32Ports[2]) monitor" `; `
  new-tab --title "ESP32C3 $($esp32c3Ports[0])" cmd /k "idf.py -B build_esp32c3 -p $($esp32c3Ports[0]) monitor" `; `
  new-tab --title "ESP32C3 $($esp32c3Ports[1])" cmd /k "idf.py -B build_esp32c3 -p $($esp32c3Ports[1]) monitor"