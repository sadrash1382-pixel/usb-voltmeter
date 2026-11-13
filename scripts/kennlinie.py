#!/usr/bin/env python3
"""
Automatische Kennlinienmessung fuer das USB-Voltmeter.

Das Labornetzteil (OWON SPE-Serie) faehrt eine Spannungsrampe, das USB-Voltmeter
misst bei jedem Schritt. Beide Geraete werden per SCPI ueber PyVISA gesteuert.
Ergebnis: CSV mit allen Werten und ein Plot der Linearitaetsabweichung.

Auswertung: Durch die Messpunkte (Voltmeter ueber Netzteil-Sollwert) wird eine
Gerade gelegt. Die Restabweichung von dieser Geraden ist die Nichtlinearitaet.
Dafuer muss das Netzteil nicht genau sein, nur linear. Steigung und Achsenabschnitt
der Geraden zeigen zusaetzlich den Verstaerkungs- und Offsetunterschied der beiden
Geraete, sind aber nur so genau wie das Netzteil selbst.

Bezug ist der SOLLWERT des Netzteils, nicht sein eigener Messwert (MEAS:VOLT?):
Beim SPE3051 hat der interne Messwert nur 10 mV Aufloesung und einen saegezahn-
foermigen Fehler bis ca. -50 mV (Messung vom 13.11.2026). Der Messwert wird zur
Information trotzdem mit gespeichert. Mit --ref netzteil laesst er sich als
Bezug waehlen.

Installation (einmalig):
    pip install pyvisa pyvisa-py pyserial numpy matplotlib

Beispiele:
    python kennlinie.py --list                         # COM-Ports und *IDN? anzeigen
    python kennlinie.py --psu ASRL5::INSTR --dmm ASRL7::INSTR
    python kennlinie.py --psu ASRL5::INSTR --dmm ASRL7::INSTR --stop 9 --step 0.25
    python kennlinie.py --simulate                     # ohne Hardware testen

Hinweis: Den seriellen Monitor der Arduino IDE vorher schliessen, sonst ist der
COM-Port belegt.
"""

import argparse
import csv
import datetime
import pathlib
import random
import sys
import time

import numpy as np

BAUD = 115200
U_MAX = 10.0            # Messbereich des Voltmeters: nie mehr als 10 V anlegen


# --------------------------------------------------------------------------
# Geraete
# --------------------------------------------------------------------------
class ScpiGeraet:
    """Duenne Huelle um eine PyVISA-Ressource an einem (virtuellen) COM-Port."""

    def __init__(self, rm, resource, reset_wait=0.0, timeout_ms=5000):
        self.rm, self.resource = rm, resource
        self.reset_wait, self.timeout_ms = reset_wait, timeout_ms
        self._oeffnen()

    def _oeffnen(self):
        self.inst = self.rm.open_resource(
            self.resource,
            baud_rate=BAUD,
            write_termination="\r\n",   # Netzteil braucht CRLF, Voltmeter ignoriert CR
            read_termination="\n",
            timeout=self.timeout_ms,
        )
        if self.reset_wait:
            time.sleep(self.reset_wait)  # Arduino setzt sich beim Oeffnen des Ports zurueck

    def neu_verbinden(self):
        """Port schliessen und neu oeffnen, z. B. nach einem USB-Abbruch."""
        try:
            self.inst.close()
        except Exception:
            pass
        time.sleep(1.0)
        self._oeffnen()

    def write(self, cmd):
        self.inst.write(cmd)

    def query(self, cmd):
        return self.inst.query(cmd).strip()

    def set_timeout(self, ms):
        self.timeout_ms = ms
        self.inst.timeout = ms

    def close(self):
        self.inst.close()


class SimNetzteil:
    """Ersatz fuer das Netzteil beim Test ohne Hardware."""

    def __init__(self):
        self.u = 0.0
        self.an = False

    def write(self, cmd):
        teile = cmd.split()
        if teile[0] == "VOLT":
            self.u = float(teile[1])
        elif teile[0] == "OUTP":
            self.an = teile[1] == "ON"

    def query(self, cmd):
        if cmd == "*IDN?":
            return "OWON,SPE3051,SIMULATION,0.0"
        if cmd == "MEAS:VOLT?":
            return f"{self.u * 1.0004 + 0.002 if self.an else 0.0:.3f}"
        if cmd == "MEAS:CURR?":
            return "0.000"
        return ""

    def set_timeout(self, ms):
        pass

    def close(self):
        pass


