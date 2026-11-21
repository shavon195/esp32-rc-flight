"""
rc_listener.py  —  Step 7: receive stick data from the ESP32 over UDP

Run:   python rc_listener.py        Stop: Ctrl + C

Expects packets like:   RC,seq,ok,ch1,ch2,ch3,ch4,ch5,ch6
Once per second it prints:
  - the latest stick values
  - packets per second (should be about 50)
  - how many packets were lost (found from gaps in seq)
"""

import socket
import time

PORT = 4210

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PORT))
sock.settimeout(0.5)            # wake up regularly so Ctrl+C works

print(f"Listening for RC data on UDP port {PORT}... (Ctrl+C to stop)")

last_seq = None                 # seq number of the previous packet
received = 0                    # total packets received
lost = 0                        # total packets missing (seq gaps)
count_this_second = 0           # for packets-per-second
second_start = time.time()
latest = None                   # (ok, [ch1..ch6]) from the newest packet
last_rx_time = 0.0              # when the newest packet arrived

try:
    while True:
        # ---- 1) Try to receive one packet ----
        try:
            data, (ip, port) = sock.recvfrom(1024)
        except socket.timeout:
            data = None

        now = time.time()

        # ---- 2) Decode it ----
        if data:
            parts = data.decode(errors="replace").strip().split(",")
            if len(parts) == 9 and parts[0] == "RC":
                try:
                    seq = int(parts[1])
                    ok = parts[2] == "1"
                    channels = [int(x) for x in parts[3:9]]
                except ValueError:
                    continue                      # garbage packet: skip it

                # Packet loss: if seq jumps from 100 to 103, we lost 2
                if last_seq is not None and seq > last_seq:
                    lost += seq - last_seq - 1
                # If seq went DOWN, the ESP32 rebooted: just start again
                last_seq = seq

                received += 1
                count_this_second += 1
                latest = (ok, channels)
                last_rx_time = now

        # ---- 3) Print a status line once per second ----
        if now - second_start >= 1.0:
            rate = count_this_second / (now - second_start)
            count_this_second = 0
            second_start = now

            if latest is None or now - last_rx_time > 1.0:
                print("Waiting for ESP32... (no packets)")
            else:
                ok, c = latest
                total = received + lost
                loss_pct = 100.0 * lost / total if total else 0.0
                rx = "OK " if ok else "NO RX SIGNAL"
                print(f"{rx} CH1:{c[0]} CH2:{c[1]} CH3:{c[2]} CH4:{c[3]} "
                      f"CH5:{c[4]} CH6:{c[5]} | {rate:4.1f} pkt/s | "
                      f"lost {lost} ({loss_pct:.2f}%)")

except KeyboardInterrupt:
    print("\nStopped.")
finally:
    sock.close()
