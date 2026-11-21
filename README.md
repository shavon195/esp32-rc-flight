# ESP32 RC Flight over Wi-Fi

Fly a game plane on a laptop using a **real FlySky FS-i6X RC transmitter**.
An ESP32 reads the RC receiver over **i-BUS** and streams the stick positions
to the laptop over **Wi-Fi (UDP) at 50 packets per second**. No cables between
the controller setup and the laptop.

Built by Shavon (Chemistry & Electronics, UWI Mona) as hands-on prep for an
Internet of Things course.

## Why I built this

GTA 6 has been in the making for over a decade. I don't have a PS5 or an Xbox
to play games like that, but I do have an ESP32, an RC transmitter and
receiver, and a laptop. So I used what I had and built my own game.

It's for fun, something to relax with and something interactive: you fly it
with a real RC controller, not a keyboard. It's also how I'm learning the
Internet of Things: how devices find each other on a network, what IP
addresses and ports are, and why UDP beats TCP for live controls. It's a plane
for now; the plan is a first-person drone flight game. There's plenty of room
to improve, and that's the point.

```
FlySky FS-i6X ──2.4 GHz──► FS-iA10B receiver ──i-BUS (1 wire)──► ESP32
                                                                   │
                                     Wi-Fi UDP, port 4210, 50 Hz   │
                                                                   ▼
                                                        Laptop: Python game
```

## Hardware

| Part | Notes |
|---|---|
| FlySky FS-i6X transmitter | AFHDS 2A, Mode 2 |
| FlySky FS-iA10B receiver | 10 channels; PWM, PPM and i-BUS outputs; 4.0–6.5 V supply |
| ESP32 dev board (ESP-32D / WROOM-32, 38-pin, CP2102 USB) | Arduino core 4.0.0 |
| Laptop (Windows 11) | Runs the game and a Mobile Hotspot (2.4 GHz) the ESP32 joins |

## Wiring

i-BUS signal measured at **3.31 V** with a multimeter, so it connects to the
ESP32's 3.3 V input directly (no voltage divider needed).

| FS-iA10B **i-BUS SERVO** port (small top row, right group) | ESP32 |
|---|---|
| Left pin (GND) | GND |
| Middle pin (+) | 5V / VIN |
| Right pin (signal) | GPIO 16 (RX2) |

Pin order was confirmed with a multimeter continuity test against the main
header's − and + rows.

## Packet format (ESP32 → laptop)

Plain text, one UDP packet every 20 ms, sent to the hotspot gateway
(the laptop, usually `192.168.137.1`) on port **4210**:

```
RC,seq,ok,ch1,ch2,ch3,ch4,ch5,ch6
RC,1234,1,1504,1499,1000,1505,1000,1000
```

- `seq` — packet counter; gaps tell the laptop how many packets were lost
- `ok` — 1 if the receiver has signal, 0 if not (game triggers its failsafe)
- channels — 1000–2000 µs, same values as servo PWM

## Repository layout

```
esp32/
  ibus_reader_test/   Step 6: read i-BUS and print channels (no Wi-Fi)
  udp_hello_esp32/    Step 4: first UDP "hello" packets to the laptop
  rc_wifi_sender/     Step 7: i-BUS -> Wi-Fi UDP at 50 Hz (the main sketch)
laptop/
  udp_listener.py     Step 4: print raw UDP packets
  rc_listener.py      Step 7: decode RC packets, show rate and packet loss
  rc_flight_wifi.py   Step 8: side-scrolling flight game (pygame)
```

## How to run

1. **Wi-Fi details:** in `esp32/rc_wifi_sender/`, copy `secrets_example.h` to
   `secrets.h` and fill in your hotspot name and password. `secrets.h` is in
   `.gitignore`, so it is never uploaded.
2. **ESP32:** Arduino IDE → board "ESP32 Dev Module" → upload `rc_wifi_sender.ino`.
3. **Laptop:** turn on Windows Mobile Hotspot (2.4 GHz band), then:
   ```
   pip install pygame-ce
   python laptop/rc_flight_wifi.py
   ```
   Allow Python through Windows Firewall when asked.

Controls: left stick = throttle (CH3), right stick up/down = pitch (CH2),
CH5 switch = restart. Keyboard fallback: W/S throttle, ↑/↓ pitch, R restart.

## Results

- i-BUS decoding: 28,000+ frames, **0 checksum errors**, ~130 frames/s
- Wi-Fi link: **~50 packets/s**, typically 0–2 % loss, packet age ~20 ms
- Occasional short loss bursts — the laptop's single Wi-Fi radio is shared
  between the campus network and the hotspot

## Things learned along the way

- **UDP vs TCP:** for live stick data, the newest packet is all that matters.
  UDP drops a late packet; TCP would stall everything to resend it.
- **Ports:** only one program can bind a UDP port. `netstat -ano | findstr 4210`
  finds the owner's PID, `taskkill /PID <pid> /F` frees it.
- **Station mode + laptop hotspot:** the ESP32 joins the laptop's hotspot,
  so `WiFi.gatewayIP()` *is* the laptop — no hard-coded IP needed.
- **`WiFi.setSleep(false)`** removes the ESP32's Wi-Fi power-save delays.
- **Windows username with a space** breaks the ESP32 build tools; fixed by
  moving the Arduino15 data folder to `C:\Arduino15`.
- **Python 3.14:** plain `pygame` had no prebuilt wheel; `pygame-ce` works
  as a drop-in replacement.

## Roadmap

- [ ] First-person 3D drone game (Godot) with obstacles, gates, timer
- [ ] Player names and a high-score table
- [ ] Laptop → ESP32 events: room LEDs turn red on crash, cyan on restart
- [ ] Voice audio ("Begin", "Game over") via ESP32 Bluetooth to a speaker
- [ ] Browser version of the game served by the ESP32 (WebSockets)
- [ ] Port to ESP32-S3
