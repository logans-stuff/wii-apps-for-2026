"""Write source/cacerts.h from a PEM bundle (default: the host's CA store)."""
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "/etc/ssl/certs/ca-certificates.crt"
pem = open(src, encoding="ascii").read()
blocks = [b for b in pem.split("-----END CERTIFICATE-----") if "BEGIN CERTIFICATE" in b]
out = ["/* Root certificates. Made by tools/mkcerts.py from %s. */" % src,
       "/* %d certificates. */" % len(blocks),
       "static const char CA_BUNDLE[] ="]
for b in blocks:
    b = b[b.index("-----BEGIN CERTIFICATE-----"):] + "-----END CERTIFICATE-----\n"
    for line in b.splitlines():
        out.append('    "%s\\n"' % line)
out[-1] += ";"
open(sys.argv[2] if len(sys.argv) > 2 else "source/cacerts.h", "w").write("\n".join(out) + "\n")
print(len(blocks), "certificates")
