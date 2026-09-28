"""Generate the single public panel gallery for the current repository edition."""

from __future__ import annotations

import argparse
import io
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "docs/panels.png"
FONT_BOLD = ROOT / "firmware/assets/fonts/Aileron-Bold.otf"

BG = "#05090d"
PANEL = "#0c141b"
CARD = "#111d24"
LINE = "#263743"
TEXT = "#f1f5f8"
MUTED = "#91a3b1"
CYAN = "#3dc4f3"
GREEN = "#35d493"
AMBER = "#ffba55"
RED = "#ff667d"

PAGES = {
    "rpi5": ["rpi", "performance", "storage", "network", "cpu", "ports", "docker"],
    "proxmox": ["pve", "pve_performance", "guests", "pve_storage", "tasks"],
    "settings": ["diagnostics", "brightness", "themes", "splash", "orientation"],
}

TITLES = {
    "rpi": "RPI5",
    "performance": "RENDIMIENTO",
    "storage": "DISCOS",
    "network": "RED",
    "cpu": "CPU / TEMP",
    "ports": "PUERTOS",
    "docker": "DOCKER",
    "pve": "PVE RESUMEN",
    "pve_performance": "PVE RENDIM.",
    "guests": "PVE VM / LXC",
    "pve_storage": "PVE DISCOS",
    "tasks": "PVE TAREAS",
    "diagnostics": "DIAGNOSTICO",
    "brightness": "BRILLO",
    "themes": "PERSONALIZACION",
    "splash": "ARRANQUE",
    "orientation": "ORIENTACION",
}


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_BOLD), size)


def text(draw: ImageDraw.ImageDraw, xy: tuple[int, int], value: str, size: int,
         fill: str = TEXT, anchor: str | None = None) -> None:
    draw.text(xy, value, font=font(size), fill=fill, anchor=anchor)


def card(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], outline: str = LINE) -> None:
    draw.rounded_rectangle(box, radius=4, fill=CARD, outline=outline, width=1)


def metric(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], label: str,
           value: str, color: str = CYAN) -> None:
    card(draw, box)
    x1, y1, x2, y2 = box
    text(draw, (x1 + 7, y1 + 5), label, 8, MUTED)
    text(draw, (x1 + 7, y1 + 22), value, 15, color)
    draw.rectangle((x1 + 7, y2 - 8, x2 - 7, y2 - 6), fill="#071017")
    draw.rectangle((x1 + 7, y2 - 8, x1 + 7 + int((x2 - x1 - 14) * 0.55), y2 - 6), fill=color)


def row(draw: ImageDraw.ImageDraw, y: int, left: str, middle: str, right: str,
        color: str = GREEN) -> None:
    draw.ellipse((16, y + 4, 21, y + 9), fill=color)
    text(draw, (25, y), left, 8)
    text(draw, (129, y), middle, 7, MUTED)
    text(draw, (302, y), right, 7, color, "ra")
    draw.line((15, y + 14, 305, y + 14), fill="#1d2b34")


def chart(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], colors=(CYAN, GREEN)) -> None:
    x1, y1, x2, y2 = box
    card(draw, box)
    for step in range(1, 4):
        y = y1 + step * (y2 - y1) // 4
        draw.line((x1 + 8, y, x2 - 8, y), fill="#1a2a34")
    values = ([0.68, 0.55, 0.61, 0.35, 0.47, 0.22, 0.39, 0.18, 0.31, 0.12],
              [0.83, 0.76, 0.79, 0.68, 0.72, 0.66, 0.70, 0.61, 0.65, 0.58])
    for series, color in zip(values, colors):
        points = []
        for index, value in enumerate(series):
            x = x1 + 8 + index * (x2 - x1 - 16) / (len(series) - 1)
            y = y1 + 8 + value * (y2 - y1 - 16)
            points.append((round(x), round(y)))
        draw.line(points, fill=color, width=2)


