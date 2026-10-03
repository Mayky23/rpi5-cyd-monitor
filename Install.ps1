[CmdletBinding()]
param(
    [string]$WifiSsid,
    [string]$WifiPassword,
    [string]$ApiUrl,
    [string]$ApiToken,
    [string]$ApiCertSha256,
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
    # The Microsoft Store "python" alias exists even without Python: run each candidate.
    foreach ($name in @("py", "python", "python3")) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $command) { continue }
        & $command.Source -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" 2>$null
        if ($LASTEXITCODE -eq 0) { return $command.Source }
    }
    throw "Python 3.10 o posterior no esta instalado. Instalalo desde https://www.python.org/downloads/ y repite el proceso."
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

# Returns the command line that runs PlatformIO: the executable plus its leading arguments.
function Find-PlatformIO([string]$Python) {
    $command = Get-Command pio -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return , @($command.Source) }
    $known = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
    if (Test-Path $known) { return , @($known) }
    & $Python -m platformio --version 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Instalando PlatformIO..." -ForegroundColor Cyan
        & $Python -m pip install --user --upgrade platformio
        if ($LASTEXITCODE -ne 0) { throw "No se pudo instalar PlatformIO." }
        & $Python -m platformio --version 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO se instalo pero no responde. Abre otra terminal y repite el proceso." }
    }
    # pip --user leaves pio.exe outside PATH; running it as a module always works.
    return , @($Python, "-m", "platformio")
}

function Invoke-Pio([string[]]$Pio, [string[]]$Arguments) {
    $prefix = @($Pio | Select-Object -Skip 1)
    & $Pio[0] @prefix @Arguments
}

# Windows PowerShell 5.1 does not enumerate the array returned by ConvertFrom-Json:
# unroll it explicitly so every serial port is a separate element.
function ConvertTo-PortList([string]$Json) {
    $parsed = $Json | ConvertFrom-Json
    return , @($parsed | ForEach-Object { $_ } | Where-Object { $_ -and $_.port })
}

function Select-Esp32Port([string[]]$Pio, [string]$RequestedPort) {
    if ($RequestedPort) { return $RequestedPort }
    $raw = Invoke-Pio $Pio @("device", "list", "--json-output")
    if ($LASTEXITCODE -ne 0) { throw "No se pudieron consultar los puertos serie." }
    $devices = ConvertTo-PortList ($raw | Out-String)
    $likely = @($devices | Where-Object {
        ([string]$_.description + " " + [string]$_.hwid) -match "ESP32|CP210|CH340|CH341|USB.Serial|10C4:EA60|1A86:7523"
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
    $answer = Read-Host "Selecciona la placa"
    $choice = 0
    if (-not [int]::TryParse($answer, [ref]$choice) -or $choice -lt 1 -or $choice -gt $likely.Count) {
        throw "Seleccion no valida."
    }
    return [string]$likely[$choice - 1].port
}

function Get-Utf8Length([string]$Value) {
    return [Text.Encoding]::UTF8.GetByteCount($Value)
}

if ($MyInvocation.InvocationName -eq ".") { return }  # dot-sourced by the tests: functions only

Write-Host "CYD Monitor $Version - edicion $Edition" -ForegroundColor Cyan
$Python = Get-Python
$Pio = Find-PlatformIO $Python
$ConfigPath = Join-Path (Join-Path $FirmwareDir "include") "config.local.h"

if (-not $KeepExistingConfig -or -not (Test-Path $ConfigPath)) {
    if (-not $WifiSsid) { $WifiSsid = Read-Host "Nombre de la red Wi-Fi" }
    if (-not $PSBoundParameters.ContainsKey("WifiPassword")) {
        $WifiPassword = Get-PlainSecret "Contrasena Wi-Fi (Enter si la red es abierta)"
    }
    if (-not $ApiUrl) { $ApiUrl = Read-Host "URL de la API (ejemplo http://192.168.1.50:8787)" }
    if (-not $ApiToken) { $ApiToken = Get-PlainSecret "Token de la API" }
    if (-not $WifiSsid -or -not $ApiUrl -or -not $ApiToken) {
        throw "Wi-Fi, URL y token son obligatorios."
    }
    if ((Get-Utf8Length $WifiSsid) -gt 32) { throw "El nombre de la Wi-Fi no puede pasar de 32 bytes." }
    $passwordLength = Get-Utf8Length $WifiPassword
    if ($passwordLength -gt 0 -and ($passwordLength -lt 8 -or ($passwordLength -gt 63 -and $WifiPassword -notmatch '^[0-9a-fA-F]{64}$'))) {
        throw "La contrasena Wi-Fi debe tener entre 8 y 63 caracteres (o 64 hexadecimales), o quedar vacia si la red es abierta."
    }
    if ($ApiToken.Length -lt 16 -or $ApiToken.Length -gt 128 -or $ApiToken -match '\s') {
        throw "El token debe tener entre 16 y 128 caracteres y no contener espacios."
    }
    $parsedApiUrl = $null
    if (-not [Uri]::TryCreate($ApiUrl, [UriKind]::Absolute, [ref]$parsedApiUrl) -or
        $parsedApiUrl.Scheme -notin @("http", "https")) {
        throw "La URL de la API debe comenzar por http:// o https:// y contener un host valido."
    }
    if ($parsedApiUrl.Scheme -eq "https" -and -not $PSBoundParameters.ContainsKey("ApiCertSha256")) {
        $ApiCertSha256 = Read-Host "Huella SHA-256 del certificado de la API (Enter para no comprobarlo)"
    }
    $pin = ($ApiCertSha256 -replace '[:\s]', '').ToLowerInvariant()
    if ($pin -and $pin -notmatch '^[0-9a-f]{64}$') {
        throw "La huella del certificado debe tener 64 caracteres hexadecimales."
    }
    if ($parsedApiUrl.Scheme -eq "https" -and -not $pin) {
        Write-Host "Aviso: con https sin huella la conexion se cifra pero el certificado no se comprueba." -ForegroundColor Yellow
    }
    # The panel uses whichever configuration is newer: this build or a later web installer block.
    $stamp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $config = @"
#pragma once

#define WIFI_SSID "$(Escape-CString $WifiSsid)"
#define WIFI_PASSWORD "$(Escape-CString $WifiPassword)"
#define API_BASE_URL "$(Escape-CString $ApiUrl.TrimEnd('/'))"
#define API_TOKEN "$(Escape-CString $ApiToken)"
#define API_CERT_SHA256 "$pin"
#define CONFIG_STAMP ${stamp}UL
"@
    [IO.File]::WriteAllText($ConfigPath, $config + "`n", [Text.UTF8Encoding]::new($false))
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
    & $Python -c "from PIL import Image; Image.Image.get_flattened_data" 2>$null
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
    Invoke-Pio $Pio @("run", "-e", "esp32-2432S028R")
    if ($LASTEXITCODE -ne 0) { throw "La compilacion ha fallado; no se ha modificado la placa." }
    Write-Host "Cargando firmware en $SelectedPort..." -ForegroundColor Cyan
    Invoke-Pio $Pio @("run", "-e", "esp32-2432S028R", "-t", "upload", "--upload-port", $SelectedPort)
    if ($LASTEXITCODE -ne 0) { throw "La carga ha fallado. Revisa el cable y el puerto." }
} finally {
    Pop-Location
}

Write-Host "Instalacion completada correctamente: CYD Monitor $Version ($Edition)." -ForegroundColor Green
