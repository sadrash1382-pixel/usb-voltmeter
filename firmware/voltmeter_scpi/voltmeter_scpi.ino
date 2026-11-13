/*
  USB-Voltmeter – SCPI-Firmware fuer Platine Rev. 1
  -------------------------------------------------
  Das Geraet misst nur auf Anfrage und antwortet wie ein Labormessgeraet.
  Schnittstelle: USB (virtueller COM-Port), 115200 Baud, 8N1.
  Befehle enden mit '\n' (LF); '\r' wird ignoriert. Antworten enden mit '\n'.
  Gross-/Kleinschreibung egal, Kurz- und Langform erlaubt
  (z. B. MEAS:VOLT:DC? = MEASure:VOLTage:DC? = measure:voltage:dc?).

  Befehle
    *IDN?                 Identifikation
    *RST                  Grundeinstellung (Mittelung 10, Kalibrierung aus EEPROM)
    *CLS                  Fehlerspeicher loeschen
    *OPC?                 antwortet 1
    *TST?                 Selbsttest: 0 = ADC antwortet korrekt, 1 = Fehler
    MEAS:VOLT:DC?         Messung, kalibriert [V]   (auch MEAS:VOLT? und READ?)
    MEAS:VOLT:DC:RAW?     Messung, unkalibriert [V]
    CALC:SDEV?            Standardabweichung der letzten Messung am Eingang [V]
    [SENS:]AVER:COUN <n>  Einzelwandlungen pro Messung, 1..100 (je ca. 70 ms)
    [SENS:]AVER:COUN?
    CAL:OFFS <V> | ?      Offset b in V_kal = a * V_roh + b
    CAL:GAIN <a> | ?      Verstaerkung a
    CAL:ZERO              Eingang kurzschliessen, dann senden: setzt b so, dass 0 V angezeigt wird
    CAL:SAVE              a und b dauerhaft im EEPROM speichern
    SYST:ERR?             aeltesten Fehler auslesen ("0,\"No error\"" wenn leer)

  Hardware-Hinweise Rev. 1 (Header-Versatz, siehe docs/inbetriebnahme-rev1.md):
    Software-SPI auf D12 (SCLK), D11 (MISO), D10 (MOSI), D9 (CS).
    P4 nur Pin 5..8 bestueckt, P5 nicht bestueckt. DRDY wird nicht benutzt.

  Hinweis: Beim Oeffnen des COM-Ports setzt sich der Uno zurueck (DTR).
  Nach dem Oeffnen ca. 2 s warten, bevor der erste Befehl gesendet wird.
*/

#include <EEPROM.h>

// ---------------- Hardware ----------------
const uint8_t PIN_CS   = 9;
const uint8_t PIN_MOSI = 10;
const uint8_t PIN_MISO = 11;
const uint8_t PIN_SCLK = 12;

const float VREF_ADC = 2.048;    // interne Referenz ADS1220 [V]
const float TEILER   = 11.0;     // (R1 + R2) / R2
const float V_FUSS   = 2.507;    // LM4040, an TP2 gemessen [V]

// ---------------- Einstellungen ----------------
const float KAL_A_DEFAULT = 1.0;
const float KAL_B_DEFAULT = -0.0171;   // Offset aus Kurzschluss (13.11.2026)
const int   N_DEFAULT     = 10;
const int   N_MAX         = 100;

const char IDN[] = "Shahanjarini,USB-Voltmeter,REV1,1.0";

float kalA = KAL_A_DEFAULT;
float kalB = KAL_B_DEFAULT;
int   nAvg = N_DEFAULT;
float lastSdev = 0.0;            // [V], am Eingang
bool  adcOk = false;

float samples[N_MAX];            // Einzelwerte V_diff [mV]

// ---------------- EEPROM ----------------
const uint16_t EE_MAGIC = 0x5643;   // "VC"
struct KalDaten { uint16_t magic; float a; float b; };

void ladeKalibrierung() {
  KalDaten k;
  EEPROM.get(0, k);
  if (k.magic == EE_MAGIC && !isnan(k.a) && !isnan(k.b)) {
    kalA = k.a; kalB = k.b;
  } else {
    kalA = KAL_A_DEFAULT; kalB = KAL_B_DEFAULT;
  }
}

void speichereKalibrierung() {
  KalDaten k = { EE_MAGIC, kalA, kalB };
  EEPROM.put(0, k);
}

// ---------------- Fehlerspeicher (SCPI-Fehlercodes) ----------------
const uint8_t ERR_MAX = 5;
int errQueue[ERR_MAX];
uint8_t errCount = 0;

void pushError(int code) {
  if (errCount < ERR_MAX) errQueue[errCount++] = code;
  else errQueue[ERR_MAX - 1] = -350;               // Queue overflow
}

const __FlashStringHelper* errText(int code) {
  switch (code) {
    case -113: return F("Undefined header");
    case -109: return F("Missing parameter");
    case -222: return F("Data out of range");
    case -223: return F("Too much data");
    case -240: return F("Hardware error");
    case -350: return F("Queue overflow");
    default:   return F("Unknown error");
  }
}

