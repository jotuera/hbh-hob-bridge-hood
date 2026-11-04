#!/usr/bin/env python3
"""Print the remote events (esphome.hbh_remote) that Home Assistant receives from hbh-remote-rx.yaml.

    HA_URL=http://homeassistant.local:8123 HA_TOKEN=<long-lived token> python3 tools/listen_remote.py [seconds]

Create the token in HA: your profile → Security → Long-lived access tokens.
Standard library only; plain ws:// (for https use a local URL of HA).
"""
import base64
import json
import os
import socket
import struct
import sys
import time
from urllib.parse import urlparse

url = urlparse(os.environ.get('HA_URL', 'http://homeassistant.local:8123'))
token = os.environ.get('HA_TOKEN') or sys.exit('set HA_TOKEN (long-lived access token)')
duration = float(sys.argv[1]) if len(sys.argv) > 1 else 300

s = socket.create_connection((url.hostname, url.port or 8123), timeout=30)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f'GET /api/websocket HTTP/1.1\r\nHost: {url.hostname}\r\nUpgrade: websocket\r\n'
           f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
buf = b''
while b'\r\n\r\n' not in buf:
    buf += s.recv(4096)
buf = buf.split(b'\r\n\r\n', 1)[1]


def recv_exact(n):
    global buf
    while len(buf) < n:
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError('closed')
        buf += chunk
    d, buf = buf[:n], buf[n:]
    return d


def recv():
    data = b''
    while True:
        b1, b2 = recv_exact(2)
        ln = b2 & 0x7f
        if ln == 126:
            ln = struct.unpack('>H', recv_exact(2))[0]
        elif ln == 127:
            ln = struct.unpack('>Q', recv_exact(8))[0]
        data += recv_exact(ln)
        if b1 & 0x80:
            op = b1 & 0x0f
            if op == 9:                                   # ping -> pong
                s.sendall(bytes([0x8a, 0x80]) + os.urandom(4))
                return {}
            return json.loads(data) if op in (0, 1) else {}


def send(o):
    p = json.dumps(o).encode()
    h = bytes([0x81])
    if len(p) < 126:
        h += bytes([0x80 | len(p)])
    elif len(p) < 65536:
        h += bytes([0x80 | 126]) + struct.pack('>H', len(p))
    else:
        h += bytes([0x80 | 127]) + struct.pack('>Q', len(p))
    m = os.urandom(4)
    s.sendall(h + m + bytes(c ^ m[i % 4] for i, c in enumerate(p)))


recv()
send({'type': 'auth', 'access_token': token})
if recv().get('type') != 'auth_ok':
    sys.exit('authentication failed')
send({'id': 1, 'type': 'subscribe_events', 'event_type': 'esphome.hbh_remote'})
print(f'listening for {duration:.0f} s – press the remote keys', flush=True)
s.settimeout(None)
end = time.time() + duration
while time.time() < end:
    r = recv()
    if r.get('type') == 'event':
        d = r['event']['data']
        print(time.strftime('%H:%M:%S'), f"{d.get('key')}  frames={d.get('frames')}  held={d.get('held_ms')} ms", flush=True)
