# CYD Monitor: Proxmox VE

Panel táctil para **ESP32-2432S028R (Cheap Yellow Display)** dedicado a
monitorizar Proxmox VE. Esta rama instala únicamente las vistas del hipervisor.

| Rama | Contenido |
|---|---|
| `web` | Instalador web (GitHub Pages) |
| `rpi5-proxmox` | Edición completa: Raspberry Pi 5 y Proxmox VE |
| `rpi5` | Solo Raspberry Pi 5 |
| `proxmox` | Solo Proxmox VE |

![Todos los paneles de la edición Proxmox VE](docs/panels.png)

*Capturas reales de la pantalla, generadas con datos de ejemplo.*

## Instalación rápida desde el navegador

Sin instalar nada en el PC: abre **<https://mayky23.github.io/rpi5-cyd-monitor/>**
con Chrome, Edge u Opera, elige la edición, escribe tu Wi-Fi, la dirección de la
API y el token, conecta la placa y pulsa *Instalar*. La propia página te da los
comandos exactos para instalar la API en tu servidor.

## Funciones

- Estado, uptime, CPU y memoria de los nodos.
- Gráficas históricas con escala adaptativa.
- Inventario y estado de máquinas virtuales y contenedores LXC.
- Uso y disponibilidad del almacenamiento.
- Historial de tareas con detalle de errores y cancelaciones.
- Estado `OFFLINE` sin conservar métricas antiguas cuando Proxmox está apagado, y
  `SIN CONFIG` en ámbar mientras la API aún no tiene los datos de Proxmox.
- Diagnóstico, brillo nocturno, temas, animaciones y cuatro orientaciones.
- Instalador web (Chrome/Edge/Opera) o instalador de Windows que detecta la ESP32 y carga el firmware.

## Requisitos

- ESP32-2432S028R y cable USB de datos.
- Windows 10/11 con PowerShell y Python 3.10 o posterior.
- Un equipo Debian/Linux con Python 3.10 o posterior para alojar la pasarela API
  (Debian 12 / Raspberry Pi OS Bookworm en adelante).
- Un token de Proxmox VE con el rol de solo lectura `PVEAuditor`.

## 1. Preparar Proxmox

Crea en Proxmox un usuario técnico y un token API dedicados. Asigna únicamente
el rol `PVEAuditor` sobre `/`; el panel no necesita permisos de escritura.
Conserva el identificador completo y el secreto que Proxmox muestra una sola vez.

Obtén la huella SHA-256 del certificado desde el equipo que alojará la API:

```bash
openssl s_client -connect proxmox.example.lan:8006 </dev/null 2>/dev/null \
  | openssl x509 -noout -fingerprint -sha256 \
  | cut -d= -f2 | tr -d ':'
```

## 2. Instalar la pasarela API

En un equipo Debian/Linux, preferiblemente uno que permanezca encendido:

```bash
git clone --branch proxmox --single-branch https://github.com/Mayky23/rpi5-cyd-monitor.git
cd rpi5-cyd-monitor/server
sudo ./install.sh \
  --proxmox-url 'https://proxmox.example.lan:8006/api2/json' \
  --proxmox-token-id 'monitor@pve!esp32' \
  --proxmox-token-secret 'secreto-del-token-api' \
  --proxmox-cert-sha256 'huella-sha256'
```

El instalador comprueba el formato de los datos, genera el token de la API y
conserva `/opt/rpi-monitor/server/.env` en futuras actualizaciones (para
actualizar, ejecuta `git pull` y repite `sudo ./install.sh`). Consulta el token
con `sudo grep '^MONITOR_API_TOKEN=' /opt/rpi-monitor/server/.env`.

También puedes editar ese archivo a mano, sin comillas y sin cambiar
`MONITOR_API_TOKEN`:

```dotenv
PROXMOX_API_URL=https://proxmox.example.lan:8006/api2/json
PROXMOX_TOKEN_ID=monitor@pve!esp32
PROXMOX_TOKEN_SECRET=secreto-del-token-api
PROXMOX_CERT_SHA256=huella-sha256-sin-dos-puntos
```

