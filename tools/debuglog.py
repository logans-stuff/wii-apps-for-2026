"""Print debug lines that the Wii app sends to TCP port 4301."""
import socket
import time

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 4301))
s.listen(8)
while True:
    c, addr = s.accept()
    c.settimeout(3)
    try:
        data = c.recv(2048)
    except OSError:
        data = b""
    c.close()
    print(time.strftime("%H:%M:%S"), addr[0], data.decode("ascii", "replace").rstrip(), flush=True)