def draw_body(draw: ImageDraw.ImageDraw, page: str) -> None:
    if page == "rpi":
        labels = (("CPU", "13%"), ("RAM", "34%"), ("TEMP", "57 C"), ("DISCO", "42%"))
        for i, (label, value) in enumerate(labels):
            metric(draw, (9 + i * 76, 43, 79 + i * 76, 96), label, value)
        card(draw, (9, 102, 158, 207)); card(draw, (164, 102, 311, 207))
        text(draw, (17, 110), "RED", 8, MUTED); text(draw, (17, 130), "^ 965 KB/s", 11, GREEN)
        text(draw, (17, 153), "v 11.8 MB/s", 11, CYAN)
        text(draw, (172, 110), "SERVICIOS", 8, MUTED); text(draw, (172, 133), "Docker", 9)
        text(draw, (302, 133), "5 / 5", 9, GREEN, "ra"); text(draw, (172, 157), "Puertos", 9)
        text(draw, (302, 157), "8", 9, CYAN, "ra")
    elif page in ("performance", "pve_performance"):
        metric(draw, (9, 43, 157, 94), "CPU", "13.0%")
        metric(draw, (163, 43, 311, 94), "MEMORIA RAM", "34.0%", GREEN)
        text(draw, (14, 105), "HISTORICO", 8, MUTED)
        chart(draw, (9, 118, 311, 207))
    elif page == "storage":
        metric(draw, (9, 43, 104, 94), "LECTURA", "11.9 MB/s")
        metric(draw, (110, 43, 205, 94), "ESCRITURA", "814 KB/s", GREEN)
        metric(draw, (211, 43, 311, 94), "RAIZ LIBRE", "32.6 GB", AMBER)
        card(draw, (9, 102, 311, 207)); text(draw, (16, 109), "DISPOSITIVOS 3", 8, MUTED)
        row(draw, 128, "nvme0n1", "391 GB / 931 GB", "42%")
        row(draw, 149, "mmcblk0", "59 GB / 64 GB", "8%")
        row(draw, 170, "sda", "LECTURA NO DISP.", "ERROR", RED)
    elif page == "network":
        metric(draw, (9, 43, 157, 94), "SUBIDA", "965 KB/s", GREEN)
        metric(draw, (163, 43, 311, 94), "DESCARGA", "11.8 MB/s")
        card(draw, (9, 102, 311, 207)); text(draw, (16, 109), "INTERFACES", 8, MUTED)
        row(draw, 130, "Ethernet / eth0", "192.0.2.10", "1 Gb/s")
        row(draw, 153, "VPN / tailscale0", "198.51.100.20", "ACTIVA")
        row(draw, 176, "Docker / dashboard", "172.20.0.1", "ACTIVA")
    elif page == "cpu":
        for i, (label, value, color) in enumerate((("CPU", "13%", CYAN), ("TEMP", "57.3 C", GREEN),
                                                   ("MHz", "2400", CYAN), ("CARGA", "0.75", AMBER))):
            metric(draw, (9 + i * 76, 43, 79 + i * 76, 94), label, value, color)
        card(draw, (9, 102, 158, 207)); card(draw, (164, 102, 311, 207))
        text(draw, (17, 109), "NUCLEOS", 8, MUTED); text(draw, (172, 109), "SONDAS", 8, MUTED)
        for i, value in enumerate((12, 7, 33, 10)):
            y = 130 + i * 17; text(draw, (17, y), f"CPU{i}", 7, MUTED)
            draw.rectangle((55, y + 2, 130, y + 7), fill="#071017"); draw.rectangle((55, y + 2, 55 + value, y + 7), fill=GREEN)
            text(draw, (146, y), f"{value}%", 7, TEXT, "ra")
        row(draw, 130, "CPU", "", "57.3 C"); row(draw, 153, "Chip RP1", "", "57.2 C")
    elif page == "ports":
        card(draw, (9, 43, 311, 207)); text(draw, (16, 51), "PUERTO        SERVICIO              ACCESO", 7, MUTED)
        for y, values in zip((76, 99, 122, 145, 168), (("22/tcp", "SSH", "LAN"), ("53/udp", "DNS", "LAN"),
            ("80/tcp", "reverse-proxy", "TODAS"), ("443/tcp", "reverse-proxy", "TODAS"), ("9443/tcp", "containers", "LAN"))):
            row(draw, y, *values, CYAN if y < 120 else GREEN)
    elif page == "docker":
        metric(draw, (9, 43, 104, 94), "ESTADO", "OK", GREEN)
        metric(draw, (110, 43, 205, 94), "ACTIVOS", "5", GREEN)
        metric(draw, (211, 43, 311, 94), "TOTAL", "5")
        card(draw, (9, 102, 311, 207)); text(draw, (16, 109), "CONTENEDORES", 8, MUTED)
        for y, name in zip((130, 147, 164, 181), ("dashboard", "dns-filter", "reverse-proxy", "uptime-monitor")):
            row(draw, y, name, "", "ACTIVO")
    elif page == "pve":
        metric(draw, (9, 43, 157, 94), "API", "ONLINE", GREEN)
        metric(draw, (163, 43, 311, 94), "UPTIME", "10d 4h", CYAN)
        card(draw, (9, 102, 311, 207)); text(draw, (16, 109), "NODOS", 8, MUTED)
        row(draw, 132, "pve-node", "CPU 18%", "ONLINE")
        text(draw, (18, 160), "VM / LXC", 8, MUTED); text(draw, (18, 181), "2 activas de 3", 13, CYAN)
        text(draw, (174, 160), "MEMORIA", 8, MUTED); text(draw, (174, 181), "42%", 13, GREEN)
    elif page == "guests":
        card(draw, (9, 43, 311, 207)); text(draw, (16, 51), "VM / LXC 3", 8, MUTED)
        row(draw, 76, "100  firewall", "VM  CPU 12%", "ACTIVA")
        row(draw, 103, "101  home-lab", "VM  RAM 54%", "ACTIVA")
        row(draw, 130, "102  laboratorio", "LXC", "PARADA", AMBER)
    elif page == "pve_storage":
        metric(draw, (9, 43, 157, 94), "TOTAL", "600 GB", CYAN)
        metric(draw, (163, 43, 311, 94), "LIBRE", "261 GB", GREEN)
        card(draw, (9, 102, 311, 207)); text(draw, (16, 109), "ALMACENAMIENTO", 8, MUTED)
        row(draw, 132, "local", "34 GB / 100 GB", "34%")
        row(draw, 159, "local-lvm", "305 GB / 500 GB", "61%", AMBER)
    elif page == "tasks":
        card(draw, (9, 43, 311, 207)); text(draw, (16, 51), "ULTIMAS TAREAS", 8, MUTED)
        row(draw, 76, "Copia de seguridad", "27/09 02:10", "OK")
        row(draw, 103, "Actualizar paquetes", "26/09 18:15", "ERROR", RED)
        row(draw, 130, "Consola del nodo", "26/09 19:42", "CANCELADA", AMBER)
        row(draw, 157, "Iniciar VM", "27/09 12:01", "EN CURSO", CYAN)
    elif page == "diagnostics":
        items = (("WIFI", "OK", GREEN), ("API", "OK", GREEN), ("MEMORIA", "198 KB", CYAN),
                 ("RESPUESTA", "42 ms", CYAN), ("DATOS", "ACTUALES", GREEN), ("VERSION", "1.0", AMBER))
        for i, (label, value, color) in enumerate(items):
            col, line = i % 2, i // 2
            metric(draw, (9 + col * 154, 43 + line * 55, 157 + col * 154, 94 + line * 55), label, value, color)
    elif page == "brightness":
        card(draw, (22, 56, 298, 192)); text(draw, (160, 76), "BRILLO", 9, MUTED, "ma")
        text(draw, (160, 116), "40%", 31, CYAN, "mm")
        draw.rounded_rectangle((72, 151, 248, 161), radius=5, fill="#071017")
        draw.rounded_rectangle((72, 151, 143, 161), radius=5, fill=CYAN)
        text(draw, (50, 156), "−", 24, TEXT, "mm"); text(draw, (270, 156), "+", 22, TEXT, "mm")
    elif page == "themes":
        card(draw, (9, 43, 311, 207)); text(draw, (16, 51), "TEMAS", 8, MUTED)
        choices = (("ORIGINAL", CYAN), ("GRAFITO", AMBER), ("BOSQUE", GREEN), ("CARMESI", RED))
        for y, (name, color) in zip((76, 105, 134, 163), choices):
            draw.rounded_rectangle((18, y, 44, y + 18), radius=3, fill=color)
            text(draw, (54, y + 2), name, 9)
            if y == 76:
                text(draw, (296, y + 3), "ACTIVO", 7, color, "ra")
    elif page == "splash":
        card(draw, (9, 43, 311, 207)); text(draw, (16, 51), "ANIMACIONES", 8, MUTED)
        for y, (name, color) in zip((75, 104, 133, 162), (("RASPBERRY", RED), ("TERMINAL", GREEN),
                                                              ("CYBER", CYAN), ("PERSONAL", AMBER))):
            draw.ellipse((19, y, 37, y + 18), outline=color, width=3)
            text(draw, (50, y + 2), name, 9)
            if y == 75:
                text(draw, (296, y + 3), "ACTIVA", 7, color, "ra")
    elif page == "orientation":
        card(draw, (9, 43, 311, 207)); text(draw, (16, 51), "GIRAR PANTALLA", 8, MUTED)
        labels = (("0°", (28, 78, 82, 158)), ("90°", (96, 91, 174, 145)),
                  ("180°", (190, 78, 244, 158)), ("270°", (232, 91, 310, 145)))
        for label, box in labels:
            draw.rounded_rectangle(box, radius=4, fill="#071017", outline=CYAN if label == "90°" else LINE, width=2)
            text(draw, ((box[0] + box[2]) // 2, box[3] + 13), label, 8, CYAN if label == "90°" else MUTED, "mm")


def render_panel(page: str, number: int, total: int) -> Image.Image:
    image = Image.new("RGB", (320, 240), BG)
    draw = ImageDraw.Draw(image)
    title = TITLES[page]
    text(draw, (10, 5), title, 17)
    draw.ellipse((257, 10, 263, 16), fill=GREEN)
    text(draw, (269, 7), "ONLINE", 8, GREEN)
    subtitle = "192.0.2.10" if not page.startswith("pve") and page not in ("guests", "tasks") else "PROXMOX VE"
    text(draw, (10, 27), subtitle, 7, MUTED)
    draw.line((7, 38, 313, 38), fill=LINE)
    draw_body(draw, page)
    draw.rectangle((0, 215, 319, 239), fill="#071017")
    draw.line((0, 215, 319, 215), fill=LINE)
    text(draw, (26, 227), "‹", 19, CYAN, "mm")
    text(draw, (294, 227), "›", 19, CYAN, "mm")
    text(draw, (176, 228), f"{number} / {total}", 8, TEXT, "mm")
    for x, y in ((139, 223), (147, 223), (139, 231), (147, 231)):
        draw.rectangle((x, y, x + 3, y + 3), fill=MUTED)
    return image


def pages_for(edition: str) -> list[str]:
    if edition == "rpi5":
        return PAGES["rpi5"] + PAGES["settings"]
    if edition == "proxmox":
        return PAGES["proxmox"] + PAGES["settings"]
    return PAGES["rpi5"] + PAGES["proxmox"] + PAGES["settings"]


def render(edition: str) -> Image.Image:
    pages = pages_for(edition)
    columns = 4
    rows = (len(pages) + columns - 1) // columns
    width = 36 + columns * 320 + (columns - 1) * 18
    height = 116 + rows * 240 + (rows - 1) * 28 + 34
    image = Image.new("RGB", (width, height), "#03070a")
    draw = ImageDraw.Draw(image)
    label = {"combined": "RPi5 + PROXMOX", "rpi5": "RASPBERRY PI 5", "proxmox": "PROXMOX VE"}[edition]
    text(draw, (36, 28), "CYD MONITOR", 30)
    text(draw, (36, 68), f"EDICION {label}  ·  VERSION 1.0", 13, CYAN)
    text(draw, (width - 36, 69), f"{len(pages)} PANELES", 12, GREEN, "ra")
    for index, page in enumerate(pages):
        x = 36 + (index % columns) * 338
        y = 116 + (index // columns) * 268
        panel = render_panel(page, index + 1, len(pages))
        image.paste(panel, (x, y))
        draw.rounded_rectangle((x - 1, y - 1, x + 320, y + 240), radius=3, outline="#34505f", width=1)
    return image


def encoded_png(image: Image.Image) -> bytes:
    output = io.BytesIO()
    image.save(output, format="PNG", optimize=True)
    return output.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--edition", choices=("combined", "rpi5", "proxmox"))
    parser.add_argument("--check", action="store_true", help="Comprueba que la imagen publicada está actualizada")
    args = parser.parse_args()
    edition = args.edition or (ROOT / "EDITION").read_text(encoding="ascii").strip()
    content = encoded_png(render(edition))
    if args.check:
        if not OUTPUT.exists():
            raise SystemExit("docs/panels.png no corresponde a esta edición; ejecuta build_gallery.py")
        expected = Image.open(io.BytesIO(content)).convert("RGB")
        published = Image.open(OUTPUT).convert("RGB")
        if published.size != expected.size:
            raise SystemExit("docs/panels.png tiene dimensiones incorrectas; ejecuta build_gallery.py")
        difference = ImageChops.difference(published, expected)
        average_error = sum(ImageStat.Stat(difference).mean) / 3
        if average_error > 0.75:
            raise SystemExit(
                f"docs/panels.png no corresponde a esta edición (diferencia {average_error:.2f}); "
                "ejecuta build_gallery.py"
            )
        print(f"Galería {edition} verificada: {OUTPUT}")
        return
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(content)
    print(f"Galería {edition} creada: {OUTPUT}")


if __name__ == "__main__":
    main()