class SimVoltmeter:
    """Ersatz fuer das USB-Voltmeter beim Test ohne Hardware."""

    def __init__(self, netzteil):
        self.nt = netzteil
        self.sdev = 0.0

    def neu_verbinden(self):
        pass

    def write(self, cmd):
        pass

    def query(self, cmd):
        if cmd == "*IDN?":
            return "Shahanjarini,USB-Voltmeter,REV1,SIMULATION"
        if cmd == "MEAS:VOLT:DC?":
            u = self.nt.u if self.nt.an else 0.0
            self.sdev = 80e-6
            return f"{u * 0.9983 + 1e-4 * np.sin(u) + random.gauss(0, 25e-6):+.6f}"
        if cmd == "CALC:SDEV?":
            return f"{self.sdev:+.7f}"
        if cmd == "SYST:ERR?":
            return '0,"No error"'
        return ""

    def set_timeout(self, ms):
        pass

    def close(self):
        pass


# --------------------------------------------------------------------------
# Hilfsfunktionen
# --------------------------------------------------------------------------
def liste_ports():
    import pyvisa
    rm = pyvisa.ResourceManager("@py")
    ressourcen = [r for r in rm.list_resources() if r.startswith("ASRL")]
    if not ressourcen:
        print("Keine seriellen Ports gefunden.")
        return
    for r in ressourcen:
        try:
            g = ScpiGeraet(rm, r, reset_wait=2.5, timeout_ms=1500)
            idn = g.query("*IDN?")
            g.close()
        except Exception as e:      # Port belegt, kein SCPI-Geraet, ...
            idn = f"(keine Antwort: {type(e).__name__})"
        print(f"{r:20s} {idn}")


_RM = None


def resource_manager():
    global _RM
    if _RM is None:
        import pyvisa
        _RM = pyvisa.ResourceManager("@py")
    return _RM


def oeffne_netzteil(args):
    nt = SimNetzteil() if args.simulate else ScpiGeraet(resource_manager(), args.psu)
    idn = nt.query("*IDN?")
    print(f"Netzteil:  {idn}")
    if "SPE" not in idn.upper():
        print("Warnung: --psu meldet sich nicht als OWON SPE. Befehle koennten abweichen.")
    return nt


def oeffne_voltmeter(args, nt):
    dmm = SimVoltmeter(nt) if args.simulate else ScpiGeraet(resource_manager(), args.dmm, reset_wait=2.5)
    idn = dmm.query("*IDN?")
    print(f"Voltmeter: {idn}")
    if "USB-Voltmeter" not in idn:
        raise SystemExit("Abbruch: --dmm ist nicht das USB-Voltmeter. Ports vertauscht?")
    dmm.write(f"AVER:COUN {args.avg}")
    dmm.set_timeout(int(args.avg * 80 + 3000))     # ~70 ms je Wandlung + Reserve
    return dmm


def dmm_messen(dmm, avg):
    """Eine Messung am Voltmeter. Bei Timeout: COM-Port neu oeffnen (falls die
    USB-Verbindung kurz weg war), Mittelung neu setzen und einmal wiederholen."""
    for versuch in (1, 2):
        try:
            u = float(dmm.query("MEAS:VOLT:DC?"))
            sdev = float(dmm.query("CALC:SDEV?"))
            return u, sdev
        except Exception as e:
            if versuch == 2:
                raise
            print(f"  ! Voltmeter antwortet nicht ({type(e).__name__}) - verbinde neu ...")
            dmm.neu_verbinden()
            print(f"  ! Voltmeter wieder da: {dmm.query('*IDN?')}")
            dmm.write(f"AVER:COUN {avg}")


def auswerten(daten, ref="soll"):
    u_ref = np.array([d["u_soll_V" if ref == "soll" else "u_netzteil_V"] for d in daten])
    u_dut = np.array([d["u_voltmeter_V"] for d in daten])
    m, c = np.polyfit(u_ref, u_dut, 1)
    rest = u_dut - (m * u_ref + c)
    for d, r in zip(daten, rest):
        d["rest_mV"] = r * 1000
    return m, c, rest


