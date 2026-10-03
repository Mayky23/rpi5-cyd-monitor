"""USB rendering regression test; requires SPLASH_CAPTURE_ENABLED=1.

Fixtures resemble a Pi with Ethernet, VPN, Docker networks and five containers.
No preferences are saved. Always restore the normal firmware after this test.
"""
import argparse
import copy
import json
from pathlib import Path
import time

import serial
from check_splashes import capture_command, device_info, png

# Page ids of firmware/src/main.cpp (enum Page). Each edition shows a subset; the
# capture protocol addresses pages by their position, so ids are mapped per board.
TOTAL, OVERVIEW, STORAGE, NETWORK, CPU, PORTS, DOCKER = range(7)
PROXMOX, PVE_PERFORMANCE, PVE_GUESTS, PVE_STORAGE, PVE_TASKS = range(7, 12)
DIAG, BRIGHTNESS, CUSTOMIZE, SPLASH, ORIENTATION = range(12, 17)
DATA_PAGES = tuple(range(TOTAL, DIAG))
PAGED_LISTS = (STORAGE, NETWORK, PORTS, PVE_GUESTS, PVE_STORAGE, PVE_TASKS, CUSTOMIZE, SPLASH)


def page_positions(info):
    """Map page id -> position on this board (older firmware reports only the count)."""
    ids = info.get('page_ids')
    if ids is None:
        if info['pages'] != 17:
            raise RuntimeError('Firmware sin page_ids: carga el build de captura de esta versión')
        ids = list(range(17))
    if len(ids) != info['pages']:
        raise RuntimeError(f'page_ids incoherente: {ids} para {info["pages"]} paneles')
    return {page_id: position for position, page_id in enumerate(ids)}


def fixture():
    ports = []
    for number, service in ((22, 'sshd'), (53, 'pihole-FTL'), (80, 'docker-proxy'),
                            (81, 'docker-proxy'), (111, 'rpcbind'), (443, 'docker-proxy'),
                            (631, 'cupsd'), (3000, 'docker-proxy')):
        for protocol, address in (('tcp', '127.0.0.1' if number == 631 else '0.0.0.0'),
                                  ('tcp6', '::1' if number == 631 else '::')):
            ports.append(dict(port=number, service=service, protocol=protocol, address=address))
    interfaces = [dict(name=name, ipv4=ip, speed_mbps=speed, up=True)
                  for name, ip, speed in [('br-1a947a33ee86', '172.20.0.1', 10000),
                                           ('br-62ab7647fced', '172.19.0.1', 10000),
                                           ('br-a0fea5407a98', '172.18.0.1', 10000),
                                           ('docker0', '172.17.0.1', 10000),
                                           ('eth0', '192.0.2.10', 1000),
                                           ('tailscale0', '198.51.100.20', None),
                                           ('veth65174dd', '--', 10000),
                                           ('vethc79f3cb', '--', 10000)]]
    names = ['dashboard', 'dns-filter', 'reverse-proxy', 'containers', 'uptime-monitor']
    return dict(schema=2, system=dict(ip='192.0.2.10', uptime_s=936520),
                cpu=dict(percent=13, temperature_c=57.3, frequency_mhz=2400, load=[0.75],
                         per_core=[12, 7, 33, 10]), memory=dict(percent=34),
                disk=dict(percent=42, status='ok', free_bytes=35000000000,
                          io_status='ok', read_bps=12500000, write_bps=834000),
                storage=dict(mounts=[
                    dict(device='/dev/nvme0n1', mount='/', status='ok', percent=42,
                         total_bytes=1000000000000, used_bytes=420000000000),
                    dict(device='/dev/nvme0n1p1', mount='/boot', status='ok', percent=12,
                         total_bytes=500000000, used_bytes=60000000),
                    dict(device='/dev/sda', status='error', percent=None),
                    dict(device='/dev/mmcblk0', status='unmounted', size_bytes=64000000000)]),
                network=dict(rx_bps=12345678, tx_bps=987654),
                interfaces=interfaces, ports=ports,
                history=[dict(cpu=8 + i % 11, ram=32 + i % 3) for i in range(60)],
                temperatures=[dict(name='cpu_thermal:sensor 1', current_c=57.3),
                              dict(name='rp1_adc:sensor 1', current_c=57.2)],
                docker=dict(available=True, running=5, total=5,
                            containers=[dict(name=name, state='running') for name in names]),
                proxmox=dict(available=True, version='9.2.11', nodes_online=1, nodes_total=1,
                             guests_running=2, guests_total=3, tasks_failed=1,
                             tasks_cancelled=1, history_interval_ms=8000,
                             history=[dict(cpu=14 + i % 9, ram=39 + i % 5)
                                      for i in range(60)],
                             nodes=[dict(name='proxmox', status='online', cpu_percent=18.5,
                                         cpu_total=8, memory_percent=42.0, uptime_s=864000)],
                             guests=[
                                 dict(id=100, name='firewall', type='VM', status='running',
                                      cpu_percent=12, memory_percent=37, disk_percent=31),
                                 dict(id=101, name='home-assistant', type='VM', status='running',
                                      cpu_percent=21, memory_percent=54, disk_percent=48),
                                 dict(id=102, name='laboratorio', type='LXC', status='stopped',
                                      cpu_percent=0, memory_percent=0, disk_percent=22)],
                             storage=[dict(name='local', node='proxmox', status='available',
                                           percent=34, used_bytes=34000000000, total_bytes=100000000000),
                                      dict(name='local-lvm', node='proxmox', status='available',
                                           percent=61, used_bytes=305000000000, total_bytes=500000000000)],
                             tasks=[
                                 dict(label='Copia de seguridad', state='OK', started='27/09 02:10',
                                      detail='Completada correctamente'),
                                 dict(label='Consola del nodo', state='CANCELLED', started='26/09 19:42',
                                      detail='Consola cerrada por el usuario'),
                                 dict(label='Actualizar repositorios', state='ERROR', started='26/09 18:15',
                                      detail='No se pudo contactar con el repositorio'),
                                 dict(label='Iniciar VM', state='RUNNING', started='27/09 12:01',
                                      detail='En curso')]))


