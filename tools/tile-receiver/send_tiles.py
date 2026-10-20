#!/usr/bin/env python3
"""Serve the local Meshtoad tile tree once to the Wii receiver."""

import argparse
import socket
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tiles", type=Path)
    parser.add_argument("--port", type=int, default=4312)
    parser.add_argument("--start", type=int, default=1,
                        help="one-based first tile to send")
    parser.add_argument("--limit", type=int,
                        help="maximum number of tiles to send")
    parser.add_argument("--suffix", default=".png",
                        help="tile filename suffix to send")
    args = parser.parse_args()
    files = sorted(args.tiles.rglob(f"*{args.suffix}"))
    if not files:
        parser.error(f"no {args.suffix} tiles found")
    if args.start < 1 or args.start > len(files):
        parser.error("--start is outside the tile set")
    files = files[args.start - 1:]
    if args.limit is not None:
        files = files[:args.limit]

    with socket.create_server(("0.0.0.0", args.port), reuse_port=True) as server:
        print(f"waiting on port {args.port} to send {len(files)} tiles", flush=True)
        conn, addr = server.accept()
        with conn:
            print(f"Wii connected from {addr[0]}", flush=True)
            conn.sendall(b"MTIL" + struct.pack(">I", len(files)))
            for index, path in enumerate(files, 1):
                rel = f"tiles/{path.relative_to(args.tiles).as_posix()}".encode()
                data = path.read_bytes()
                conn.sendall(struct.pack(">HI", len(rel), len(data)) + rel + data)
                if conn.recv(1) != b"\x01":
                    raise ConnectionError(f"Wii did not acknowledge {rel.decode()}")
                print(f"sent {index}/{len(files)} {rel.decode()} ({len(data)} bytes)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
