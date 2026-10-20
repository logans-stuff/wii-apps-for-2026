"""Print the Wii firmware's streamed log (one long TCP connection on :4302).

    docker run -d --name wii-fwlog --network host -v $PWD/tools/fwlog:/t \
        python:3-alpine python -u /t/fwlog.py
    docker logs -f wii-fwlog
"""
import socket
import time

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 4302))
s.listen(2)
while True:
    c, addr = s.accept()
    print(time.strftime("%H:%M:%S"), "--- connected", addr[0], flush=True)
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
            print(time.strftime("%H:%M:%S"), line.decode("utf-8", "replace").rstrip("\r"), flush=True)
    c.close()
    print(time.strftime("%H:%M:%S"), "--- disconnected", flush=True)