def speichere_csv(pfad, daten, args, m, c):
    bezug = "u_soll" if args.ref == "soll" else "u_netzteil"
    felder = ["u_soll_V", "u_netzteil_V", "u_voltmeter_V", "sdev_voltmeter_uV", "rest_mV"]
    with open(pfad, "w", newline="") as f:
        f.write(f"# Kennlinie USB-Voltmeter, {datetime.datetime.now():%Y-%m-%d %H:%M}\n")
        f.write(f"# Mittelung {args.avg} Wandlungen, Einschwingzeit {args.settle} s\n")
        f.write(f"# Ausgleichsgerade u_voltmeter = {m:.6f} * {bezug} + {c * 1000:.3f} mV\n")
        f.write("# rest_mV = Abweichung von der Ausgleichsgeraden (Nichtlinearitaet)\n")
        w = csv.DictWriter(f, fieldnames=felder)
        w.writeheader()
        for d in daten:
            w.writerow({k: (f"{d[k]:.6f}" if isinstance(d[k], float) else d[k]) for k in felder})


def speichere_plot(pfad, daten, m, c, ref="soll"):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    ink, muted, grid, mark = "#1f2328", "#6e7781", "#e6e8eb", "#2f6fb3"
    u = [d["u_soll_V" if ref == "soll" else "u_netzteil_V"] for d in daten]
    r = [d["rest_mV"] for d in daten]
    s = [d["sdev_voltmeter_uV"] / 1000 for d in daten]

    fig, ax = plt.subplots(figsize=(7, 4), dpi=150)
    ax.axhline(0, color=muted, lw=1)
    ax.errorbar(u, r, yerr=s, fmt="o-", color=mark, lw=2, ms=6, mec="white", mew=1.5,
                elinewidth=1.5, capsize=3, zorder=3)
    ax.set_xlabel("Netzteil-Sollwert [V]" if ref == "soll" else "Netzteil-Messwert [V]", color=ink)
    ax.set_ylabel("Abweichung von der Geraden [mV]", color=ink)
    ax.set_title("Linearität USB-Voltmeter (Restabweichung nach Geradenausgleich)",
                 color=ink, fontsize=10, loc="left")
    ax.grid(True, color=grid, lw=0.8)
    ax.set_axisbelow(True)
    for sp in ("top", "right"):
        ax.spines[sp].set_visible(False)
    for sp in ("left", "bottom"):
        ax.spines[sp].set_color(muted)
    ax.tick_params(colors=muted)
    fig.text(0.01, 0.01,
             f"Gerade: U_Voltmeter = {m:.5f} · U_Netzteil,{'soll' if ref == 'soll' else 'mess'} {c * 1000:+.2f} mV   |   "
             f"Fehlerbalken: Rauschen σ des Voltmeters",
             color=muted, fontsize=8)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(pfad, facecolor="white")


