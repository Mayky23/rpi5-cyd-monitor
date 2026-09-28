# CYD Monitor: Raspberry Pi 5

Panel táctil para **ESP32-2432S028R (Cheap Yellow Display)** dedicado a
monitorizar una Raspberry Pi 5. Esta rama instala únicamente las vistas de RPi5.

| Rama | Integración |
|---|---|
| `main` | Raspberry Pi 5 y Proxmox VE |
| `rpi5` | Solo Raspberry Pi 5 |
| `proxmox` | Solo Proxmox VE |

## Funciones

- Resumen de CPU, RAM, temperatura, almacenamiento, red y servicios.
- Gráficas históricas con escala adaptativa.
- Discos reales sin duplicados y estado `ERROR` cuando una lectura falla.
- Interfaces de red, puertos y contenedores Docker con nombres claros.
- Diagnóstico, brillo nocturno, temas, animaciones y cuatro orientaciones.
- API de solo lectura protegida mediante token.
- Instalador de Windows que detecta la ESP32 y carga el firmware.

## Requisitos

- ESP32-2432S028R y cable USB de datos.
- Windows 10/11 con PowerShell y Python 3.10 o posterior.
- Raspberry Pi 5 con Raspberry Pi OS o Debian.

## 1. Instalar la API en la Raspberry Pi

```bash
git clone --branch rpi5 --single-branch https://github.com/Mayky23/rpi5-cyd-monitor.git
cd rpi5-cyd-monitor/server
sudo ./install.sh
```

El instalador crea un token aleatorio. Puedes consultarlo localmente con:

```bash
sudo grep '^MONITOR_API_TOKEN=' /opt/rpi-monitor/server/.env
```

## 2. Instalar el firmware

Conecta la pantalla al PC y ejecuta:

```powershell
.\Install.ps1
```

El asistente instala las dependencias necesarias, pide la Wi-Fi y los datos de
la API, detecta la placa conectada, compila esta edición y la carga por USB.
La información privada queda en `firmware/include/config.local.h`, ignorada por
Git.

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

## Desarrollo

```powershell
python -m pip install -r server/requirements-dev.txt
python -m unittest discover -s tests -v
pio run -d firmware -e esp32-2432S028R
```
