import struct, sys

KNOWN = {
    0x350EACF0: ("Macintosh LC", 0x40A00000),
    0x4D1EEEE1: ("Mac Plus v1", 0x400000),
    0x4D1EEAE1: ("Mac Plus v2", 0x400000),
    0x4D1F8172: ("Mac Plus v3", 0x400000),
    0xB2E362A8: ("Mac SE", 0x400000),
}

def analyze(label, data):
    cksum, reset_pc = struct.unpack(">II", data[0:8])
    name, base = KNOWN.get(cksum, ("UNKNOWN", None))
    print(f"[{label}] size {len(data)//1024} KB  checksum ${cksum:08X} -> {name}  reset PC ${reset_pc:08X}")
    if base is not None:
        print(f"  expected base ${base:08X}; reset PC offset = ${reset_pc - base:X}")
    calc = sum(struct.unpack(f">{(len(data)-4)//2}H", data[4:])) & 0xFFFFFFFF
    print(f"  calc sum ${calc:08X} {'OK' if calc == cksum else 'MISMATCH'}")
    for pat, lab in [(b"\x4E\xF9\x00\x60\x00\x00", "JMP.L $600000"),
                     (b"\x4E\xFA\x00\x60\x00\x00", "JSR.L $600000"),
                     (b"\x00\x60\x00\x00", "long $00600000")]:
        hits = [i for i in range(len(data)) if data.startswith(pat, i)]
        print(f"  {lab}: {len(hits)} hit(s) " + ", ".join(f"${h:X}" for h in hits[:10]))

def variants(data):
    n = len(data)
    yield "as-is", data
    yield "16-bit swap", bytes(data[i ^ 1] for i in range(n))
    q = n // 4
    yield "4-chip merge", bytes(data[(i % 4) * q + i // 4] for i in range(n))
    yield "4-chip split", bytes(data[(i // 4) + (i % 4) * q] for i in range(n))
    h = n // 2
    yield "2-chip merge", bytes(data[(i % 2) * h + i // 2] for i in range(n))

for path in sys.argv[1:]:
    data = open(path, "rb").read()
    print(f"=== {path} ===")
    analyze("as-is", data)
    for label, v in variants(data):
        if label == "as-is":
            continue
        ck = struct.unpack(">I", v[0:4])[0]
        calc = sum(struct.unpack(f">{(len(v)-4)//2}H", v[4:])) & 0xFFFFFFFF
        tag = KNOWN.get(ck, ("UNKNOWN", None))[0]
        print(f"  variant {label}: checksum ${ck:08X} ({tag})  calc ${calc:08X} {'OK' if calc == ck else 'MISMATCH'}")
