/*
  ADS1220 Breadboard-Test – USB-Voltmeter Projekt
  ------------------------------------------------
  Misst die Spannung an AIN0 gegen AVSS (Masse) und gibt sie im
  seriellen Monitor aus: Mittelwert und Standardabweichung (Rauschen)
  über N Messungen.

  Verdrahtung (gleiche Pins wie auf dem Shield):
    ADS1220   ->  Arduino Uno
    CS        ->  D10
    DRDY      ->  D2
    MOSI      ->  D11
    MISO      ->  D12
    SCLK      ->  D13
    AVDD/DVDD ->  5V
    AGND/DGND ->  GND
    CLK       ->  GND (oder offen, Modul hat 10k Pulldown)
  Messsignal: Netzteil (+) -> AIN0, Netzteil (-) -> GND
              (Strombegrenzung 10 mA, OVP ca. 2 V)
  Achtung: AIN0 darf nur zwischen 0 und 2,048 V liegen!

  Rauschen: Die Standardabweichung wird ueber die Abweichungen vom
  Mittelwert berechnet (zwei Durchlaeufe). Die Formel E[v^2] - E[v]^2
  ist auf dem Uno unbrauchbar, weil "double" dort nur ein 32-Bit-float
  ist (~7 Stellen) -> Ausloeschung, sigma springt in 500-uV-Stufen.

  Bibliothek: "ADS1220_WE" von Wolfgang Ewald (Bibliotheksverwalter)
*/

#include <SPI.h>
#include <ADS1220_WE.h>

const uint8_t PIN_CS   = 10;
const uint8_t PIN_DRDY = 2;
const int     N        = 20;   // Messungen pro Ausgabe (bei 20 SPS ca. 1 s)

ADS1220_WE ads(PIN_CS, PIN_DRDY);

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  if (!ads.init()) {
    Serial.println(F("FEHLER: ADS1220 antwortet nicht. Verdrahtung pruefen!"));
    while (1) {}
  }

  ads.setCompareChannels(ADS1220_MUX_0_AVSS); // AIN0 gegen Masse
  // ads.setCompareChannels(ADS1220_MUX_AVDD_P_AVSS_2); // Eingaenge intern kurzgeschlossen (Offset/Rauschen des ADC allein)
  ads.bypassPGA(true);                        // fuer Messung gegen AVSS noetig
  ads.setGain(ADS1220_GAIN_1);
  ads.setVRefSource(ADS1220_VREF_INT);        // interne 2,048-V-Referenz
  ads.setDataRate(ADS1220_DR_LVL_0);          // 20 SPS (Normal Mode)
  ads.setFIRFilter(ADS1220_50HZ_60HZ);        // 50/60-Hz-Unterdrueckung
  ads.setConversionMode(ADS1220_CONTINUOUS);

  Serial.println(F("ADS1220 bereit. Mittelwert [mV] | Std.-Abw. [uV]"));
}

void loop() {
  float v[N];
  float sum = 0.0;

  for (int i = 0; i < N; i++) {
    v[i] = ads.getVoltage_mV();       // wartet intern auf DRDY
    sum += v[i];
  }
  float mean = sum / N;

  float sq = 0.0;
  for (int i = 0; i < N; i++) {
    float d = v[i] - mean;
    sq += d * d;
  }
  float sigma = sqrt(sq / (N - 1));   // Stichproben-Standardabweichung

  Serial.print(mean, 4);
  Serial.print(F(" mV  |  "));
  Serial.print(sigma * 1000.0, 2);
  Serial.println(F(" uV"));
}
