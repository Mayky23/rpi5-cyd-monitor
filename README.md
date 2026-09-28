# CYD Monitor: RPi5 + Proxmox

Panel tactil para **ESP32-2432S028R (Cheap Yellow Display)** que monitoriza una
Raspberry Pi 5 y un servidor Proxmox VE desde una sola interfaz.

Esta es la edicion combinada del proyecto. El repositorio dispone de tres ramas:

| Rama | Paneles incluidos |
|---|---|
| `main` | Raspberry Pi 5 y Proxmox VE |
| `rpi5` | Solo Raspberry Pi 5 |
| `proxmox` | Solo Proxmox VE |

## Funciones

- Resumen, CPU, RAM, temperatura, discos, red, puertos y Docker de la RPi5.
- Resumen, rendimiento, VM/LXC, almacenamiento y tareas de Proxmox.
- Graficas historicas independientes con escala adaptativa.
- Estados claros `ONLINE`, `PARCIAL`, `OFFLINE` y `ERROR`.
- Cuatro orientaciones, brillo nocturno, diez temas y navegacion tactil.
- Cuatro animaciones de arranque genericas y una pantalla `PERSONAL`.
- API de solo lectura protegida con token y certificado Proxmox fijado por SHA-256.
- Instalador de Windows que detecta la placa, instala dependencias y carga el firmware.

## Requisitos

- ESP32-2432S028R con cable USB de datos.
- Windows 10/11 con PowerShell y Python 3.10 o posterior.
- Una Raspberry Pi o equipo Linux Debian para alojar la API.
- Para esta rama, acceso de solo lectura a la API de Proxmox VE.

## 1. Instalar la API

En el equipo Linux:

```bash
git clone https://github.com/Mayky23/rpi5-cyd-monitor.git
cd rpi5-cyd-monitor/server
sudo ./install.sh
```

El instalador crea un token aleatorio y conserva `/opt/rpi-monitor/server/.env`
durante futuras actualizaciones. Consulta el token localmente con:

```bash
sudo grep '^MONITOR_API_TOKEN=' /opt/rpi-monitor/server/.env
```

Para activar Proxmox, completa estas variables en ese archivo:

```dotenv
PROXMOX_API_URL=https://proxmox.example.lan:8006/api2/json
PROXMOX_TOKEN_ID=monitor@pve!esp32
PROXMOX_TOKEN_SECRET=PEGA_EL_SECRETO_DEL_TOKEN
PROXMOX_CERT_SHA256=HUELLA_SHA256_SIN_DOS_PUNTOS
```

Usa un token dedicado con permisos de auditoria. Reinicia la API tras editarlo:

```bash
sudo systemctl restart rpi-monitor
```

## 2. Instalar el firmware

Conecta la pantalla al PC y ejecuta:

```powershell
.\Install.ps1
```

El asistente:

1. Localiza Python y prepara PlatformIO si hace falta.
2. Solicita Wi-Fi, URL de la API y token sin publicarlos.
3. Detecta automaticamente placas ESP32 por USB y permite elegir si hay varias.
4. Compila la edicion correspondiente a la rama y la carga en la placa.

La configuracion privada se guarda en `firmware/include/config.local.h`. Este
archivo esta excluido de Git.

## Pantalla personalizada

Durante la instalacion se puede:

- Elegir una imagen PNG o JPG, que se adapta a 320x240 y 240x320.
- Crear un diseño nuevo indicando titulo, subtitulo y color principal.

Tambien se puede generar directamente:

```powershell
python firmware/tools/build_custom_splash.py --source "C:\imagenes\logo.png"
python firmware/tools/build_custom_splash.py --title "HOME LAB" --subtitle "SYSTEM STATUS" --accent 00D8A0
```

El recurso resultante se guarda como `firmware/include/custom_splash.local.h`,
tambien excluido de Git. Selecciona `PERSONAL` desde la pagina Arranque del panel.

## Seguridad

- No publiques `config.local.h`, `custom_splash.local.h` ni `.env`.
- No reutilices contraseñas personales como tokens de la API.
- No expongas el puerto 8787 directamente a Internet; utiliza una VPN o proxy HTTPS.
- El certificado de Proxmox se valida mediante su huella SHA-256.

## Desarrollo y pruebas

```powershell
python -m pip install -r server/requirements-dev.txt
python -m unittest discover -s tests -v
pio run -d firmware -e esp32-2432S028R
```

El firmware se valida en CI y las herramientas USB de `firmware/tools` permiten
capturar paneles, orientaciones, temas y animaciones desde la placa real.