// ---------------- Software-SPI, Mode 1 ----------------
uint8_t spiByte(uint8_t out) {
  uint8_t in = 0;
  for (int8_t i = 7; i >= 0; i--) {
    digitalWrite(PIN_SCLK, HIGH);                 // ADS1220 gibt Bit auf DOUT aus
    digitalWrite(PIN_MOSI, (out >> i) & 0x01);
    delayMicroseconds(2);
    in = (in << 1) | (digitalRead(PIN_MISO) ? 1 : 0);
    digitalWrite(PIN_SCLK, LOW);                  // ADS1220 uebernimmt DIN
    delayMicroseconds(2);
  }
  return in;
}

void csLow()  { digitalWrite(PIN_CS, LOW);  delayMicroseconds(2); }
void csHigh() { delayMicroseconds(2); digitalWrite(PIN_CS, HIGH); }

void command(uint8_t c) { csLow(); spiByte(c); csHigh(); }

void writeReg(uint8_t reg, uint8_t val) {
  csLow(); spiByte(0x40 | (reg << 2)); spiByte(val); csHigh();
}

uint8_t readReg(uint8_t reg) {
  csLow(); spiByte(0x20 | (reg << 2)); uint8_t v = spiByte(0x00); csHigh();
  return v;
}

int32_t readData() {
  csLow();
  spiByte(0x10);                                  // RDATA
  int32_t b2 = spiByte(0x00), b1 = spiByte(0x00), b0 = spiByte(0x00);
  csHigh();
  int32_t code = (b2 << 16) | (b1 << 8) | b0;
  if (code & 0x800000L) code -= 0x1000000L;       // 24-Bit-Zweierkomplement -> negativ
  return code;
}

// ADC einstellen und pruefen; true = ADS1220 antwortet korrekt
bool adcInit() {
  command(0x06);                 // RESET
  delay(5);
  writeReg(0, 0x00);             // AIN0-AIN1, Gain 1, PGA an
  writeReg(1, 0x00);             // 20 SPS, Normal Mode, Single-Shot
  writeReg(2, 0x10);             // interne Referenz, 50/60 Hz simultan
  writeReg(3, 0x00);
  return readReg(0) == 0x00 && readReg(1) == 0x00 && readReg(2) == 0x10;
}

// Eine Wandlung, Ergebnis V_diff in mV
float wandlung_mV() {
  command(0x08);                 // START/SYNC
  delay(70);                     // Wandlungszeit ca. 50 ms + Reserve
  return readData() * (VREF_ADC * 1000.0 / 8388608.0);
}

// nAvg Wandlungen -> unkalibrierte Eingangsspannung [V], setzt lastSdev
float messeRoh() {
  float sum = 0.0;
  for (int i = 0; i < nAvg; i++) { samples[i] = wandlung_mV(); sum += samples[i]; }
  float mean = sum / nAvg;

  float sq = 0.0;
  for (int i = 0; i < nAvg; i++) { float d = samples[i] - mean; sq += d * d; }
  lastSdev = (nAvg > 1) ? TEILER * sqrt(sq / (nAvg - 1)) / 1000.0 : 0.0;

  return TEILER * mean / 1000.0 + V_FUSS;
}

// ---------------- Ausgabe ----------------
void antwortZahl(float v, uint8_t stellen) {
  if (isnan(v)) { Serial.print(F("9.91E+37\n")); return; }   // SCPI "NaN"
  if (v >= 0) Serial.print('+');
  Serial.print(v, stellen);
  Serial.print('\n');
}

// ---------------- SCPI-Parser ----------------
// Kurzform <-> Langform der Schluesselwoerter
const char* const KW_LANG[]  = { "MEASURE", "VOLTAGE", "SENSE", "AVERAGE", "COUNT",
                                 "CALIBRATION", "OFFSET", "SYSTEM", "ERROR",
                                 "CALCULATE", "SDEVIATION" };
const char* const KW_KURZ[]  = { "MEAS", "VOLT", "SENS", "AVER", "COUN",
                                 "CAL", "OFFS", "SYST", "ERR",
                                 "CALC", "SDEV" };
const uint8_t KW_ANZ = sizeof(KW_LANG) / sizeof(KW_LANG[0]);

// Header (z. B. "MEASURE:VOLTAGE:DC?") auf Kurzform bringen -> "MEAS:VOLT:DC?"
void normalisiere(const char* header, char* out, size_t outLen) {
  size_t o = 0;
  const char* p = header;
  if (*p == ':') p++;                              // fuehrender Doppelpunkt optional
  while (*p && o < outLen - 1) {
    char tok[16]; uint8_t t = 0;
    while (*p && *p != ':' && *p != '?' && t < sizeof(tok) - 1) tok[t++] = *p++;
    tok[t] = '\0';
    const char* ersatz = tok;
    for (uint8_t k = 0; k < KW_ANZ; k++) {
      if (strcmp(tok, KW_LANG[k]) == 0 || strcmp(tok, KW_KURZ[k]) == 0) { ersatz = KW_KURZ[k]; break; }
    }
    for (const char* e = ersatz; *e && o < outLen - 1; e++) out[o++] = *e;
    if (*p == ':' || *p == '?') { if (o < outLen - 1) out[o++] = *p; p++; }
  }
  out[o] = '\0';
  // optionalen Knoten SENS: entfernen ("SENS:AVER:COUN" == "AVER:COUN")
  if (strncmp(out, "SENS:", 5) == 0) memmove(out, out + 5, strlen(out + 5) + 1);
}

