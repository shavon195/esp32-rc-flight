/*
 * ibus_reader_test.ino  —  read FlySky i-BUS on the ESP32 and print the channels
 * Board: ESP32 Dev Module (ESP-32D, 38-pin)
 * Receiver: FlySky FS-iA10B, i-BUS SERVO port (small top row, right group)
 *
 * WIRING  (i-BUS signal measured at 3.31 V -> safe, no divider)
 *   Receiver i-BUS SERVO  left  pin (GND)    -> ESP32 GND
 *   Receiver i-BUS SERVO  middle pin (+)     -> ESP32 5V (VIN)
 *   Receiver i-BUS SERVO  right pin (signal) -> ESP32 GPIO 16 (RX2)
 *
 * WHAT IT DOES
 *   Reads 32-byte i-BUS frames on UART2, checks the checksum, and prints
 *   CH1–CH6 ten times per second. Values are 1000–2000 (microseconds).
 *
 * i-BUS FRAME FORMAT (32 bytes, sent every ~7 ms at 115200 baud)
 *   byte 0      : 0x20  (frame length = 32)
 *   byte 1      : 0x40  (command: "channel data")
 *   bytes 2–29  : 14 channels, 2 bytes each, LOW byte first (little-endian)
 *   bytes 30–31 : checksum = 0xFFFF minus the sum of bytes 0–29
 */

const int IBUS_RX_PIN = 16;     // receiver signal goes here
const int IBUS_TX_PIN = 17;     // not used (receiver only talks, never listens)
const int NUM_CH      = 14;     // i-BUS always carries 14 channels

HardwareSerial IBus(2);         // use the ESP32's UART2 hardware serial port

uint8_t  frame[32];             // holds the bytes of the frame being received
int      idx = 0;               // how many bytes of the current frame we have
uint16_t ch[NUM_CH];            // latest good channel values
uint32_t goodFrames = 0;        // frames that passed the checksum
uint32_t badFrames  = 0;        // frames that failed (corrupted)
uint32_t lastFrameMs = 0;       // time of last good frame (to detect signal loss)

// Feed one byte into the frame decoder. Returns true when a full,
// valid frame has just been decoded into ch[].
bool ibusFeed(uint8_t b) {
  // Byte 0 must be 0x20, otherwise we're not at the start of a frame
  if (idx == 0 && b != 0x20) return false;

  // Byte 1 must be 0x40, otherwise that 0x20 was a coincidence: start over
  if (idx == 1 && b != 0x40) {
    idx = (b == 0x20) ? 1 : 0;  // this byte might be the real start
    return false;
  }

  frame[idx++] = b;
  if (idx < 32) return false;   // frame not complete yet
  idx = 0;                      // got 32 bytes: get ready for the next frame

  // Checksum: 0xFFFF minus the sum of the first 30 bytes
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= frame[i];
  uint16_t received = frame[30] | (frame[31] << 8);   // little-endian
  if (sum != received) {
    badFrames++;
    return false;               // corrupted frame: ignore it
  }

  // Unpack the 14 channels (low byte first, then high byte)
  for (int i = 0; i < NUM_CH; i++) {
    ch[i] = (frame[2 + 2 * i] | (frame[3 + 2 * i] << 8)) & 0x0FFF;
  }
  goodFrames++;
  lastFrameMs = millis();
  return true;
}

void setup() {
  Serial.begin(115200);         // USB serial to the laptop (Serial Monitor)
  IBus.begin(115200, SERIAL_8N1, IBUS_RX_PIN, IBUS_TX_PIN);  // i-BUS in
  Serial.println("i-BUS reader started. Turn on the transmitter...");
}

void loop() {
  // Process every byte that has arrived from the receiver
  while (IBus.available()) {
    ibusFeed(IBus.read());
  }

  // Print 10 times per second
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 100) {
    lastPrint = millis();

    if (millis() - lastFrameMs > 500) {
      Serial.println("No i-BUS signal (check wiring / transmitter on?)");
    } else {
      Serial.printf("CH1:%4u CH2:%4u CH3:%4u CH4:%4u CH5:%4u CH6:%4u  good:%lu bad:%lu\n",
                    ch[0], ch[1], ch[2], ch[3], ch[4], ch[5],
                    (unsigned long)goodFrames, (unsigned long)badFrames);
    }
  }
}
