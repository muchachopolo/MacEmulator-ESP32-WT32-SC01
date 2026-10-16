import serial
import time
import sys

port = 'COM7'
baud = 115200

s = serial.Serial(port, baud, timeout=0.5)
# Toggle DTR to reset the board
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
time.sleep(0.1)

start = time.time()
timeout = 300  # seconds - 5 minutes to wait for dispatch loop natural exit

while time.time() - start < timeout:
    data = s.read(s.in_waiting or 1)
    if data:
        try:
            text = data.decode('utf-8', 'replace')
            sys.stdout.write(text)
            sys.stdout.flush()
        except:
            pass

s.close()
