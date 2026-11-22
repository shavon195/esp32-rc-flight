"""
rc_flight_wifi.py  —  Step 8: fly a plane with your real FlySky transmitter over Wi-Fi

HOW IT CONNECTS
    FlySky FS-i6X  --2.4 GHz-->  FS-iA10B receiver  --i-BUS-->  ESP32
    ESP32  --Wi-Fi UDP, port 4210, 50 packets/s-->  this game

    Packet from the ESP32 (rc_wifi_sender.ino or rc_wifi_led.ino):
        RC,seq,ok,ch1,ch2,ch3,ch4,ch5,ch6

    Events from this game back to the ESP32 (rc_wifi_led.ino), sent to the
    same IP and port the stick packets came from:
        EVT,id,CRASH   -> room LEDs turn red
        EVT,id,START   -> room LEDs turn cyan

CONTROLS (transmitter, Mode 2)
    Right stick up/down  (CH2) = pitch (nose up / down)
    Left stick up/down   (CH3) = throttle
    CH5 switch flipped up      = restart after a crash
    If the plane pitches the wrong way, set INVERT_PITCH = True below.

KEYBOARD FALLBACK (used automatically when no RC data is arriving)
    W / S          = throttle up / down
    Up / Down      = nose up / down
    R              = restart
    Esc            = quit

GAME
    Take off from the runway, fly through the yellow rings for points.
    Too slow -> STALL (the nose drops; dive to regain speed).
    Hit the ground too hard or too steep -> CRASH.

Run:   python rc_flight_wifi.py
Needs: pip install pygame
"""

import math
import random
import socket
import time

import pygame

# ============================================================
# SETTINGS
# ============================================================
UDP_PORT = 4210             # must match LAPTOP_PORT in the ESP32 sketch
RC_TIMEOUT = 0.5            # seconds without packets -> "RC link lost"
INVERT_PITCH = False        # flip if stick-back makes the nose go DOWN
DEADBAND_US = 20            # ignore tiny stick wobble around centre (us)
CH_RESTART = 4              # index of CH5 in the channel list (0-based)

WIDTH, HEIGHT = 1000, 600
FPS = 60
SCALE = 4.0                 # pixels per metre
GROUND_Y = HEIGHT - 70      # screen y of the ground when camera is at 0 m

# Flight model (simple "arcade" physics, units: metres, seconds)
G = 9.81
MAX_THRUST = 14.0           # forward acceleration at full throttle (m/s^2)
TOP_SPEED = 40.0            # level top speed at full throttle (m/s)
DRAG_K = MAX_THRUST / TOP_SPEED ** 2
STALL_SPEED = 12.0          # below this (in the air) the wing stops lifting
ROTATE_SPEED = 14.0         # minimum speed to lift the nose on the runway
PITCH_RATE = 70.0           # degrees/second at full stick
CRASH_SINK = 4.0            # touching ground faster than this (m/s) = crash
CRASH_ANGLE = 12.0          # touching ground nose-down more than this = crash

RING_RADIUS = 8.0           # metres (half the ring opening)


