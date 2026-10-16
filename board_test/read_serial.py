import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM7'
s = serial.Serial(port, 115200, timeout=2)

# Toggle DTR/RTS to reset the board
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)
time.sleep(0.5)

# Read output for 15 seconds
end_time = time.time() + 15
while time.time() < end_time:
    data = s.read(1024)
    if data:
        text = data.decode('ascii', errors='replace')
        print(text, end='', flush=True)
    else:
        # Check if we already got the "DIAGNOSTIC COMPLETE" marker
        pass

s.close()