bool hatParameter(const char* param) {
  if (param == NULL || *param == '\0') { pushError(-109); return false; }
  return true;
}

void bearbeite(char* zeile) {
  // in Grossbuchstaben wandeln
  for (char* c = zeile; *c; c++) *c = toupper(*c);

  // Header und Parameter trennen
  char* param = strchr(zeile, ' ');
  if (param) { *param++ = '\0'; while (*param == ' ') param++; }

  char h[40];
  normalisiere(zeile, h, sizeof(h));

  if      (!strcmp(h, "*IDN?")) { Serial.print(IDN); Serial.print('\n'); }
  else if (!strcmp(h, "*RST"))  { nAvg = N_DEFAULT; ladeKalibrierung(); adcOk = adcInit(); }
  else if (!strcmp(h, "*CLS"))  { errCount = 0; }
  else if (!strcmp(h, "*OPC?")) { Serial.print(F("1\n")); }
  else if (!strcmp(h, "*TST?")) { adcOk = adcInit(); Serial.print(adcOk ? F("0\n") : F("1\n")); }

  else if (!strcmp(h, "MEAS:VOLT:DC?") || !strcmp(h, "MEAS:VOLT?") || !strcmp(h, "READ?")) {
    if (!adcOk) { pushError(-240); antwortZahl(NAN, 0); }
    else antwortZahl(kalA * messeRoh() + kalB, 6);
  }
  else if (!strcmp(h, "MEAS:VOLT:DC:RAW?")) {
    if (!adcOk) { pushError(-240); antwortZahl(NAN, 0); }
    else antwortZahl(messeRoh(), 6);
  }
  else if (!strcmp(h, "CALC:SDEV?")) { antwortZahl(lastSdev, 7); }

  else if (!strcmp(h, "AVER:COUN")) {
    if (!hatParameter(param)) return;
    int n = atoi(param);
    if (n < 1 || n > N_MAX) pushError(-222); else nAvg = n;
  }
  else if (!strcmp(h, "AVER:COUN?")) { Serial.print(nAvg); Serial.print('\n'); }

  else if (!strcmp(h, "CAL:OFFS"))  { if (hatParameter(param)) kalB = atof(param); }
  else if (!strcmp(h, "CAL:OFFS?")) { antwortZahl(kalB, 6); }
  else if (!strcmp(h, "CAL:GAIN")) {
    if (!hatParameter(param)) return;
    float a = atof(param);
    if (a < 0.9 || a > 1.1) pushError(-222); else kalA = a;
  }
  else if (!strcmp(h, "CAL:GAIN?")) { antwortZahl(kalA, 6); }
  else if (!strcmp(h, "CAL:ZERO")) {
    if (!adcOk) { pushError(-240); return; }
    kalB = -kalA * messeRoh();
  }
  else if (!strcmp(h, "CAL:SAVE")) { speichereKalibrierung(); }

  else if (!strcmp(h, "SYST:ERR?") || !strcmp(h, "SYST:ERR:NEXT?")) {
    if (errCount == 0) { Serial.print(F("0,\"No error\"\n")); return; }
    int code = errQueue[0];
    for (uint8_t i = 1; i < errCount; i++) errQueue[i - 1] = errQueue[i];
    errCount--;
    Serial.print(code); Serial.print(F(",\"")); Serial.print(errText(code)); Serial.print(F("\"\n"));
  }
  else {
    pushError(-113);
  }
}

// ---------------- Arduino ----------------
const uint8_t ZEILE_MAX = 64;
char zeile[ZEILE_MAX];
uint8_t laenge = 0;
bool ueberlauf = false;

void setup() {
  pinMode(PIN_CS, OUTPUT);   digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_SCLK, OUTPUT); digitalWrite(PIN_SCLK, LOW);
  pinMode(PIN_MOSI, OUTPUT); digitalWrite(PIN_MOSI, LOW);
  pinMode(PIN_MISO, INPUT);

  Serial.begin(115200);
  delay(50);

  ladeKalibrierung();
  adcOk = adcInit();
  if (!adcOk) pushError(-240);
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (ueberlauf) pushError(-223);
      else if (laenge > 0) { zeile[laenge] = '\0'; bearbeite(zeile); }
      laenge = 0; ueberlauf = false;
    } else if (laenge < ZEILE_MAX - 1) {
      zeile[laenge++] = c;
    } else {
      ueberlauf = true;
    }
  }
}
