"""Capture actual ESP32-rendered frames. Build with SPLASH_CAPTURE_ENABLED=1 first.

Run with the PlatformIO Python environment (pyserial), then restore the flag to 0.
No credentials, preferences or remote API data are read by this diagnostic.
"""
import argparse
import collections
import json
from pathlib import Path
import struct
import time
import zlib

import serial


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def read_exact(port, size):
    data = bytearray()
    while len(data) < size:
        block = port.read(size - len(data))
        if not block:
            raise TimeoutError(f'Incomplete frame ({len(data)}/{size})')
        data.extend(block)
    return data


def capture(port, design, timestamp, width, theme):
    return capture_command(port, f'F {design} {timestamp} {width} {theme}')


def device_info(port):
    port.write(b'I\n')
    return json.loads(port.readline())


def capture_command(port, command):
    port.write((command + '\n').encode())
    deadline = time.monotonic() + 20
    messages = []
    while time.monotonic() < deadline:
        header = port.readline().decode(errors='replace').strip()
        if header:
            messages.append(header)
        if header.startswith('FRAME '):
            _, w, h = header.split()
            w, h = int(w), int(h)
            break
        if header.startswith('ERROR'):
            raise RuntimeError(header)
    else:
        raise TimeoutError(f'No framebuffer received for {command}: {messages}')
    colors = []
    while len(colors) < w * h:
        count, color = struct.unpack('<HH', read_exact(port, 4))
        if not count or count > w * h - len(colors):
            raise ValueError('Invalid RLE run')
        colors.extend([color] * count)
    if port.readline().strip() or port.readline().strip() != b'END':
        raise ValueError('Missing frame terminator')
    rgb = bytearray()
    for color in colors:
        rgb.extend((((color >> 11) & 31) * 255 // 31, ((color >> 5) & 63) * 255 // 63, (color & 31) * 255 // 31))
    return w, h, colors, rgb


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--output', type=Path, default=Path('firmware/.pio/splash-captures'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with serial.Serial(args.port, 460800, timeout=8) as port:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if b'CAPTURE READY' in port.readline():
                break
        else:
            raise TimeoutError('Flash the capture build first')
        info = device_info(port)
        landscape = []
        reference = {}
        for width in (320, 240):
            for design in range(info['splashes']):
                frames = []
                for timestamp in (800, 2300):
                    w, h, colors, pixels = capture(port, design, timestamp, width, 0)
                    for band in (colors[:w*(h//5)], colors[w*(h-h//5):]):
                        assert len(set(band)) > 3, f'Empty full-screen margin: {design}/{width}'
                    png(args.output / f'{design}-{width}-{timestamp}.png', w, h, pixels)
                    background_count = collections.Counter(colors).most_common(1)[0][1]
                    assert w * h - background_count > 1800, f'Blank scene {design}/{width}'
                    frames.append(colors)
                    if width == 320 and timestamp == 2300:
                        landscape.append(pixels)
                    if timestamp == 2300:
                        reference[(design, width)] = colors
                # Compare only artwork, excluding text/status, so a changing label cannot pass.
                start, end = 0, (h-30)*w
                changed = sum(a != b for a, b in zip(frames[0][start:end], frames[1][start:end]))
                assert changed > 150, f'Animation is static: {design}/{width}: {changed}'
                print(f'design={design} viewport={w}x{h}: {changed} changed artwork pixels', flush=True)
        for width in (320, 240):
            for design in range(info['splashes']):
                for theme in range(info['themes']):
                    _, _, colors, _ = capture(port, design, 2300, width, theme)
                    assert colors == reference[(design, width)], f'Theme leaked into splash: {design}/{width}/{theme}'
                print(f'design={design} width={width}: identical across all {info["themes"]} dashboard themes', flush=True)
        for rotation in range(4):
            width=320 if rotation%2 else 240
            for design in range(info['splashes']):
                _, _, colors, _ = capture_command(port,f'F {design} 2300 {width} 0 {rotation} 40')
                assert colors == reference[(design,width)], f'Rotation changed frame: {design}/{rotation}'
                _, _, wide_strip, _ = capture_command(port,f'F {design} 2300 {width} 0 {rotation} 80')
                assert colors == wide_strip, f'Strip seam: {design}/{rotation}'
            print(f'All animations match at {rotation*90} degrees, with both strip sizes.',flush=True)
        for width in (320,240):
            for design in range(info['splashes']):
                for stage in range(6):
                    w,h,_,pixels=capture_command(port,f'F {design} 2300 {width} 0 255 40 {stage}')
                    png(args.output/f'stage-{design}-{width}-{stage}.png',w,h,pixels)
        montage = bytearray()
        for row in range(240):
            for pixels in landscape:
                montage.extend(pixels[row * 320 * 3:(row + 1) * 320 * 3])
        png(args.output / 'all-designs.png', 320 * info['splashes'], 240, montage)
        print(f'All {info["splashes"]} scenes passed in landscape and portrait; palettes are theme-independent.', flush=True)


if __name__ == '__main__':
    main()
