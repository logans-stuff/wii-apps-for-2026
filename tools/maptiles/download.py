#!/usr/bin/env python3
"""Download OpenStreetMap tiles for the Meshtoad offline map.

  python3 download.py <lat> <lon> [zooms] [radius]

Writes tiles/<z>/<x>/<y>.png around the center. Copy the tiles/ folder to
sd:/apps/meshtoad/ on the Wii's SD card. radius = tiles each way per zoom.

Be gentle with the OSM tile server (this throttles and sends a UA). For a
small personal area this is fine; don't bulk-download large regions.
"""
import math
import os
import sys
import time
import urllib.request

UA = "Meshtoad-Wii/0.1 (personal offline map; a few tiles)"


def deg2tile(lat, lon, z):
    n = 2 ** z
    x = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(lat)
    y = int((1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n)
    return x, y


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    lat = float(sys.argv[1])
    lon = float(sys.argv[2])
    zooms = [int(z) for z in sys.argv[3].split(",")] if len(sys.argv) > 3 else [11, 12, 13]
    radius = int(sys.argv[4]) if len(sys.argv) > 4 else 3

    total = got = 0
    for z in zooms:
        cx, cy = deg2tile(lat, lon, z)
        for dx in range(-radius, radius + 1):
            for dy in range(-radius, radius + 1):
                x, y = cx + dx, cy + dy
                if x < 0 or y < 0 or x >= 2 ** z or y >= 2 ** z:
                    continue
                total += 1
                path = f"tiles/{z}/{x}/{y}.png"
                if os.path.exists(path):
                    got += 1
                    continue
                os.makedirs(os.path.dirname(path), exist_ok=True)
                url = f"https://tile.openstreetmap.org/{z}/{x}/{y}.png"
                try:
                    req = urllib.request.Request(url, headers={"User-Agent": UA})
                    data = urllib.request.urlopen(req, timeout=20).read()
                    open(path, "wb").write(data)
                    got += 1
                    print(f"  {path} ({len(data)} B)")
                except Exception as e:
                    print(f"  FAILED {path}: {e}")
                time.sleep(0.12)
    size = sum(os.path.getsize(os.path.join(dp, f))
               for dp, _, fs in os.walk("tiles") for f in fs)
    print(f"\n{got}/{total} tiles, {size/1024:.0f} KB total in ./tiles")


if __name__ == "__main__":
    sys.exit(main())
