$ErrorActionPreference = "Stop"
# PlatformIO from PATH, from the IDE, or as a Python module (pip install --user platformio).
$pio = @()
$command = Get-Command pio -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
$known = if ($env:USERPROFILE) { Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe" } else { "" }
if ($command) { $pio = @($command.Source) }
elseif ($known -and (Test-Path $known)) { $pio = @($known) }
else {
    foreach ($name in @("py", "python", "python3")) {
        $python = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $python) { continue }
        & $python.Source -m platformio --version 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $pio = @($python.Source, "-m", "platformio"); break }
    }
}
if (-not $pio) { throw "No se encuentra PlatformIO. Ejecuta antes .\Install.ps1 desde la raiz del proyecto." }
$prefix = @($pio | Select-Object -Skip 1)
Push-Location $PSScriptRoot
try {
    & $pio[0] @prefix device monitor -b 115200
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally { Pop-Location }
