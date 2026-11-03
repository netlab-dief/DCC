#!/usr/bin/env python3
"""Read-only ncurses dashboard for the DCC and CAM CSV telemetry."""

import argparse
from collections import deque
import csv
from datetime import datetime
import math
import os
from pathlib import Path
import sys
import time


class CsvTail:
    """Follow complete CSV records, including file creation, rotation and truncation.

    Memory and work per poll are bounded; old files start at their last 256 KiB.
    The producers use single-line records without embedded newlines.
    """

    LIMIT = 256 * 1024

    def __init__(self, path, required):
        self.path = Path(path)
        self.required = set(required)
        self.identity = None
        self.offset = 0
        self.header = None
        self.latest = None
        self.error = None

    def poll(self):
        rows = []
        try:
            with self.path.open('rb') as stream:
                stat = os.fstat(stream.fileno())
                identity = (stat.st_dev, stat.st_ino)
                if identity != self.identity or stat.st_size < self.offset:
                    self.identity = identity
                    self.offset = 0
                    self.header = None
                    self.latest = None
                if self.header is None:
                    line = stream.readline(self.LIMIT)
                    if not line.endswith(b'\n'):
                        return rows
                    self.header = next(csv.reader([line.decode('utf-8-sig')]))
                    if not self.required.issubset(self.header):
                        self.header = None
                        raise ValueError('schema CSV non compatibile')
                    self.offset = stream.tell()
                    if stat.st_size - self.offset > self.LIMIT:
                        stream.seek(stat.st_size - self.LIMIT)
                        stream.readline()  # discard potentially incomplete leading record
                        self.offset = stream.tell()
                stream.seek(self.offset)
                block = stream.read(self.LIMIT)
                complete = block.rfind(b'\n') + 1
                self.offset += complete
                for line in block[:complete].decode('utf-8').splitlines():
                    values = next(csv.reader([line]))
                    if len(values) == len(self.header):
                        row = dict(zip(self.header, values))
                        self.latest = row
                        rows.append(row)
                self.error = None
        except (OSError, ValueError, csv.Error) as error:
            self.error = str(error)
        return rows


def number(row, key):
    try:
        value = float(row[key])
        return value if math.isfinite(value) else None
    except (KeyError, TypeError, ValueError):
        return None


def age(row, cam=False, now=None):
    try:
        stamp = (float(row['Timestamp_Unix_ms']) / 1000 if cam else
                 datetime.fromisoformat(row['Timestamp']).timestamp())
        value = (time.time() if now is None else now) - stamp
        return value if math.isfinite(value) else None
    except (KeyError, TypeError, ValueError, OverflowError):
        return None


def status(tail, cam=False):
    if tail.error:
        return 'IN ATTESA / FILE NON DISPONIBILE', False
    elapsed = age(tail.latest, cam)
    if elapsed is None:
        return 'IN ATTESA DI DATI', False
    if elapsed < -1:
        return 'OROLOGIO DEL LOG NEL FUTURO', False
    fresh = elapsed <= (3 if cam else 1)
    return ('AGGIORNATO' if fresh else 'DATI FERMI') + f' | ultimo campione {max(0, elapsed):.1f} s fa', fresh


def metric(row, key, unit=''):
    value = number(row, key)
    return '--' if value is None else f'{value:g}{unit}'


