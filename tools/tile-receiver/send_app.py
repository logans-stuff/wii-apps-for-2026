#!/usr/bin/env python3
"""Serve the Meshtoad app files once to the Wii repair receiver."""

import argparse
import socket
import struct
import zlib
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("--port", type=int, default=4312)
    args = parser.parse_args()
    names = ("boot.dol", "meta.xml", "icon.png")
    files = [(name, args.app / name) for name in names]
    if any(not path.is_file() for _, path in files):
        parser.error("app directory is incomplete")

    with socket.create_server(("0.0.0.0", args.port), reuse_port=True) as server:
        print(f"waiting on port {args.port}", flush=True)
        conn, addr = server.accept()
        with conn:
            print(f"Wii connected from {addr[0]}", flush=True)
            conn.sendall(b"MTIL" + struct.pack(">I", len(files)))
            for index, (name, path) in enumerate(files, 1):
                data = path.read_bytes()
                if name == "boot.dol":
                    rel = b"boot.dol.z"
                    data = struct.pack(">I", len(data)) + zlib.compress(data, 9)
                else:
                    rel = name.encode()
                conn.sendall(struct.pack(">HI", len(rel), len(data)) + rel + data)
                if conn.recv(1) != b"\x01":
                    raise ConnectionError(f"Wii did not acknowledge {name}")
                print(f"sent {index}/{len(files)} {name} ({len(data)} bytes)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
