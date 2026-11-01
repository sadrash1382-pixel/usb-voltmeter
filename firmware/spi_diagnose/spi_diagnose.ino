/*
  SPI-Diagnose fuer den ADS1220 (ohne Bibliothek)
  Schreibt einen Testwert in Register 0, liest ihn zurueck
  und sagt, wo das Problem vermutlich liegt.
  Laeuft in einer Schleife (alle 0,5 s), damit man mit dem Oszi messen kann.
  Pins: CS=10, MOSI=11, MISO=12, SCLK=13
*/
#include <SPI.h>

const uint8_t CS = 10;
SPISettings cfg(500000, MSBFIRST, SPI_MODE1);   // langsam, Mode 1 wie ADS1220

void command(uint8_t c) {
  SPI.beginTransaction(cfg);
  digitalWrite(CS, LOW);
  SPI.transfer(c);
  digitalWrite(CS, HIGH);
  SPI.endTransaction();
}

uint8_t readReg(uint8_t reg) {
  SPI.beginTransaction(cfg);
  digitalWrite(CS, LOW);
  SPI.transfer(0x20 | (reg << 2));   // RREG, 1 Byte
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(CS, HIGH);
  SPI.endTransaction();
  return v;
}

void writeReg(uint8_t reg, uint8_t val) {
  SPI.beginTransaction(cfg);
  digitalWrite(CS, LOW);
  SPI.transfer(0x40 | (reg << 2));   // WREG, 1 Byte
  SPI.transfer(val);
  digitalWrite(CS, HIGH);
  SPI.endTransaction();
}

void printHex(const __FlashStringHelper* label, uint8_t v) {
  Serial.print(label);
  Serial.print(F("0x"));
  if (v < 0x10) Serial.print('0');
  Serial.println(v, HEX);
}

void setup() {
  Serial.begin(115200);
  pinMode(CS, OUTPUT);
  digitalWrite(CS, HIGH);
  SPI.begin();
  delay(50);
}

void loop() {
  command(0x06);          // RESET
  delay(5);

  uint8_t before = readReg(0);
  writeReg(0, 0x01);      // PGA_BYPASS = 1 als Testwert
  uint8_t after  = readReg(0);
  writeReg(0, 0x00);      // zuruecksetzen

  Serial.println(F("--- ADS1220 SPI-Diagnose ---"));
  printHex(F("Register 0 nach Reset (erwartet 0x00): "), before);
  printHex(F("Register 0 nach Schreiben (erwartet 0x01): "), after);

  if (before == 0x00 && after == 0x01) {
    Serial.println(F("OK: SPI funktioniert. Problem liegt woanders (Bibliothek/Sketch)."));
  } else if (before == 0xFF && after == 0xFF) {
    Serial.println(F("Liest nur 0xFF: MISO (12) nicht verbunden oder Chip wird nicht"));
    Serial.println(F("angesprochen -> CS (10), MISO (12) und Versorgung pruefen."));
  } else if (before == 0x00 && after == 0x00) {
    Serial.println(F("Liest nur 0x00: Schreiben kommt nicht an oder MISO haengt auf GND"));
    Serial.println(F("-> MOSI (11) und SCLK (13) pruefen, MISO/MOSI evtl. vertauscht."));
  } else {
    Serial.println(F("Unerwartete Werte: Wackelkontakt oder vertauschte Leitungen."));
  }
  Serial.println();
  delay(500);   // alle 0,5 s wiederholen -> mit dem Oszi gut zu triggern
}
