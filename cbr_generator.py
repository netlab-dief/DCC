#!/usr/bin/env python3
"""
cbr_trace_generator.py

Genera una traccia CBR sintetica e la scrive periodicamente in un file
(default: "cbr"), da usare al posto della lettura reale da CBRReader per
testare in modo controllato e riproducibile l'isteresi del DCCController.

La traccia e' composta da 4 fasi consecutive:
  1. stabile basso        -> verifica stato RELAXED stabile
  2. rampa lenta verso soglia -> transizione "pulita" attesa
  3. oscillazione attorno alla soglia (onda triangolare/sinusoidale)
     -> qui ti aspetti oscillazione SENZA isteresi, stabilita' CON isteresi
  4. step netto oltre soglia -> verifica tempo di convergenza

Scrittura ATOMICA (file temporaneo + os.replace) per evitare che il lettore
C++ legga un valore a meta' scritto.

Uso base:
    python3 cbr_trace_generator.py

Uso con parametri custom:
    python3 cbr_trace_generator.py --threshold 0.4 --amplitude 0.04 \
        --interval 0.15 --output cbr --noise-std 0.01

Ogni riga stampata a schermo (e ogni valore scritto su file) e' anche
loggata su un CSV (default: cbr_trace_log.csv) con timestamp assoluto,
tempo relativo, fase corrente e valore, cosi' hai gia' pronto il dato
per i grafici A/B (con e senza isteresi) usando la STESSA traccia.
"""

import argparse
import csv
import math
import os
import random
import tempfile
import time
from datetime import datetime


def build_phases(threshold, amplitude, args):
    """
    Ritorna la lista di fasi come tuple:
        (nome, durata_s, funzione(t_nella_fase) -> valore_cbr)
    t_nella_fase e' il tempo in secondi trascorso dall'inizio della fase.
    """
    low_value = max(0.0, threshold - args.low_offset)
    high_value = min(1.0, threshold + args.step_offset)

    def phase_stable_low(t):
        return low_value

    def phase_ramp(t):
        # rampa lineare da low_value a threshold in ramp_duration secondi
        frac = min(1.0, t / args.ramp_duration) if args.ramp_duration > 0 else 1.0
        return low_value + frac * (threshold - low_value)

    def phase_critical_oscillation(t):
        # onda sinusoidale centrata sulla soglia, ampiezza costante
        period = args.osc_period
        return threshold + amplitude * math.sin(2 * math.pi * t / period)

    def phase_step(t):
        return high_value

    phases = [
        ("stable_low", args.stable_duration, phase_stable_low),
        ("ramp_to_threshold", args.ramp_duration, phase_ramp),
        ("critical_oscillation", args.osc_duration, phase_critical_oscillation),
        ("step_beyond_threshold", args.step_duration, phase_step),
    ]
    return phases


def atomic_write(path, text):
    """Scrive 'text' su 'path' in modo atomico (tmpfile + os.replace)."""
    directory = os.path.dirname(os.path.abspath(path)) or "."
    fd, tmp_path = tempfile.mkstemp(dir=directory, prefix=".cbr_tmp_")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.replace(tmp_path, path)  # rename atomico sullo stesso filesystem
    except Exception:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", default="cbr",
                         help="File su cui scrivere il valore CBR corrente (default: cbr)")
    parser.add_argument("--log", default="cbr_trace_log.csv",
                         help="CSV di log con timestamp/fase/valore (default: cbr_trace_log.csv)")
    parser.add_argument("--interval", type=float, default=0.15,
                         help="Intervallo di aggiornamento in secondi (default: 0.15 = 150ms)")
    parser.add_argument("--threshold", type=float, default=0.4,
                         help="Valore di soglia CBR attorno a cui costruire la traccia (default: 0.4)")
    parser.add_argument("--amplitude", type=float, default=0.03,
                         help="Ampiezza oscillazione attorno alla soglia nella fase critica (default: 0.03)")
    parser.add_argument("--osc-period", type=float, default=4.0,
                         help="Periodo dell'oscillazione in secondi nella fase critica (default: 4.0)")
    parser.add_argument("--noise-std", type=float, default=0.0,
                         help="Deviazione standard di rumore gaussiano sovrapposto (default: 0.0, disattivo)")
    parser.add_argument("--low-offset", type=float, default=0.25,
                         help="Distanza sotto soglia per il valore 'basso stabile' (default: 0.25)")
    parser.add_argument("--step-offset", type=float, default=0.25,
                         help="Distanza sopra soglia per lo step finale (default: 0.25)")
    parser.add_argument("--stable-duration", type=float, default=30.0,
                         help="Durata fase 1 - stabile basso, secondi (default: 30)")
    parser.add_argument("--ramp-duration", type=float, default=30.0,
                         help="Durata fase 2 - rampa verso soglia, secondi (default: 30)")
    parser.add_argument("--osc-duration", type=float, default=180.0,
                         help="Durata fase 3 - oscillazione critica, secondi (default: 180 = 3 min)")
    parser.add_argument("--step-duration", type=float, default=30.0,
                         help="Durata fase 4 - step netto oltre soglia, secondi (default: 30)")
    parser.add_argument("--seed", type=int, default=None,
                         help="Seed per il rumore casuale, utile per riprodurre ESATTAMENTE la stessa traccia tra run A e B")
    args = parser.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    phases = build_phases(args.threshold, args.amplitude, args)
    total_duration = sum(d for _, d, _ in phases)

    print(f"[cbr_trace_generator] output={args.output} log={args.log} "
          f"interval={args.interval}s durata_totale={total_duration:.0f}s")
    print("[cbr_trace_generator] fasi:")
    for name, duration, _ in phases:
        print(f"  - {name}: {duration:.0f}s")
    print("[cbr_trace_generator] Ctrl+C per interrompere.\n")

    log_file = open(args.log, "w", newline="")
    csv_writer = csv.writer(log_file)
    csv_writer.writerow(["timestamp_iso", "t_rel_s", "phase", "cbr_value"])

    t_start = time.monotonic()
    try:
        for phase_name, phase_duration, phase_fn in phases:
            phase_t0 = time.monotonic()
            while True:
                now = time.monotonic()
                t_in_phase = now - phase_t0
                if t_in_phase >= phase_duration:
                    break

                value = phase_fn(t_in_phase)
                if args.noise_std > 0:
                    value += random.gauss(0.0, args.noise_std)
                value = min(1.0, max(0.0, value))  # clamp in [0,1]

                atomic_write(args.output, f"{value:.4f}\n")

                t_rel = now - t_start
                csv_writer.writerow([datetime.now().isoformat(), f"{t_rel:.3f}",
                                      phase_name, f"{value:.4f}"])
                log_file.flush()

                print(f"\r[{phase_name:>22s}] t={t_rel:6.1f}s  CBR={value:.4f}", end="")

                time.sleep(args.interval)
        print("\n[cbr_trace_generator] Traccia completata.")
    except KeyboardInterrupt:
        print("\n[cbr_trace_generator] Interrotto dall'utente.")
    finally:
        log_file.close()


if __name__ == "__main__":
    main()
