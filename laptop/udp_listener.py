"""
udp_listener.py  —  Step 4: receive UDP messages from the ESP32

Run it in a terminal:   python udp_listener.py
Stop it with:           Ctrl + C

What it does:
  Opens UDP port 4210 on the laptop and prints every packet that arrives,
  plus the IP address and port it came from.

If Windows Firewall pops up asking about Python, click "Allow",
otherwise the packets get blocked before they reach this program.
"""

import socket

PORT = 4210   # must match LAPTOP_PORT in the ESP32 sketch

# AF_INET = IPv4 addresses, SOCK_DGRAM = UDP (datagrams)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# "0.0.0.0" means: listen on ALL of the laptop's network connections
# (hotspot, campus Wi-Fi, etc.)
sock.bind(("0.0.0.0", PORT))

# Wake up every second so Ctrl+C works on Windows
sock.settimeout(1.0)

print(f"Listening for UDP on port {PORT}... (Ctrl+C to stop)")

try:
    while True:
        try:
            data, (ip, port) = sock.recvfrom(1024)   # wait for one packet
        except socket.timeout:
            continue                                 # nothing yet, keep waiting
        print(f"From {ip}:{port}  ->  {data.decode(errors='replace')}")
except KeyboardInterrupt:
    print("\nStopped.")
finally:
    sock.close()
