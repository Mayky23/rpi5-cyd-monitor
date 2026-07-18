$ErrorActionPreference = "Stop"

function Find-PlatformIO {
    $command = Get-Command pio -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $defaultPath = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
    if (Test-Path $defaultPath) { return $defaultPath }

    throw "No se encuentra PlatformIO. Instala PlatformIO IDE en VS Code o PlatformIO Core."
}

$firmwareDir = Join-Path $PSScriptRoot "firmware"
if (-not (Test-Path (Join-Path $firmwareDir "platformio.ini"))) {
    throw "No se encuentra firmware\platformio.ini. Ejecuta este script desde la raíz del repositorio."
}

if (-not (Test-Path (Join-Path $firmwareDir "include\config.local.h"))) {
    Write-Warning "No existe firmware\include\config.local.h. Se usarán los valores de ejemplo de config.h."
    Write-Warning "Copia config.local.example.h como config.local.h y añade Wi-Fi, URL y token."
}

$pio = Find-PlatformIO
Write-Host "Proyecto: $firmwareDir" -ForegroundColor DarkGray
Write-Host "PlatformIO: $pio" -ForegroundColor DarkGray

Push-Location $firmwareDir
try {
    Write-Host "[1/2] Compilando firmware..." -ForegroundColor Cyan
    & $pio run -e esp32-2432S028R
    if ($LASTEXITCODE -ne 0) { throw "El build ha fallado. No se intentará cargar el firmware." }

    Write-Host "[2/2] Cargando firmware en la CYD..." -ForegroundColor Cyan
    & $pio run -e esp32-2432S028R -t upload
    if ($LASTEXITCODE -ne 0) { throw "El upload ha fallado. Comprueba el cable USB y el puerto COM." }

    Write-Host "BUILD + UPLOAD completados correctamente." -ForegroundColor Green
}
finally {
    Pop-Location
}