# ============================================================
# RC LINK: receives UDP packets from the ESP32 without blocking
# ============================================================
class RCLink:
    """Non-blocking UDP receiver. Call update() once per game frame."""

    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", port))
        self.sock.setblocking(False)       # recvfrom returns immediately
        # Windows quirk: if we send to a port nobody is listening on, the
        # NEXT recvfrom raises ConnectionResetError. Turn that behaviour off.
        if hasattr(socket, "SIO_UDP_CONNRESET"):
            self.sock.ioctl(socket.SIO_UDP_CONNRESET, False)
        self.channels = [1500] * 6
        self.rx_ok = False                 # receiver has signal (from ESP32)
        self.last_rx_time = 0.0            # when the last packet arrived
        self.last_seq = None
        self.lost = 0
        self.received = 0
        self.sender = None                 # ESP32 IP (from its packets)
        self.sender_port = None            # ESP32 port (we reply to it)
        self.event_id = 0                  # numbers our events so repeats are ignored
        self._count = 0                    # packets this second
        self._t0 = time.time()
        self.rate = 0.0                    # packets per second

    def update(self):
        # Read EVERY packet waiting in the buffer; keep only the newest.
        # (At 60 FPS and 50 packets/s there's usually 0 or 1 waiting.)
        while True:
            try:
                data, addr = self.sock.recvfrom(1024)
            except BlockingIOError:
                break                      # buffer empty: done for this frame
            except OSError:
                break                      # other socket hiccup: try next frame
            self._handle(data, addr)

        now = time.time()
        if now - self._t0 >= 1.0:
            self.rate = self._count / (now - self._t0)
            self._count = 0
            self._t0 = now

    def _handle(self, data, addr):
        parts = data.decode(errors="replace").strip().split(",")
        if len(parts) != 9 or parts[0] != "RC":
            return                         # not our packet
        try:
            seq = int(parts[1])
            ok = parts[2] == "1"
            chans = [int(x) for x in parts[3:9]]
        except ValueError:
            return

        # A silence of over 1 s means the ESP32 restarted or reconnected:
        # start counting fresh instead of judging this seq against the old one.
        if time.time() - self.last_rx_time > 1.0:
            self.last_seq = None

        if self.last_seq is not None:
            if seq > self.last_seq:
                self.lost += seq - self.last_seq - 1
            elif seq >= self.last_seq - 50:
                return                     # duplicate or late packet: ignore it
            # else: seq dropped a lot -> ESP32 rebooted, accept and restart
        self.last_seq = seq

        self.channels = chans
        self.rx_ok = ok
        self.sender = addr[0]
        self.sender_port = addr[1]
        self.last_rx_time = time.time()
        self.received += 1
        self._count += 1

    def send_event(self, name):
        """Tell the ESP32 something happened (CRASH / START).
        UDP can lose packets, so we send 3 copies with the same id;
        the ESP32 ignores copies of an id it has already seen."""
        if self.sender is None:
            return                         # no ESP32 heard yet: nobody to tell
        self.event_id += 1
        msg = f"EVT,{self.event_id},{name}".encode()
        for _ in range(3):
            try:
                self.sock.sendto(msg, (self.sender, self.sender_port))
            except OSError:
                pass

    @property
    def active(self):
        """True if fresh packets are arriving AND the receiver has signal."""
        return self.rx_ok and (time.time() - self.last_rx_time) < RC_TIMEOUT

    @property
    def age_ms(self):
        return (time.time() - self.last_rx_time) * 1000.0

    @property
    def loss_pct(self):
        total = self.received + self.lost
        return 100.0 * self.lost / total if total else 0.0

    def close(self):
        self.sock.close()


def stick(us, deadband=DEADBAND_US):
    """Convert a 1000..2000 us channel to -1.0..+1.0 with a centre deadband."""
    x = us - 1500
    if abs(x) < deadband:
        return 0.0
    x = x - deadband if x > 0 else x + deadband
    return max(-1.0, min(1.0, x / (500 - deadband)))


def throttle_from(us):
    """Convert a 1000..2000 us throttle channel to 0.0..1.0."""
    return max(0.0, min(1.0, (us - 1000) / 1000.0))


# ============================================================
# PLANE: simple side-view flight model
# ============================================================
class Plane:
    def __init__(self):
        self.x = 0.0          # distance flown (m)
        self.y = 0.0          # altitude (m), 0 = ground
        self.speed = 0.0      # airspeed along the nose direction (m/s)
        self.pitch = 0.0      # nose angle in degrees (+ = up)
        self.throttle = 0.0   # 0..1
        self.on_ground = True
        self.stalled = False
        self.crashed = False
        self.vy = 0.0         # vertical speed (m/s), for the HUD and crash check

    def update(self, pitch_input, throttle, dt):
        """pitch_input: -1..+1 (+ = nose up). throttle: 0..1."""
        if self.crashed:
            return
        self.throttle = throttle

        # --- Pitch: stick sets how FAST the nose rotates (like a real elevator)
        self.pitch += pitch_input * PITCH_RATE * dt

        # --- Stall: too slow in the air -> wing stops lifting, nose falls
        self.stalled = (not self.on_ground) and self.speed < STALL_SPEED
        if self.stalled:
            self.pitch -= 45.0 * dt                     # nose drops by itself
        self.pitch = max(-80.0, min(80.0, self.pitch))

        # --- On the runway: nose can't point down, and can't lift until fast
        if self.on_ground:
            if self.speed < ROTATE_SPEED:
                self.pitch = 0.0
            self.pitch = max(0.0, min(15.0, self.pitch))

        rad = math.radians(self.pitch)

        # --- Speed along the nose: thrust - drag - gravity (climbing slows you)
        accel = throttle * MAX_THRUST - DRAG_K * self.speed ** 2
        accel -= G * math.sin(rad)
        if self.on_ground:
            accel -= 1.0                                # rolling friction
        self.speed = max(0.0, self.speed + accel * dt)

        # --- Move along the nose direction
        vx = self.speed * math.cos(rad)
        vy = self.speed * math.sin(rad)
        if self.stalled:
            vy -= (1.0 - self.speed / STALL_SPEED) * 10.0   # extra sinking
        self.vy = vy
        self.x += vx * dt
        self.y += vy * dt

        # --- Ground contact
        if self.y <= 0.0:
            if not self.on_ground and (vy < -CRASH_SINK or self.pitch < -CRASH_ANGLE):
                self.crashed = True
            self.y = 0.0
            self.on_ground = True
            self.vy = 0.0
        elif self.y > 0.5:
            self.on_ground = False


