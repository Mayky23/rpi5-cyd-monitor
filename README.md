# CYD Monitor Installer

Esta rama (`web`) aloja el **instalador web** de CYD Monitor, el panel táctil para
la **ESP32-2432S028R (Cheap Yellow Display)** que vigila una Raspberry Pi 5, un
servidor Proxmox VE o ambos.

**<https://mayky23.github.io/rpi5-cyd-monitor/>**

Abre la página con Chrome, Edge u Opera, elige qué quieres vigilar, escribe tu
Wi-Fi, la dirección de la API y el token, conecta la placa por USB y pulsa
*Instalar*. La página también te da los comandos para instalar la API en tu
servidor. No hace falta descargar nada ni instalar programas en el PC.

## ¿Qué rama necesito?

El proyecto tiene **una rama por herramienta**. Cada una contiene su propio
firmware, su API, su instalador de Windows y su documentación, y no comparten
código entre sí. Elige la rama según lo que quieras monitorizar:

| Quiero vigilar… | Rama | Paneles |
|---|---|---|
| **Una Raspberry Pi 5 y un servidor Proxmox** en la misma pantalla | [`rpi5-proxmox`](../../tree/rpi5-proxmox) | 17 |
| **Solo una Raspberry Pi 5** | [`rpi5`](../../tree/rpi5) | 12 |
| **Solo un servidor Proxmox VE** | [`proxmox`](../../tree/proxmox) | 10 |
| Instalarlo desde el navegador, sin clonar nada | `web` (esta rama) | — |

Si usas el instalador web **no tienes que elegir ni clonar ninguna rama**: la
página lo hace por ti según la edición que marques en el primer paso. Las ramas
solo te hacen falta si prefieres compilar el firmware tú mismo, modificar el
código o instalar la API a mano.

Para descargar solo la que necesites:

```bash
git clone --branch rpi5 --single-branch https://github.com/Mayky23/rpi5-cyd-monitor.git
```

Cambia `rpi5` por `proxmox` o `rpi5-proxmox` según el caso. Cada rama tiene su
`README.md` con los pasos completos.

## Cómo funciona

1. `.github/workflows/pages.yml` compila el firmware de las tres ramas de edición.
2. `web/build_site.py` reúne los binarios y la página en un único sitio.
3. GitHub Pages lo publica. La página carga el firmware con
   [ESP Web Tools](https://esphome.github.io/esp-web-tools/) mediante Web Serial.

Los datos que escribes (Wi-Fi, dirección de la API y token) se generan en tu
navegador y se graban en la placa por USB; no se envían a ningún servidor. El
firmware los lee de un bloque de 4 KB de su memoria flash
(`firmware/include/runtime_config.h`, en las ramas de edición).

## Desarrollo

```powershell
python -m unittest discover -s tests -v
```

Para ver la página en local necesitas una carpeta con los binarios de cada
edición (`bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin`,
`VERSION` y `COMMIT`):

```powershell
python web/build_site.py --artifacts artifacts --out site
python -m http.server 8000 --directory site
```

Después abre <http://localhost:8000>. Web Serial funciona en `localhost` y en HTTPS.

### Publicar

En *Settings → Pages* elige *Source: GitHub Actions*. El flujo se lanza con cada
`push` a `web` o a mano desde *Actions*. Como compila las ramas de edición, sube
antes `rpi5`, `proxmox` y `rpi5-proxmox`, y `web` al final.

Los logotipos de Raspberry Pi y Proxmox son marcas de sus respectivos
propietarios y se usan solo para identificar cada edición. La tipografía de los
títulos es Aileron, de Sora Sagano, sin derechos reservados.
