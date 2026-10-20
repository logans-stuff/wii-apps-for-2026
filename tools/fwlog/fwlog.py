"""Print the Wii firmware's streamed log (a long TCP connection on :4302).

    docker run -d --name wii-fwlog --network host -v $PWD/tools/fwlog:/t \
        python:3-alpine python -u /t/fwlog.py
    docker logs -f wii-fwlog

One thread per connection: a Wii that exits without closing its socket would
otherwise block every later run's connection.
"""
import socket
import threading
import time


def stamp():
    return time.strftime("%H:%M:%S")


def serve(c, addr):
    c.settimeout(600)
    print(stamp(), "--- connected", addr[0], flush=True)
    buf = b""
    while True:
        try:
            data = c.recv(4096)
        except OSError:
            data = b""
        if not data:
            break
        buf += data
        *lines, buf = buf.split(b"\n")
        for line in lines:
            print(stamp(), line.decode("utf-8", "replace").rstrip("\r"), flush=True)
    if buf:
        print(stamp(), buf.decode("utf-8", "replace"), flush=True)
    c.close()
    print(stamp(), "--- disconnected", addr[0], flush=True)


s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 4302))
s.listen(8)
while True:
    conn, addr = s.accept()
    threading.Thread(target=serve, args=(conn, addr), daemon=True).start()