# ============================================================
# RINGS
# ============================================================
class Rings:
    def __init__(self):
        self.items = []       # each: [x, y, passed, hit]
        self.next_x = 250.0
        self.score = 0

    def update(self, prev_x, plane):
        # Keep rings generated ~600 m ahead
        while self.next_x < plane.x + 600:
            self.items.append([self.next_x, random.uniform(25, 110), False, False])
            self.next_x += random.uniform(150, 250)
        # Did the plane cross a ring's x position this frame?
        for r in self.items:
            if not r[2] and prev_x < r[0] <= plane.x:
                r[2] = True
                if abs(plane.y - r[1]) <= RING_RADIUS:
                    r[3] = True
                    self.score += 1
        # Forget rings far behind
        self.items = [r for r in self.items if r[0] > plane.x - 200]


# ============================================================
# DRAWING
# ============================================================
def make_clouds():
    return [(random.uniform(0, 3000), random.uniform(40, 160), random.uniform(0.6, 1.4))
            for _ in range(25)]


def draw_world(screen, plane, rings, clouds, cam_x, cam_alt):
    def sx(x):  # world x -> screen x
        return int((x - cam_x) * SCALE)

    def sy(y):  # altitude -> screen y
        return int(GROUND_Y - (y - cam_alt) * SCALE)

    # Sky (lighter near the horizon)
    for i in range(0, HEIGHT, 10):
        t = i / HEIGHT
        col = (int(90 + 80 * t), int(150 + 70 * t), int(230 + 20 * t))
        pygame.draw.rect(screen, col, (0, i, WIDTH, 10))

    # Clouds (parallax: they move at half speed so they feel far away)
    for cx, cy, size in clouds:
        x = int(((cx - cam_x * 0.5) % 3000) * SCALE * 0.4) - 100
        y = sy(cy)
        for dx, r in ((0, 22), (25, 28), (52, 20)):
            pygame.draw.circle(screen, (245, 245, 250), (x + int(dx * size), y), int(r * size))

    # Ground
    gy = sy(0)
    if gy < HEIGHT:
        pygame.draw.rect(screen, (70, 150, 70), (0, gy, WIDTH, HEIGHT - gy))
        # Runway at the start
        pygame.draw.rect(screen, (90, 90, 95), (sx(-30), gy, int(230 * SCALE), 8))
        # Distance posts every 20 m to show speed
        first = int(cam_x // 20) * 20
        for px in range(first, first + int(WIDTH / SCALE) + 40, 20):
            pygame.draw.line(screen, (50, 110, 50), (sx(px), gy), (sx(px), gy + 14), 3)

    # Rings (yellow = waiting, green = got it, grey = missed)
    for rx, ry, passed, hit in rings.items:
        col = (60, 220, 90) if hit else ((150, 150, 150) if passed else (255, 210, 40))
        rect = pygame.Rect(0, 0, 18, int(RING_RADIUS * 2 * SCALE))
        rect.center = (sx(rx), sy(ry))
        pygame.draw.ellipse(screen, col, rect, 5)

    # Plane: a simple polygon rotated to the pitch angle
    body = [(22, 0), (8, 5), (-18, 5), (-24, 14), (-28, 14), (-24, 0), (-18, -4), (8, -5)]
    wing = [(4, 0), (-8, 0), (-14, -12), (-6, -12)]
    centre = pygame.math.Vector2(sx(plane.x), sy(plane.y) - 6)
    col = (120, 60, 60) if plane.crashed else (220, 60, 50)
    for shape, c in ((body, col), (wing, (170, 40, 40))):
        pts = [centre + pygame.math.Vector2(p).rotate(-plane.pitch) * 1.2 for p in shape]
        pygame.draw.polygon(screen, c, pts)


def draw_hud(screen, font, big, plane, rings, link, using_rc, best):
    # Throttle bar
    pygame.draw.rect(screen, (30, 30, 30), (20, 20, 24, 150), 2)
    h = int(146 * plane.throttle)
    pygame.draw.rect(screen, (240, 140, 40), (22, 168 - h, 20, h))
    screen.blit(font.render("THR", True, (20, 20, 20)), (14, 175))

    lines = [
        f"Speed  {plane.speed:5.1f} m/s",
        f"Alt    {plane.y:5.1f} m",
        f"Climb  {plane.vy:+5.1f} m/s",
        f"Pitch  {plane.pitch:+5.1f} deg",
        f"Rings  {rings.score}   Best {best}",
    ]
    for i, t in enumerate(lines):
        screen.blit(font.render(t, True, (20, 20, 20)), (60, 20 + i * 22))

    # Link panel (top right)
    if using_rc:
        status, col = f"RC via Wi-Fi ({link.sender})", (0, 110, 0)
    elif link.last_rx_time and not link.rx_ok and link.age_ms < 1000:
        status, col = "RX SIGNAL LOST - keyboard", (180, 0, 0)
    else:
        status, col = "KEYBOARD (no RC packets)", (180, 90, 0)
    info = [
        (status, col),
        (f"{link.rate:4.1f} pkt/s   lost {link.lost} ({link.loss_pct:.1f}%)", (20, 20, 20)),
        (f"last packet {min(link.age_ms, 9999):5.0f} ms ago", (20, 20, 20)),
    ]
    for i, (t, c) in enumerate(info):
        surf = font.render(t, True, c)
        screen.blit(surf, (WIDTH - surf.get_width() - 15, 20 + i * 22))

    # Warnings
    if plane.stalled and not plane.crashed:
        s = big.render("STALL", True, (200, 0, 0))
        screen.blit(s, (WIDTH // 2 - s.get_width() // 2, 80))
    if plane.crashed:
        s = big.render("CRASH!", True, (200, 0, 0))
        screen.blit(s, (WIDTH // 2 - s.get_width() // 2, HEIGHT // 2 - 80))
        t = font.render("Press R or flip the CH5 switch to restart", True, (20, 20, 20))
        screen.blit(t, (WIDTH // 2 - t.get_width() // 2, HEIGHT // 2 - 20))
    if plane.on_ground and plane.x < 5 and not plane.crashed:
        t = font.render("Throttle up to take off  (keyboard: W, then Up arrow)", True, (20, 20, 20))
        screen.blit(t, (WIDTH // 2 - t.get_width() // 2, 140))


# ============================================================
# MAIN LOOP
# ============================================================
def main():
    pygame.init()
    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("RC Flight over Wi-Fi")
    clock = pygame.time.Clock()
    font = pygame.font.SysFont("consolas", 18)
    big = pygame.font.SysFont("consolas", 56, bold=True)

    link = RCLink(UDP_PORT)
    plane, rings, clouds = Plane(), Rings(), make_clouds()
    best = 0
    kb_throttle = 0.0
    was_rc = False
    last_ch5_high = False
    was_crashed = False
    start_sent = False

    running = True
    while running:
        dt = min(clock.tick(FPS) / 1000.0, 0.05)   # seconds since last frame

        # ---------- events ----------
        restart = False
        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                running = False
            elif e.type == pygame.KEYDOWN:
                if e.key == pygame.K_ESCAPE:
                    running = False
                elif e.key == pygame.K_r:
                    restart = True

        # ---------- network ----------
        link.update()
        using_rc = link.active

        # ---------- inputs ----------
        if using_rc:
            ch = link.channels
            pitch_in = stick(ch[1])                 # CH2
            if INVERT_PITCH:
                pitch_in = -pitch_in
            thr = throttle_from(ch[2])              # CH3
            ch5_high = ch[CH_RESTART] > 1700        # restart on switch flip
            if ch5_high and not last_ch5_high:
                restart = True
            last_ch5_high = ch5_high
            kb_throttle = thr                       # keyboard takes over smoothly
        else:
            if was_rc:
                kb_throttle = 0.0                   # FAILSAFE: link lost -> cut throttle
            keys = pygame.key.get_pressed()
            if keys[pygame.K_w]:
                kb_throttle = min(1.0, kb_throttle + 0.6 * dt)
            if keys[pygame.K_s]:
                kb_throttle = max(0.0, kb_throttle - 0.6 * dt)
            pitch_in = (1.0 if keys[pygame.K_UP] else 0.0) - (1.0 if keys[pygame.K_DOWN] else 0.0)
            thr = kb_throttle
        was_rc = using_rc

        if restart:
            best = max(best, rings.score)
            plane, rings = Plane(), Rings()
            link.send_event("START")                # room LEDs -> cyan

        # First time we hear the ESP32, tell it a game has started
        if not start_sent and link.sender is not None:
            link.send_event("START")
            start_sent = True

        # ---------- physics ----------
        prev_x = plane.x
        plane.update(pitch_in, thr, dt)
        rings.update(prev_x, plane)
        best = max(best, rings.score)

        # Crash just happened (was fine last frame, crashed now)?
        if plane.crashed and not was_crashed:
            link.send_event("CRASH")                # room LEDs -> red
        was_crashed = plane.crashed

        # ---------- camera ----------
        cam_x = plane.x - 250 / SCALE              # plane sits 250 px from the left
        cam_alt = max(0.0, plane.y - 60.0)         # follow upward, keep ground in view

        # ---------- draw ----------
        draw_world(screen, plane, rings, clouds, cam_x, cam_alt)
        draw_hud(screen, font, big, plane, rings, link, using_rc, best)
        pygame.display.flip()

    link.close()
    pygame.quit()


if __name__ == "__main__":
    main()