# --------------------------------------------------------------------------
# Messung
# --------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser(description="Kennlinie USB-Voltmeter gegen Labornetzteil")
    p.add_argument("--list", action="store_true", help="COM-Ports mit *IDN? anzeigen")
    p.add_argument("--psu", help="VISA-Ressource Netzteil, z. B. ASRL5::INSTR")
    p.add_argument("--dmm", help="VISA-Ressource Voltmeter, z. B. ASRL7::INSTR")
    p.add_argument("--start", type=float, default=0.0, help="Startspannung [V]")
    p.add_argument("--stop", type=float, default=9.0, help="Endspannung [V], max. 10")
    p.add_argument("--step", type=float, default=0.5, help="Schrittweite [V]")
    p.add_argument("--ilim", type=float, default=0.01, help="Strombegrenzung [A]")
    p.add_argument("--avg", type=int, default=10, help="Wandlungen pro Messpunkt (1..100)")
    p.add_argument("--settle", type=float, default=1.0, help="Wartezeit nach Spannungswechsel [s]")
    p.add_argument("--out", default=".", help="Ordner fuer CSV und PNG")
    p.add_argument("--ref", choices=["soll", "netzteil"], default="soll",
                   help="Bezug fuer die Auswertung: Sollwert (Standard) oder Messwert des Netzteils")
    p.add_argument("--simulate", action="store_true", help="ohne Hardware testen")
    args = p.parse_args()

    if args.list:
        liste_ports()
        return
    if not args.simulate and not (args.psu and args.dmm):
        p.error("--psu und --dmm angeben (oder --list bzw. --simulate)")
    if not (0 <= args.start < args.stop <= U_MAX):
        p.error(f"Spannungen muessen 0 <= start < stop <= {U_MAX} V erfuellen")
    if not 1 <= args.avg <= 100:
        p.error("--avg muss zwischen 1 und 100 liegen")

    nt = oeffne_netzteil(args)
    dmm = None
    daten = []
    try:
        # Netzteil zuerst vorbereiten und einschalten. Erst danach wird das Voltmeter
        # verbunden, damit ein Stoerimpuls beim Einschalten des Ausgangs die
        # USB-Verbindung zum Uno nicht unterbricht.
        nt.write("OUTP OFF")
        nt.write(f"CURR {args.ilim:.3f}")
        nt.write(f"VOLT {args.start:.3f}")
        try:
            nt.write(f"VOLT:LIMIT {min(args.stop + 0.5, U_MAX + 0.5):.2f}")   # Spannungsgrenze
        except Exception:
            print("Hinweis: Spannungsgrenze (VOLT:LIMIT) nicht gesetzt.")
        nt.write("OUTP ON")
        time.sleep(2)                      # Ausgang einschwingen lassen

        dmm = oeffne_voltmeter(args, nt)

        punkte = np.arange(args.start, args.stop + args.step / 2, args.step)
        print(f"\n{len(punkte)} Messpunkte, ca. "
              f"{len(punkte) * (args.settle + args.avg * 0.075 + 0.3):.0f} s\n")
        print(f"{'Soll':>7} {'Netzteil':>10} {'Voltmeter':>11} {'sigma':>9}")

        for u_soll in punkte:
            nt.write(f"VOLT {u_soll:.3f}")
            time.sleep(args.settle)
            u_nt = float(nt.query("MEAS:VOLT?"))
            u_dmm, sdev = dmm_messen(dmm, args.avg)
            daten.append({"u_soll_V": float(u_soll), "u_netzteil_V": u_nt,
                          "u_voltmeter_V": u_dmm, "sdev_voltmeter_uV": sdev * 1e6})
            print(f"{u_soll:7.3f} {u_nt:10.3f} {u_dmm:11.6f} {sdev * 1e6:7.1f} uV")

        fehler = dmm.query("SYST:ERR?")
        if not fehler.startswith("0"):
            print(f"Voltmeter meldet Fehler: {fehler}")

    except KeyboardInterrupt:
        print("\nAbgebrochen.")
    finally:
        # Netzteil immer sicher abschalten
        try:
            nt.write("VOLT 0")
            nt.write("OUTP OFF")
        finally:
            nt.close()
            if dmm is not None:
                dmm.close()

    if len(daten) < 3:
        print("Zu wenige Messpunkte fuer eine Auswertung.")
        return

    m, c, rest = auswerten(daten, args.ref)
    bezug = "Sollwert" if args.ref == "soll" else "Messwert"
    print(f"\nAusgleichsgerade (Bezug Netzteil-{bezug}): U_Voltmeter = {m:.6f} * U {c * 1000:+.3f} mV")
    print(f"  Steigungsunterschied zum Netzteil: {(m - 1) * 100:+.3f} %")
    print(f"  Nichtlinearitaet: max. |Rest| = {np.max(np.abs(rest)) * 1000:.3f} mV, "
          f"RMS = {np.sqrt(np.mean(rest ** 2)) * 1000:.3f} mV")

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H%M")
    csv_pfad = out / f"{stamp}_kennlinie_auto.csv"
    png_pfad = out / f"{stamp}_kennlinie_auto.png"
    speichere_csv(csv_pfad, daten, args, m, c)
    speichere_plot(png_pfad, daten, m, c, args.ref)
    print(f"\nGespeichert: {csv_pfad}\n             {png_pfad}")


if __name__ == "__main__":
    main()
