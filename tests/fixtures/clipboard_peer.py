#!/usr/bin/env python3
"""Minimal X11 clipboard data fixture; never launches or asserts a test."""
import base64, json, socketserver, struct, sys, threading
from pathlib import Path
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8"
    "/x8AAwMCAO+aAn8AAAAASUVORK5CYII=")
JPEG = b"\xff\xd8\xff\xe0\x00\x10JFIF\x00" + b"fixture" + b"\xff\xd9"


class Clipboard(socketserver.TCPServer):
    """Minimal X11 peer; exercises the public clipboard path, without Xorg."""

    def __init__(self):
        super().__init__(("127.0.0.1", 0), ClipboardPeer)
        self.image_type = "image/png"
        self.payload = PNG
        self.errors = []
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()

    @property
    def display(self):
        return f"127.0.0.1:{self.server_address[1] - 6000}"


class ClipboardPeer(socketserver.BaseRequestHandler):
    def read(self, length):
        result = bytearray()
        while len(result) < length:
            data = self.request.recv(length - len(result))
            if not data:
                raise EOFError
            result.extend(data)
        return bytes(result)

    def handle(self):
        try:
            self.request.settimeout(5)
            setup = self.read(12)
            if setup[0] != ord("l"): raise ValueError("unsupported X11 byte order")
            auth_name, auth_data = struct.unpack_from("<HH", setup, 6)
            self.read(((auth_name + 3) & ~3) + ((auth_data + 3) & ~3))
            setup_reply = bytearray(80)
            struct.pack_into("<BBHHH", setup_reply, 0, 1, 0, 11, 0, 18)
            struct.pack_into("<II", setup_reply, 12, 0x100000, 0xFFFFF)
            setup_reply[28] = 1  # one root, no pixmap formats/vendor
            struct.pack_into("<I", setup_reply, 40, 1)
            self.request.sendall(setup_reply)
            atoms = {}
            selected = 0
            sequence = 0
            while True:
                header = self.read(4)
                opcode = header[0]
                units = struct.unpack_from("<H", header, 2)[0]
                request = header + self.read(units * 4 - 4)
                sequence += 1
                reply = bytearray(32)
                struct.pack_into("<H", reply, 2, sequence)
                if opcode == 16:  # InternAtom
                    size = struct.unpack_from("<H", request, 4)[0]
                    name = request[8:8 + size].decode()
                    atom = atoms.setdefault(name, len(atoms) + 100)
                    reply[0] = 1
                    struct.pack_into("<I", reply, 8, atom)
                elif opcode == 18:  # ChangeProperty -> PropertyNotify timestamp
                    reply[0] = 28
                    reply[4:12] = request[4:12]
                    struct.pack_into("<I", reply, 12, 123)
                elif opcode == 24:  # ConvertSelection -> SelectionNotify
                    if (root / 'config.json').exists():
                        config = json.loads((root / 'config.json').read_text())
                        self.server.image_type = config.get('image_type', 'image/png')
                        self.server.payload = base64.b64decode(config['payload']) if 'payload' in config else PNG
                    window, selection, selected, prop, timestamp = (
                        struct.unpack_from("<IIIII", request, 4))
                    available = selected == atoms.get(self.server.image_type)
                    reply[0] = 31
                    struct.pack_into("<IIIII", reply, 4, timestamp, window,
                                     selection, selected, prop if available else 0)
                elif opcode == 20:  # GetProperty, single bounded image transfer
                    payload = self.server.payload
                    reply[0:2] = b"\x01\x08"
                    struct.pack_into("<I", reply, 4, (len(payload) + 3) // 4)
                    struct.pack_into("<I", reply, 8, selected)
                    struct.pack_into("<I", reply, 16, len(payload))
                    reply.extend(payload + b"\0" * (-len(payload) % 4))
                elif opcode in (1, 19, 4):  # Create/DeleteProperty/DestroyWindow
                    continue
                else:
                    raise ValueError(f"unexpected X11 opcode {opcode}")
                self.request.sendall(reply)
        except EOFError:
            pass
        except Exception as exc:
            self.server.errors.append(exc)
            print(str(exc), file=sys.stderr, flush=True)


root = Path(sys.argv[1])
server = Clipboard()
(root / 'display').write_text(server.display)
try:
    server.thread.join()
finally:
    server.server_close()
