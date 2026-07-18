$ErrorActionPreference = "Stop"
$pioCommand = Get-Command pio -ErrorAction SilentlyContinue
$pio = if ($pioCommand) { $pioCommand.Source } else { Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe" }
if (-not (Test-Path $pio)) { throw "No se encuentra PlatformIO: $pio" }
Push-Location $PSScriptRoot
try {
    & $pio run -e display-test -t upload
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally { Pop-Location }
