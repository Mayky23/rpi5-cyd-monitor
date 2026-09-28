[CmdletBinding()]
param(
    [string]$WifiSsid,
    [string]$WifiPassword,
    [string]$ApiUrl,
    [string]$ApiToken,
    [string]$Port,
    [string]$SplashImage,
    [switch]$CreateSplash,
    [string]$SplashTitle = "MY MONITOR",
    [string]$SplashSubtitle = "SYSTEM DASHBOARD",
    [string]$SplashAccent = "35C7F3",
    [switch]$KeepExistingConfig,
    [switch]$SkipSplash,
    [switch]$SkipUpload
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$FirmwareDir = Join-Path $Root "firmware"
$Edition = (Get-Content (Join-Path $Root "EDITION") -Raw).Trim()
$VersionPath = Join-Path $Root "VERSION"
$Version = if (Test-Path $VersionPath) { (Get-Content $VersionPath -Raw).Trim() } else { "desarrollo" }
if ($Edition -notin @("combined", "rpi5", "proxmox")) {
    throw "La edicion '$Edition' no es valida. Descarga de nuevo la rama oficial del proyecto."
}

function Get-Python {
    foreach ($name in @("python", "py")) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    throw "Python 3 no esta instalado. Instalalo desde https://www.python.org/downloads/ y repite el proceso."
}

function Get-PlainSecret([string]$Label) {
    $secure = Read-Host $Label -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
}

function Escape-CString([string]$Value) {
    return $Value.Replace('\', '\\').Replace('"', '\"')
}

function Find-PlatformIO([string]$Python) {
    $command = Get-Command pio -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $known = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
    if (Test-Path $known) { return $known }
    Write-Host "Instalando PlatformIO..." -ForegroundColor Cyan
    & $Python -m pip install --user --upgrade platformio
    if ($LASTEXITCODE -ne 0) { throw "No se pudo instalar PlatformIO." }
    $command = Get-Command pio -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    if (Test-Path $known) { return $known }
    throw "PlatformIO se instalo, pero no se encontro su ejecutable. Abre otra terminal y repite el proceso."
}

function Select-Esp32Port([string]$Pio, [string]$RequestedPort) {
    if ($RequestedPort) { return $RequestedPort }
    $raw = & $Pio device list --json-output
    if ($LASTEXITCODE -ne 0) { throw "No se pudieron consultar los puertos serie." }
    $devices = @($raw | Out-String | ConvertFrom-Json)
    $likely = @($devices | Where-Object {
        ($_.description + " " + $_.hwid) -match "ESP32|CP210|CH340|CH341|USB.Serial|10C4:EA60|1A86:7523"
    })
    if ($likely.Count -eq 0) { $likely = $devices }
    if ($likely.Count -eq 0) {
        throw "No se detecto ninguna placa. Conecta la ESP32 por USB, instala su controlador y repite el proceso."
    }
    if ($likely.Count -eq 1) {
        Write-Host "Placa detectada: $($likely[0].port) - $($likely[0].description)" -ForegroundColor Green
        return [string]$likely[0].port
    }
    Write-Host "Se detectaron varios puertos:" -ForegroundColor Yellow
    for ($index = 0; $index -lt $likely.Count; $index++) {
        Write-Host "  [$($index + 1)] $($likely[$index].port) - $($likely[$index].description)"
    }
    $choice = [int](Read-Host "Selecciona la placa")
    if ($choice -lt 1 -or $choice -gt $likely.Count) { throw "Seleccion no valida." }
    return [string]$likely[$choice - 1].port
}

Write-Host "CYD Monitor $Version - edicion $Edition" -ForegroundColor Cyan
$Python = Get-Python
$Pio = Find-PlatformIO $Python
$ConfigPath = Join-Path $FirmwareDir "include\config.local.h"

if (-not $KeepExistingConfig -or -not (Test-Path $ConfigPath)) {
    if (-not $WifiSsid) { $WifiSsid = Read-Host "Nombre de la red Wi-Fi" }
    if (-not $WifiPassword) { $WifiPassword = Get-PlainSecret "Contrasena Wi-Fi" }
    if (-not $ApiUrl) { $ApiUrl = Read-Host "URL de la API (ejemplo http://rpi5.local:8787)" }
    if (-not $ApiToken) { $ApiToken = Get-PlainSecret "Token de la API" }
    if (-not $WifiSsid -or -not $WifiPassword -or -not $ApiUrl -or -not $ApiToken) {
        throw "Wi-Fi, URL y token son obligatorios."
    }
    $parsedApiUrl = $null
    if (-not [Uri]::TryCreate($ApiUrl, [UriKind]::Absolute, [ref]$parsedApiUrl) -or
        $parsedApiUrl.Scheme -notin @("http", "https")) {
        throw "La URL de la API debe comenzar por http:// o https:// y contener un host valido."
    }
    $panelTitle = if ($Edition -eq "proxmox") { "PROXMOX" } elseif ($Edition -eq "rpi5") { "RPI5" } else { "MONITOR" }
    $config = @"
#pragma once

#define WIFI_SSID "$(Escape-CString $WifiSsid)"
#define WIFI_PASSWORD "$(Escape-CString $WifiPassword)"
#define API_BASE_URL "$(Escape-CString $ApiUrl.TrimEnd('/'))"
#define API_TOKEN "$(Escape-CString $ApiToken)"
#define PANEL_TITLE "$panelTitle"
"@
    [IO.File]::WriteAllText($ConfigPath, $config, [Text.UTF8Encoding]::new($false))
    Write-Host "Configuracion privada creada. Git no la publicara." -ForegroundColor Green
}

if (-not $SkipSplash -and -not $SplashImage -and -not $CreateSplash) {
    $custom = Read-Host "Quieres preparar una pantalla de arranque personal? [s/N]"
    if ($custom -match '^[sS]') {
        $mode = Read-Host "Escribe I para usar una imagen o C para crear un diseno"
        if ($mode -match '^[iI]') { $SplashImage = Read-Host "Ruta completa de la imagen PNG o JPG" }
        elseif ($mode -match '^[cC]') {
            $CreateSplash = $true
            $SplashTitle = Read-Host "Titulo"
            $SplashSubtitle = Read-Host "Subtitulo"
            $color = Read-Host "Color principal RRGGBB (Enter para 35C7F3)"
            if ($color) { $SplashAccent = $color }
        }
    }
}

if ($SplashImage -or $CreateSplash) {
    Write-Host "Preparando la pantalla personal..." -ForegroundColor Cyan
    & $Python -c "from PIL import Image" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Instalando el editor de imagenes..." -ForegroundColor Cyan
        & $Python -m pip install --user -r (Join-Path $FirmwareDir "tools\requirements.txt")
        if ($LASTEXITCODE -ne 0) { throw "No se pudo instalar Pillow." }
    }
    $builder = Join-Path $FirmwareDir "tools\build_custom_splash.py"
    if ($SplashImage) {
        $resolvedImage = (Resolve-Path -LiteralPath $SplashImage).Path
        & $Python $builder --source $resolvedImage --fit cover
    } else {
        & $Python $builder --title $SplashTitle --subtitle $SplashSubtitle --accent $SplashAccent
    }
    if ($LASTEXITCODE -ne 0) { throw "No se pudo generar la pantalla personalizada." }
}

if ($SkipUpload) {
    Write-Host "Configuracion terminada. Carga omitida por parametro." -ForegroundColor Yellow
    exit 0
}

$SelectedPort = Select-Esp32Port $Pio $Port
Push-Location $FirmwareDir
try {
    Write-Host "Compilando firmware $Edition..." -ForegroundColor Cyan
    & $Pio run -e esp32-2432S028R
    if ($LASTEXITCODE -ne 0) { throw "La compilacion ha fallado; no se ha modificado la placa." }
    Write-Host "Cargando firmware en $SelectedPort..." -ForegroundColor Cyan
    & $Pio run -e esp32-2432S028R -t upload --upload-port $SelectedPort
    if ($LASTEXITCODE -ne 0) { throw "La carga ha fallado. Revisa el cable y el puerto." }
} finally {
    Pop-Location
}

Write-Host "Instalacion completada correctamente: CYD Monitor $Version ($Edition)." -ForegroundColor Green