Después de editarlo, aplica la configuración:

```bash
sudo systemctl restart rpi-monitor
```

Si Proxmox está apagado o no responde, la pasarela elimina las métricas antiguas
y el panel muestra `OFFLINE` en lugar de datos que ya no son válidos.

## 3. Instalar el firmware

La forma más sencilla es el [instalador web](https://mayky23.github.io/rpi5-cyd-monitor/).
También puedes compilarlo y cargarlo con el instalador de Windows. Conecta la
pantalla al PC y ejecuta:

```powershell
.\Install.ps1
```

El asistente instala las dependencias, solicita Wi-Fi, URL y token, detecta la
placa, compila esta edición y la carga por USB. Los datos privados quedan en
`firmware/include/config.local.h`, que Git ignora.

La pantalla usa siempre la configuración más reciente: si antes usaste el
instalador web y ahora cargas el firmware con `Install.ps1`, se aplican los datos
nuevos; si después vuelves a la web, gana otra vez la web.

Si la pasarela va por `https://`, indica la huella SHA-256 de su certificado (el
asistente la pide, o usa `-ApiCertSha256`). Sin huella la conexión se cifra pero
el certificado no se comprueba.

## Pantalla de arranque personal

Durante la instalación puedes elegir un PNG/JPG o crear un diseño con título,
subtítulo y color. También puedes generarlo directamente:

```powershell
python firmware/tools/build_custom_splash.py --source "C:\imagenes\logo.png"
python firmware/tools/build_custom_splash.py --title "VIRTUAL LAB" --subtitle "PROXMOX VE" --accent E57000
```

El generador crea las orientaciones vertical y horizontal. El recurso local está
excluido de Git y aparece como `PERSONAL` en la página Arranque.

## Seguridad

- Mantén el usuario de Proxmox con permisos estrictamente de auditoría.
- No publiques `config.local.h`, `custom_splash.local.h` ni `server/.env`.
- No expongas la pasarela directamente a Internet; utiliza una VPN o proxy HTTPS.
- La huella SHA-256 impide aceptar un certificado de Proxmox distinto al previsto,
  también al reconectar.
- Con una pasarela `https://`, fija también su huella SHA-256 en el panel.

## Estructura

| Ruta | Contenido |
|---|---|
| `server/` | API FastAPI de solo lectura, instalador y servicio systemd. |
| `firmware/` | Firmware PlatformIO, fuentes, animaciones y herramientas de generación y captura. |
| `tests/` | Pruebas de la API, de las herramientas y de la lógica del firmware en el PC (`tests/firmware`). |
| `Install.ps1` | Instalador guiado para Windows. |

## Desarrollo

```powershell
python -m pip install -r server/requirements-dev.txt -r firmware/tools/requirements.txt
python -m unittest discover -s tests -v
python firmware/tools/build_gallery.py --check
python firmware/tools/check_fonts.py
python firmware/tools/check_theme_contrast.py
pio run -d firmware -e esp32-2432S028R
bash tests/firmware/run.sh   # lógica del firmware en el PC (Linux/WSL, tras compilar)
```

El firmware se valida en CI. Las mismas correcciones deben aplicarse en las ramas
`rpi5`, `proxmox` y `rpi5-proxmox`, que comparten el código; el CI avisa si el
código común se ha desincronizado. Las herramientas USB de `firmware/tools` capturan
los paneles, orientaciones, temas y animaciones desde la placa real.

Para regenerar la imagen de este README, instala
`firmware/tools/requirements.txt`, carga el firmware con capturas y ejecuta el
generador. Usa datos de ejemplo, así que no expone tu red ni tus servidores:

```powershell
$env:PLATFORMIO_BUILD_FLAGS = "-D SPLASH_CAPTURE_ENABLED=1"
pio run -d firmware -e esp32-2432S028R -t upload
python firmware/tools/build_gallery.py --capture --port COM3
```

Después ejecuta `Remove-Item Env:PLATFORMIO_BUILD_FLAGS` y vuelve a cargar el
firmware normal con `.\Install.ps1`.
