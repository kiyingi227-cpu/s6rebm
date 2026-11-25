#include <Arduino.h>
#include <Wire.h>

const int PIN_SDA     = 21;
const int PIN_SCL     = 22;
const int PIN_DSP_PDN = 5;     // AK7755 PDN: LOW = power-down, HIGH = run

const uint8_t AK_ADDR = 0x18;  // 0011000 (CAD = L). Use 0x19 if CAD = H.

const uint8_t CMD_DEVICE_ID = 0x60;   // returns 0x55
const uint8_t CMD_READ_CONT0D = 0x4D; // 0x40 + 0x0D, default 0x80

// ---------------------------------------------------------------------------
void checkBusIdle() {
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  delay(5);
  int sda = digitalRead(PIN_SDA);
  int scl = digitalRead(PIN_SCL);
  Serial.printf("Bus idle levels: SDA=%d SCL=%d (both should be 1)\n", sda, scl);
  if (!sda || !scl) {
    Serial.println("  -> A line is LOW. Check pull-ups to 3.3V (4.7k) or a short to GND.");
  }
}

void dspPowerCycle() {
  pinMode(PIN_DSP_PDN, OUTPUT);
  digitalWrite(PIN_DSP_PDN, LOW);   // datasheet: PDN must be L at power-up
  delay(20);
  digitalWrite(PIN_DSP_PDN, HIGH);  // release from power-down
  delay(50);                        // let the PLL and the internal logic settle
  Serial.println("DSP_PDN released (HIGH).");
}

void scanBus() {
  Serial.println("Scanning I2C bus...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X%s\n", a, a == AK_ADDR ? "  <-- AK7755" : "");
      found++;
    }
  }
  if (!found) Serial.println("  nothing answered.");
}

// Write command byte, repeated start, read one byte (AK7755 read sequence)
int readCmd(uint8_t cmd, uint8_t &val) {
  Wire.beginTransmission(AK_ADDR);
  Wire.write(cmd);
  int err = Wire.endTransmission(false);   // repeated start, no STOP
  if (err != 0) return err;                // 2 = address NACK, 3 = data NACK
  if (Wire.requestFrom((int)AK_ADDR, 1) != 1) return 99;
  val = Wire.read();
  return 0;
}

void testAK7755() {
  uint8_t id = 0;
  int err = readCmd(CMD_DEVICE_ID, id);
  if (err == 0) {
    Serial.printf("Device ID (cmd 0x60) = 0x%02X  %s\n", id,
                  id == 0x55 ? "-> OK, AK7755 is talking" : "-> unexpected (expected 0x55)");
  } else {
    Serial.printf("Device ID read FAILED, error %d\n", err);
    if (err == 2) Serial.println("  -> No ACK at 0x18. Check: CAD pin level, PDN, I2CSEL=H, SDA/SCL wiring, 3.3V.");
    if (err == 3) Serial.println("  -> Address ACKed but command rejected. DSP may still be in reset (RDY low).");
    if (err == 99) Serial.println("  -> Command sent, no data returned.");
  }

  uint8_t c0d = 0;
  err = readCmd(CMD_READ_CONT0D, c0d);
  if (err == 0)
    Serial.printf("CONT0D (cmd 0x4D)   = 0x%02X  (default 0x80)\n", c0d);
  else
    Serial.printf("CONT0D read FAILED, error %d\n", err);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== AK7755 I2C test ===");

  checkBusIdle();
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);            // 100 kHz for the first bring-up
  dspPowerCycle();
  scanBus();
  testAK7755();
}

void loop() {
  delay(3000);
  Serial.println("\n--- repeat ---");
  scanBus();
  testAK7755();
}
