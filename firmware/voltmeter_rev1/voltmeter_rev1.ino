/*
  USB-Voltmeter – Firmware fuer Platine Rev. 1 (Software-SPI-Workaround)
  ----------------------------------------------------------------------
  Rev. 1 hat die Digitalheader P4/P5 um 100 mil (1 Pin) zu weit von der
  USB-Buchse entfernt. Jedes Signal landet deshalb auf dem Nachbar-Pin
  des Uno. Hardware-SPI (D11/D12/D13) ist so nicht nutzbar, daher wird
  SPI hier per Software auf den tatsaechlich getroffenen Pins erzeugt.

  Bestueckung Rev. 1:
    P6, P7 komplett. P4 NUR Pin 5..8 (von der USB-Seite gezaehlt das
    5. bis 8. Loch). P5 NICHT bestuecken.
      P4 Pin 4 (GND)  wuerde auf D13 landen  -> weglassen
      P5 Pin 6 (DRDY) wuerde auf D1/TX landen -> weglassen, DRDY wird
                      nicht gebraucht (Single-Shot + Wartezeit)

    Netz auf Platine   landet auf Uno   Funktion hier
    SCLK (P4 Pin 5) -> D12              Ausgang
    MISO (P4 Pin 6) -> D11              Eingang  (ADS1220 DOUT)
    MOSI (P4 Pin 7) -> D10              Ausgang  (ADS1220 DIN)
    CS   (P4 Pin 8) -> D9               Ausgang

  WICHTIG: Diesen Sketch hochladen, BEVOR das Shield aufgesteckt wird.

  Messprinzip: ADS1220 misst differentiell AIN0 (OUT) - AIN1 (REF),
  PGA an, Gain 1, interne Referenz 2,048 V, 20 SPS, 50/60-Hz-Filter.
    V_in = 11 * V_diff + 2,5 V
  Eingang offen  -> Anzeige ca. 2,5 V (Knoten liegt ueber R2 auf REF)
  Eingang auf GND -> Anzeige ca. 0 V
*/

const uint8_t PIN_CS   = 9;
const uint8_t PIN_MOSI = 10;
const uint8_t PIN_MISO = 11;
const uint8_t PIN_SCLK = 12;

const float VREF_ADC = 2.048;    // interne Referenz ADS1220 [V]
const float TEILER   = 11.0;     // (R1 + R2) / R2 = (10M + 1M) / 1M
const float V_FUSS   = 2.507;    // LM4040, an TP2 gemessen [V]

// Kalibrierung V_kal = KAL_A * V_roh + KAL_B
// KAL_B: Eingang kurzgeschlossen (P1 gebrueckt) -> V_roh = 0,0171 V (13.11.2026)
// KAL_A: bleibt 1, bis eine genauere Referenz als das VC97 verfuegbar ist
const float KAL_A    = 1.0;
const float KAL_B    = -0.0171;  // [V]
const int   N        = 10;       // Einzelmessungen pro Ausgabezeile (~0,7 s)

// ---------- Software-SPI, Mode 1 (CPOL = 0, CPHA = 1) ----------
// ADS1220 gibt DOUT mit der steigenden Flanke aus und uebernimmt DIN
// mit der fallenden Flanke.
uint8_t spiByte(uint8_t out) {
  uint8_t in = 0;
  for (int8_t i = 7; i >= 0; i--) {
    digitalWrite(PIN_SCLK, HIGH);
    digitalWrite(PIN_MOSI, (out >> i) & 0x01);
    delayMicroseconds(2);
    in = (in << 1) | (digitalRead(PIN_MISO) ? 1 : 0);
    digitalWrite(PIN_SCLK, LOW);
    delayMicroseconds(2);
  }
  return in;
}

void csLow()  { digitalWrite(PIN_CS, LOW);  delayMicroseconds(2); }
void csHigh() { delayMicroseconds(2); digitalWrite(PIN_CS, HIGH); }

void command(uint8_t c) { csLow(); spiByte(c); csHigh(); }

void writeReg(uint8_t reg, uint8_t val) {
  csLow();
  spiByte(0x40 | (reg << 2));   // WREG, 1 Byte
  spiByte(val);
  csHigh();
}

uint8_t readReg(uint8_t reg) {
  csLow();
  spiByte(0x20 | (reg << 2));   // RREG, 1 Byte
  uint8_t v = spiByte(0x00);
  csHigh();
  return v;
}

long readData() {
  csLow();
  spiByte(0x10);                // RDATA
  long b2 = spiByte(0x00);
  long b1 = spiByte(0x00);
  long b0 = spiByte(0x00);
  csHigh();
  long code = (b2 << 16) | (b1 << 8) | b0;
  if (code & 0x800000L) code |= 0xFF000000L;   // 24-Bit-Zweierkomplement
  return code;
}

// Eine Wandlung im Single-Shot-Modus, Ergebnis V_diff in mV
float messeVdiff_mV() {
  command(0x08);                // START/SYNC
  delay(70);                    // Wandlungszeit bei 20 SPS ca. 50 ms + Reserve
  long code = readData();
  return code * (VREF_ADC * 1000.0 / 8388608.0);
}

void setup() {
  pinMode(PIN_CS, OUTPUT);   digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_SCLK, OUTPUT); digitalWrite(PIN_SCLK, LOW);
  pinMode(PIN_MOSI, OUTPUT); digitalWrite(PIN_MOSI, LOW);
  pinMode(PIN_MISO, INPUT);

  Serial.begin(115200);
  while (!Serial) {}
  delay(50);

  command(0x06);                // RESET
  delay(5);

  writeReg(0, 0x00);   // MUX = AIN0-AIN1, Gain 1, PGA an (puffert die ADC-Eingaenge)
  writeReg(1, 0x00);   // 20 SPS, Normal Mode, Single-Shot
  writeReg(2, 0x10);   // interne Referenz, 50/60 Hz simultan
  writeReg(3, 0x00);

  uint8_t r0 = readReg(0), r1 = readReg(1), r2 = readReg(2);
  if (r0 != 0x00 || r1 != 0x00 || r2 != 0x10) {
    Serial.print(F("FEHLER: ADS1220 antwortet nicht korrekt. Reg0/1/2 = 0x"));
    Serial.print(r0, HEX); Serial.print(F(" 0x"));
    Serial.print(r1, HEX); Serial.print(F(" 0x"));
    Serial.println(r2, HEX);
    Serial.println(F("Shield richtig aufgesteckt? P4 Pin 5..8 bestueckt?"));
    while (1) {}
  }

  Serial.println(F("ADS1220 bereit (Rev. 1, Software-SPI)."));
  Serial.println(F("V_roh [V] | V_kal [V] | sigma(V_in) [uV]"));
}

void loop() {
  float v[N];
  float sum = 0.0;
  for (int i = 0; i < N; i++) {
    v[i] = messeVdiff_mV();
    sum += v[i];
  }
  float mean = sum / N;

  float sq = 0.0;
  for (int i = 0; i < N; i++) {
    float d = v[i] - mean;
    sq += d * d;
  }
  float sigmaDiff_mV = sqrt(sq / (N - 1));

  float vRoh = TEILER * mean / 1000.0 + V_FUSS;      // [V]
  float vKal = KAL_A * vRoh + KAL_B;                  // [V]
  float sigmaIn_uV = TEILER * sigmaDiff_mV * 1000.0;  // [uV]

  Serial.print(vRoh, 5);       Serial.print(F(" V  |  "));
  Serial.print(vKal, 5);       Serial.print(F(" V  |  "));
  Serial.print(sigmaIn_uV, 1); Serial.println(F(" uV"));
}