def send_json(port, data, command='J'):
    payload = (command + ' ' + json.dumps(data, separators=(',', ':')) + '\n').encode()
    for start in range(0, len(payload), 128):
        port.write(payload[start:start + 128])
        port.flush()
        time.sleep(0.005)
    reply = port.readline().strip()
    assert reply == b'JSON OK', f'Fixture rejected: {reply!r}'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--output', type=Path, default=Path('firmware/.pio/panel-captures'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with serial.Serial(args.port, 460800, timeout=20) as port:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if b'CAPTURE READY' in port.readline():
                break
        else:
            raise TimeoutError('Flash capture build first')
        info = device_info(port)
        positions = page_positions(info)
        data_pages = [positions[page] for page in DATA_PAGES if page in positions]
        port.write(b'T\n')
        result = port.readline().decode().strip()
        assert result.startswith('TEST OK'), result
        print(result, flush=True)
        data = fixture()
        send_json(port, data)
        count = 0
        def save(name, page, subpage=0, brightness=6, width=320, theme=0, mode=1, rotation=None):
            nonlocal count
            request = f'P {page} {subpage} {brightness} {width} {theme} {mode}'
            if rotation is not None:
                request += f' {rotation}'
            w, h, colors, pixels = capture_command(port, request)
            assert (w, h) == (width, 240 if width == 320 else 320)
            assert len(set(colors)) >= 8, f'Blank panel {page}'
            png(args.output / f'{name}-{width}.png', w, h, pixels)
            count += 1
            return colors
        for width in (320, 240):
            for page in range(info['pages']):
                save(f'page-{page}', page, width=width)
            for page in PAGED_LISTS:
                if page in positions:
                    save(f'page-{positions[page]}-next', positions[page], subpage=1, width=width)
            save('navigation', 0, width=width, mode=2)
            save('offline', 0, width=width, mode=0)
            for level in range(6):
                save(f'brightness-{level}', positions[BRIGHTNESS], brightness=level, width=width)
            for theme in range(info['themes']):
                save(f'theme-{theme}', 0, width=width, theme=theme)
                save(f'picker-{theme}', positions[CUSTOMIZE], subpage=theme // info['themes_per_page'],
                     width=width, theme=theme)
            for page in range(1, info['pages']):
                save(f'light-{page}', page, width=width, theme=9)
            port.write(b'T\n')
            result = port.readline().decode().strip()
            assert result.startswith('TEST OK'), result
            send_json(port, data)
            print(f'All panels, themes, navigation and brightness captured at width {width}.', flush=True)
        for rotation in range(4):
            width = 320 if rotation % 2 else 240
            for page in range(info['pages']):
                save(f'rotation-{rotation}-page-{page}', page, width=width, rotation=rotation)
            save(f'rotation-{rotation}-navigation', 0, width=width, rotation=rotation, mode=2)
            for page in (CUSTOMIZE, SPLASH):
                save(f'rotation-{rotation}-picker-next-{positions[page]}', positions[page], subpage=1,
                     width=width, rotation=rotation)
            print(f'All {info["pages"]} panels captured at {rotation * 90} degrees with hardware rotation.', flush=True)
        before = fixture()
        changed = copy.deepcopy(before)
        changed['cpu']['percent'] = 99
        changed['cpu']['per_core'] = [None, 0, 99, 7]
        changed['memory']['percent'] = 92
        changed['storage']['mounts'][0]['status'] = 'error'
        changed['storage']['mounts'][0]['percent'] = None
        changed['interfaces'][4]['up'] = False
        changed['docker']['containers'] = changed['docker']['containers'][:2]
        changed['docker']['total'] = 2
        changed['ports'] = []
        changed['history'] = [dict(cpu=97 + i % 3, ram=90 + i % 2) for i in range(60)]
        send_json(port, before, 'U')
        send_json(port, changed)
        for width in (320, 240):
            for page in data_pages:
                full = save(f'updated-{page}', page, width=width)
                partial = save(f'partial-{page}', page, width=width, mode=3)
                assert full == partial, f'Incremental redraw differs on page {page}, width {width}'
        print('Incremental redraw matches a full redraw on every data page, in both orientations.', flush=True)
        # Wider values exercise the intermediate bold sizes without tiny fallbacks.
        large = fixture()
        large['cpu'].update(percent=100, temperature_c=100.0, frequency_mhz=3000, load=[12.34])
        large['cpu']['per_core'] = [100, 100, 100, 100]
        large['memory']['percent'] = 100
        large['disk'].update(percent=100, read_bps=999 * 1024 ** 3, write_bps=123.4 * 1024 ** 2)
        large['network'].update(rx_bps=999 * 1024 ** 3, tx_bps=123.4 * 1024 ** 2)
        large['temperatures'][0]['current_c'] = 100.0
        send_json(port, large)
        for width in (320, 240):
            for page in (TOTAL, OVERVIEW, STORAGE, NETWORK, CPU, PROXMOX, PVE_PERFORMANCE):
                if page in positions:
                    save(f'wide-values-{positions[page]}', positions[page], width=width)
        # Overflow remains reachable instead of silently dropping rows.
        data = fixture()
        data['docker']['containers'] += [dict(name=f'backup-{i}', state='paused') for i in range(7)]
        data['docker']['total'] = 12
        send_json(port, data)
        if DOCKER in positions:
            for width in (320, 240):
                frames = [save(f'docker-overflow-{subpage}', positions[DOCKER], subpage=subpage, width=width)
                          for subpage in (0, 1)]
                assert frames[0] != frames[1], 'Docker pagination did not change the display'
        data['cpu']['percent'] = None
        data['cpu']['temperature_c'] = None
        data['memory']['percent'] = None
        data['network']['rx_bps'] = None
        data['disk'] = dict(status='error', io_status='error')
        data['docker'] = dict(available=False)
        data['proxmox'] = dict(available=False, state='offline', nodes=[], guests=[],
                               storage=[], tasks=[], history=[], tasks_failed=0,
                               tasks_cancelled=0)
        data['history'] = []
        send_json(port, data)
        for width in (320, 240):
            for page in data_pages:
                save(f'error-{page}', page, width=width)
        print(f'{count} panel captures OK; overflow pages and missing-data states verified.', flush=True)


if __name__ == '__main__':
    main()
