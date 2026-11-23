"""Blank the source path, user and machine name in Skyrim .pex headers."""
import struct
import sys


def scrub(path):
    data = bytearray(open(path, "rb").read())
    pos = 16  # magic, version, game id, compile time
    out = data[:pos]
    for value in (lambda s: s.rsplit("\\", 1)[-1], lambda s: "", lambda s: ""):
        n = struct.unpack(">H", data[pos:pos + 2])[0]
        text = data[pos + 2:pos + 2 + n].decode("latin-1")
        new = value(text).encode("latin-1")
        out += struct.pack(">H", len(new)) + new
        pos += 2 + n
    out += data[pos:]
    open(path, "wb").write(out)


for p in sys.argv[1:]:
    scrub(p)
