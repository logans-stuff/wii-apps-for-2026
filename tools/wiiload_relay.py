#!/usr/bin/env python3
"""One-shot wiiload TCP relay using small segments for the Wii's old Wi-Fi."""

import socket
import sys
import threading

if len(sys.argv) != 2:
    sys.exit(f"usage: {sys.argv[0]} <wii-ip>   (then WIILOAD=tcp:127.0.0.1 wiiload ...)")
LISTEN = ("127.0.0.1", 4299)
WII = (sys.argv[1], 4299)


def copy(source: socket.socket, target: socket.socket) -> None:
    try:
        while data := source.recv(4096):
            target.sendall(data)
    except (ConnectionError, OSError):
        pass
    finally:
        try:
            target.shutdown(socket.SHUT_WR)
        except OSError:
            pass


with socket.create_server(LISTEN) as listener:
    print(f"waiting for wiiload on {LISTEN[0]}:{LISTEN[1]}", flush=True)
    local, _ = listener.accept()
    with local, socket.socket() as remote:
        remote.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 536)
        remote.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        remote.connect(WII)
        print("connected to Wii with 536-byte TCP segments", flush=True)
        upstream = threading.Thread(target=copy, args=(local, remote))
        downstream = threading.Thread(target=copy, args=(remote, local))
        upstream.start()
        downstream.start()
        upstream.join()
        # wiiload closes its local side once the complete request has been
        # delivered.  Give HBC a moment to answer, then close our remote side
        # instead of waiting forever when old HBC leaves the socket open.
        downstream.join(timeout=2.0)
