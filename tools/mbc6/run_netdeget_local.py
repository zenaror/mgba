#!/usr/bin/env python3
"""Destructive fixture-only local Net de Get download and gameplay check."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading

ROM_SHA = '9fb1e6e4a637796b8624bd2de6c9abaa9e758546b620cb5dc8441b07c288bc63'
CATALOG_SHA = '8f072d41c39146380053abe0281835b4a639511a523cd5963bc67ef222aec043'
BODY_SHA = 'a8f6e181ddedf0f5d0b1b8e164d9e41edcddaadd14cf0c9f4730ede455560a24'

def digest(data):
    return hashlib.sha256(data).hexdigest()

def dns_loop(sock, stopped):
    sock.settimeout(0.2)
    while not stopped.is_set():
        try:
            query, peer = sock.recvfrom(2048)
        except socket.timeout:
            continue
        pos = 12
        while pos < len(query) and query[pos]:
            pos += query[pos] + 1
        end = pos + 5
        if end > len(query):
            continue
        reply = (query[:2] + b'\x81\x80' + query[4:6]
                 + b'\x00\x01\x00\x00\x00\x00' + query[12:end]
                 + b'\xc0\x0c\x00\x01\x00\x01'
                 + struct.pack('!IH', 60, 4) + b'\x7f\x00\x00\x01')
        sock.sendto(reply, peer)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('rom', type=Path)
parser.add_argument('build', type=Path, help='Linux shared mGBA build directory')
parser.add_argument('empty_save', type=Path, help='validated synthetic SRAM fixture')
parser.add_argument('empty_flash', type=Path, help='erased synthetic flash sidecar')
parser.add_argument('payload', type=Path, help='8 KiB D800 PAD TEST payload')
args = parser.parse_args()
assert digest(args.rom.read_bytes()) == ROM_SHA, 'unexpected host ROM'
sram = args.empty_save.read_bytes()
flash = args.empty_flash.read_bytes()
payload = args.payload.read_bytes()
assert len(sram) == 0x8000 and sram[0x4F2] == 0xFF, 'requires empty catalog fixture'
assert len(flash) == 0x100101 and flash[:0x100000] == b'\xFF' * 0x100000
assert digest(payload) == '0e42875ef2569905d056f895ab5d6998e4f17709875dd27f13cbd9b20c2158b0'
root = Path(tempfile.mkdtemp(prefix='mgba-netdeget-local-'))
(root / 'padtest.sav').write_bytes(sram)
(root / 'padtest.sav.flash').write_bytes(flash)
repo = Path(__file__).resolve().parents[2]
subprocess.run(['cc', '-O2', '-DENABLE_VFS', '-DENABLE_DIRECTORIES', '-DENABLE_INPUT',
                '-I' + str(args.build / 'include'), '-I' + str(repo / 'include'),
                '-I' + str(repo / 'src/third-party/libmobile'),
                str(Path(__file__).with_name('netdeget_local_trace.c')),
                '-L' + str(args.build), '-Wl,-rpath,' + str(args.build), '-lmgba',
                '-o', str(root / 'runner')], check=True)
# All controls below are ordinary joypad input; no CPU state is redirected.
macro = ['0:900', '8:3', '0:180', '1:3', '0:360', '1:3', '0:120',
         '16:3', '0:30', '128:3', '0:30', '1:3', '0:600', '1:3', '0:120']
col, row = 0, 0
for char in 'fixture':
    n = ord(char) - ord('a')
    target = (n % 15, 2 + n // 15)
    for key, count in [(16, max(0, target[0] - col)), (32, max(0, col - target[0])),
                       (128, max(0, target[1] - row)), (64, max(0, row - target[1]))]:
        for _ in range(count):
            macro += [f'{key}:3', '0:10']
    macro += ['1:3', '0:20']
    col, row = target
macro += ['8:3', '0:120', '1:3', '0:120', '128:3', '0:20', '128:3', '0:20',
          '1:3', '0:120', '64:3', '0:20', '1:3', '0:600', '1:3', '0:600']
macro += ['128:3', '0:20'] * 5 + ['1:3', '0:120']
# Re-download confirmation and zero-price confirmation.
macro += ['64:3', '0:20', '1:3', '0:600', '64:3', '0:20', '1:3', '0:2400']
macro += ['1:3', '0:300', '16:3', '0:60', '1:3', '0:900']
macro += ['1:3', '0:300', '1:3', '0:120', '1:3', '0:240', '16:3', '0:90',
          '1:3', '0:60', '1:3', '0:180']
for key in [1, 2, 8, 4, 16, 32, 64, 128]:
    macro += [f'{key}:3', '0:20']
macro += ['12:3', '0:300'] + ['128:3', '0:10'] * 16 + ['1:3', '0:60', '1:3', '0:180']
assert len(macro) == 181
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('127.0.0.1', 8053))
stopped = threading.Event()
thread = threading.Thread(target=dns_loop, args=(sock, stopped), daemon=True)
thread.start()
try:
    with (root / 'run.log').open('w') as output:
        subprocess.run([str(root / 'runner'), str(args.rom), str(root), *macro],
                       env={**os.environ, 'LOCAL_MOBILE': '1'}, stdout=output,
                       stderr=subprocess.STDOUT, check=True)
finally:
    stopped.set()
    thread.join()
    sock.close()
lines = (root / 'run.log').read_text().splitlines()
stages = {int(m.group(1)): line for line in lines if (m := re.match(r'stage=(\d+) ', line))}
for i, key in enumerate([1, 2, 8, 4, 16, 32, 64, 128]):
    assert f'held={key:02X}' in stages[127 + i * 2]
    assert 'held=00' in stages[128 + i * 2]
assert 'counters=0101010101010101' in stages[142]
assert 'A=20 B=0' in stages[144]
assert 'A=0 B=0' in stages[180] and 'counters=0000000000000000' in stages[180]
assert (root / 'padtest.sav.flash').read_bytes() == payload + flash[8192:]
assert (root / 'padtest.sav').read_bytes()[0x4F2:0x4F4] == b'\x10\x00'
responses = (root / 'tcp-recv.bin').read_bytes().split(b'HTTP/1.0 ')[1:]
bodies = [response.split(b'\r\n\r\n', 1)[1] for response in responses]
assert sum(digest(body) == CATALOG_SHA for body in bodies) == 2
assert sum(digest(body) == BODY_SHA for body in bodies) == 2
assert digest(args.rom.read_bytes()) == ROM_SHA
print('PASS natural download, flash write, eight inputs, exit and relaunch:', root)
# Reload the downloaded save in a new core, with no adapter attached.
reopened = root / 'reopened'
reopened.mkdir()
for name in ['padtest.sav', 'padtest.sav.flash']:
    (reopened / name).write_bytes((root / name).read_bytes())
offline_macro = ['0:900', '8:3', '0:180', '1:3', '0:360', '1:3', '0:60', '1:3', '0:240']
offline_macro += ['128:3', '0:10'] * 16 + ['1:3', '0:60', '1:3', '0:180',
                                             '1:3', '0:20', '12:3', '0:300']
offline_env = dict(os.environ)
offline_env.pop('LOCAL_MOBILE', None)
with (reopened / 'run.log').open('w') as output:
    subprocess.run([str(root / 'runner'), str(args.rom), str(reopened), *offline_macro],
                   env=offline_env, stdout=output, stderr=subprocess.STDOUT, check=True)
stages = {int(m.group(1)): line for line in (reopened / 'run.log').read_text().splitlines()
          if (m := re.match(r'stage=(\d+) ', line))}
assert 'A=0 B=0' in stages[44] and 'counters=0000000000000000' in stages[44]
assert 'held=01' in stages[45] and 'counters=0100000000000000' in stages[45]
assert 'held=00' in stages[46] and 'counters=0100000000000000' in stages[46]
assert 'A=20 B=0' in stages[48]
assert (reopened / 'padtest.sav.flash').read_bytes() == payload + flash[8192:]
print('PASS downloaded save fresh-core launch/input/exit, unchanged flash:', reopened)