def run_screen(screen, directory):
    import curses
    curses.curs_set(0)
    screen.timeout(250)
    if curses.has_colors():
        curses.start_color()
        curses.init_pair(1, curses.COLOR_GREEN, curses.COLOR_BLACK)
        curses.init_pair(2, curses.COLOR_YELLOW, curses.COLOR_BLACK)
    dcc = CsvTail(directory / 'dcc_telemetry_log.csv',
                  ['Timestamp', 'CBR_Percent', 'State_Mode', 'Interval_ms', 'Tx_Power_dBm', 'MCS_Netlink'])
    cam = CsvTail(directory / 'cam_telemetry_estimated_log.csv',
                  ['Session_Unix_ms', 'Timestamp_Unix_ms', 'Submitted', 'Submitted_Hz'])
    history = deque(maxlen=1200)  # about two minutes of reactive DCC samples
    while True:
        previous_identity = dcc.identity
        previous_offset = dcc.offset
        rows = dcc.poll()
        if not dcc.latest or previous_identity != dcc.identity or dcc.offset < previous_offset:
            history.clear()
        for row in rows:
            cbr = number(row, 'CBR_Percent')
            if cbr is not None and 0 <= cbr <= 100:
                history.append((row['Timestamp'], cbr))
        cam.poll()
        screen.erase()
        height, width = screen.getmaxyx()

        def put(y, text, attr=0):
            if 0 <= y < height and width > 1:
                # Prevent log contents from injecting terminal control characters.
                clean = ''.join(c if c.isprintable() else ' ' for c in str(text))
                try:
                    screen.addnstr(y, 1, clean, width - 2, attr)
                except curses.error:
                    pass  # terminal may have resized between calls

        put(0, 'DCC / CAM  |  DASHBOARD', curses.A_BOLD)
        put(1, f'Log: {directory}')
        if height < 24 or width < 72:
            put(3, 'Allarga il terminale ad almeno 72 colonne e 24 righe.')
        else:
            dcc_status, fresh = status(dcc)
            color = curses.color_pair(1 if fresh else 2) if curses.has_colors() else 0
            put(3, f'DCC  {dcc_status}', color)
            data = dcc.latest or {}
            put(4, f"Stato / modalita: {data.get('State_Mode', '--')}    CBR: {metric(data, 'CBR_Percent', '%')}")
            put(5, f"Intervallo indicativo: {metric(data, 'Interval_ms', ' ms')}")
            put(6, f"Potenza richiesta: {metric(data, 'Tx_Power_dBm', ' dBm')}    Bitrate richiesto: {metric(data, 'MCS_Netlink', ' Mb/s')}")
            put(8, 'CBR 0-100% | ultimi campioni (sinistra -> destra)')
            samples = list(history)[-(width - 3):]
            for y in range(4):
                threshold = (4 - y) * 25
                put(9 + y, ''.join('#' if value >= threshold else ' ' for _, value in samples))
            if samples:
                put(13, f'{samples[0][0]}  ->  {samples[-1][0]}')
            cam_status, cam_fresh = status(cam, True)
            put(15, f'CAM  {cam_status}', curses.color_pair(1 if cam_fresh else 2) if curses.has_colors() else 0)
            data = cam.latest or {}
            put(16, f"Generati: {metric(data, 'Generated')}   Accodati al driver: {metric(data, 'Submitted')}   Frequenza: {metric(data, 'Submitted_Hz', ' Hz')}")
            put(17, f"Sostituiti: {metric(data, 'Replaced')}   Scaduti: {metric(data, 'Expired')}   Invalidati: {metric(data, 'Invalidated')}   Errori TX: {metric(data, 'TX_Errors')}")
            put(18, f"Attese medie - Coda: {metric(data, 'Queue_Mean_ms', ' ms')}   Gate: {metric(data, 'Gate_Mean_ms', ' ms')}   Invio: {metric(data, 'Send_Mean_ms', ' ms')}")
            put(19, f"Airtime stimato cumulativo: {metric(data, 'Estimated_Airtime_us', ' us')}")
            put(21, 'Dati radio richiesti e airtime stimato; nessuna conferma PHY.')
            if dcc.error or cam.error:
                put(22, 'File mancanti o illeggibili: attendo la telemetria del processo DCC.')
        put(height - 1, 'q / Esc: chiudi dashboard | Ctrl+C: chiudi dashboard', curses.A_DIM)
        screen.refresh()
        if screen.getch() in (ord('q'), ord('Q'), 27):
            return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log-dir', type=Path, default=Path('.'),
                        help='directory dei CSV DCC/CAM (default: directory corrente)')
    options = parser.parse_args()
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        parser.exit(1, 'La dashboard richiede un terminale interattivo.\n')
    try:
        import curses
    except ImportError as error:
        parser.exit(1, f'Modulo curses non disponibile: {error}\n')
    try:
        curses.wrapper(run_screen, options.log_dir.resolve())
    except KeyboardInterrupt:
        pass
    except curses.error as error:
        parser.exit(1, f'Impossibile avviare ncurses: {error}\n')


if __name__ == '__main__':
    main()
