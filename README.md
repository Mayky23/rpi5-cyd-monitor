# CYD Monitor: Raspberry Pi 5

Panel táctil para **ESP32-2432S028R (Cheap Yellow Display)** dedicado a
monitorizar una Raspberry Pi 5. Esta rama instala únicamente las vistas de RPi5.

| Rama | Contenido |
|---|---|
| `web` | Instalador web (GitHub Pages) |
| `rpi5-proxmox` | Edición completa: Raspberry Pi 5 y Proxmox VE |
| `rpi5` | Solo Raspberry Pi 5 |
| `proxmox` | Solo Proxmox VE |

![Todos los paneles de la edición Raspberry Pi 5](docs/panels.png)

*Capturas reales de la pantalla, generadas con datos de ejemplo.*

## Instalación rápida desde el navegador

Sin instalar nada en el PC: abre **<https://mayky23.github.io/rpi5-cyd-monitor/>**
con Chrome, Edge u Opera, elige la edición, escribe tu Wi-Fi, la dirección de la
API y el token, conecta la placa y pulsa *Instalar*. La propia página te da los
comandos exactos para instalar la API en tu servidor.

## Funciones

- Resumen de CPU, RAM, temperatura, almacenamiento, red y servicios.
- Gráficas históricas con escala adaptativa.
- Discos reales sin duplicados y estado `ERROR` cuando una lectura falla.
- Interfaces de red, puertos y contenedores Docker con nombres claros.
- Diagnóstico, brillo nocturno, temas, animaciones y cuatro orientaciones.
- API de solo lectura protegida mediante token.
- Instalador web (Chrome/Edge/Opera) o instalador de Windows que detecta la ESP32 y carga el firmware.

## Requisitos

- ESP32-2432S028R y cable USB de datos.
- Windows 10/11 con PowerShell y Python 3.10 o posterior.
- Raspberry Pi 5 con Raspberry Pi OS Bookworm o Debian 12 en adelante (Python 3.10+).

## 1. Instalar la API en la Raspberry Pi

```bash
git clone --branch rpi5 --single-branch https://github.com/Mayky23/rpi5-cyd-monitor.git
cd rpi5-cyd-monitor/server
sudo ./install.sh
```

El instalador crea un token aleatorio y conserva `/opt/rpi-monitor/server/.env`
durante futuras actualizaciones. Para actualizar más adelante, ejecuta
`git pull` dentro de `rpi5-cyd-monitor` y repite `sudo ./install.sh`.
Puedes consultar el token localmente con:

```bash
sudo grep '^MONITOR_API_TOKEN=' /opt/rpi-monitor/server/.env
```

## 2. Instalar el firmware

La forma más sencilla es el [instalador web](https://mayky23.github.io/rpi5-cyd-monitor/).
También puedes compilarlo y cargarlo con el instalador de Windows. Conecta la
pantalla al PC y ejecuta:

```powershell
.\Install.ps1
```

El asistente instala las dependencias necesarias, pide la Wi-Fi y los datos de
la API, detecta la placa conectada, compila esta edición y la carga por USB.
La información privada queda en `firmware/include/config.local.h`, ignorada por
Git.

La pantalla usa siempre la configuración más reciente: si antes usaste el
instalador web y ahora cargas el firmware con `Install.ps1`, se aplican los datos
nuevos; si después vuelves a la web, gana otra vez la web.

Si la API va por `https://`, indica la huella SHA-256 de su certificado (el
asistente la pide, o usa `-ApiCertSha256`). Sin huella la conexión se cifra pero
el certificado no se comprueba.

## Pantalla de arranque personal

El instalador permite elegir una imagen PNG/JPG o crear una composición con
título, subtítulo y color. También puedes generarla directamente:

```powershell
python firmware/tools/build_custom_splash.py --source "C:\imagenes\logo.png"
python firmware/tools/build_custom_splash.py --title "HOME LAB" --subtitle "SYSTEM STATUS" --accent 00D8A0
```

Se crean automáticamente versiones vertical y horizontal. El archivo generado
`firmware/include/custom_splash.local.h` está excluido de Git; selecciónalo como
`PERSONAL` en la página Arranque.

## Seguridad

- No publiques `config.local.h`, `custom_splash.local.h` ni `server/.env`.
- Usa un token largo y distinto de tus contraseñas.
- No expongas la API directamente a Internet; usa una VPN o proxy HTTPS.
- Con una API `https://`, fija la huella SHA-256 de su certificado en el panel.

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
