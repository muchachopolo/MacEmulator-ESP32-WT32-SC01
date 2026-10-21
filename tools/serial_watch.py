import serial, re, time

s = serial.Serial("COM3", 115200, timeout=1)
pat = re.compile(
    r"SPEED|panic|GPF|bad ROM|not bootable|back in bootloader|FATAL|ABORT|exception|unimp|crash|PC-HIST|BADPC",
    re.I,
)

speed_n = 0
logf = open("E:/Personal/Macintosh/tools/serial_watch.log", "a", buffering=1)
empty = 0
while True:
    try:
        line = s.readline()
    except Exception as e:
        print(f"[serial] {e}", flush=True)
        time.sleep(1)
        continue
    if not line:
        empty += 1
        if empty == 100:
            print("[serial] no bytes coming from COM3 for ~10s", flush=True)
        if empty > 100:
            time.sleep(0.1)
        continue
    empty = 0
    text = line.decode("ascii", "replace").strip()
    if text:
        logf.write(text + "\n")
    if not text:
        continue
    if re.search(r"SPEED", text):
        speed_n += 1
        if speed_n % 4 == 1:
            print(text, flush=True)
    elif pat.search(text):
        print(text, flush=True)
