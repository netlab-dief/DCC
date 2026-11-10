import tempfile
from pathlib import Path
import unittest

from tools.dashboard import CsvTail, age, number, status, net_lines
from datetime import datetime


class DashboardTests(unittest.TestCase):
    def test_follows_complete_records_and_late_creation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'log.csv'
            tail = CsvTail(path, ['Timestamp', 'CBR_Percent'])
            self.assertEqual(tail.poll(), [])
            self.assertIsNotNone(tail.error)
            path.write_text('Timestamp,CBR_Percent\n2026-11-03T12:00:00,20\n2026-11-03T12:00:01,')
            self.assertEqual(len(tail.poll()), 1)
            self.assertIsNone(tail.error)
            self.assertEqual(tail.latest['CBR_Percent'], '20')
            self.assertEqual(tail.poll(), [])
            with path.open('a') as stream:
                stream.write('40\n')
            self.assertEqual(tail.poll()[0]['CBR_Percent'], '40')
            self.assertEqual(tail.poll(), [])

    def test_rotation_and_truncation_reset_schema(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'log.csv'
            path.write_text('value,other\n1234567890,x\n')
            tail = CsvTail(path, ['value'])
            tail.poll()
            path.write_text('value\n2\n')
            self.assertEqual(tail.poll(), [{'value': '2'}])
            replacement = path.with_suffix('.new')
            replacement.write_text('value\n3\n')
            replacement.replace(path)
            self.assertEqual(tail.poll(), [{'value': '3'}])

    def test_schema_errors_can_recover(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'log.csv'
            path.write_text('wrong\n10\n')
            tail = CsvTail(path, ['value'])
            self.assertEqual(tail.poll(), [])
            self.assertIsNotNone(tail.error)
            path.write_text('value\n20\n')
            self.assertEqual(tail.poll(), [{'value': '20'}])
            self.assertIsNone(tail.error)

    def test_large_log_starts_near_end(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'log.csv'
            path.write_text('value\n' + '1234567890\n' * 100000 + '42\n')
            tail = CsvTail(path, ['value'])
            rows = tail.poll()
            self.assertLess(len(rows), 25000)
            self.assertEqual(tail.latest['value'], '42')

    def test_optional_net_telemetry_and_invalid_input(self):
        tail = CsvTail('unused', [])
        self.assertIn('--dcc-net', net_lines(tail)[0])
        tail.latest = {'Timestamp': datetime.now().isoformat(), 'Valid': 'YES',
                       'Local_CBR_Percent': '10', 'One_Hop_CBR_Percent': '90',
                       'Two_Hop_CBR_Percent': '80', 'Global_CBR_Percent': '90', 'Neighbors': '2'}
        first, second = net_lines(tail)
        self.assertIn('locale 10%', first)
        self.assertIn('globale 90%', second)
        self.assertIn('vicini 2', second)
        tail.latest['Valid'] = 'NO'
        self.assertIn('ingresso non valido', net_lines(tail)[0])
        tail.latest['Timestamp'] = '2000-01-01T00:00:00'
        self.assertIn('nessun dato recente', net_lines(tail)[0])

    def test_bad_values_and_stale_timestamps(self):
        self.assertIsNone(number({'x': 'nan'}, 'x'))
        self.assertIsNone(number({'x': 'inf'}, 'x'))
        self.assertIsNone(number({}, 'x'))
        self.assertIsNone(age({'Timestamp': 'broken'}))
        self.assertEqual(age({'Timestamp_Unix_ms': '1000'}, cam=True, now=3), 2)
        tail = CsvTail('unused', [])
        tail.latest = {'Timestamp': '2000-01-01T00:00:00'}
        self.assertFalse(status(tail)[1])
        self.assertIn('DATI FERMI', status(tail)[0])
        tail.latest = {'Timestamp': '2999-01-01T00:00:00'}
        self.assertIn('FUTURO', status(tail)[0])


if __name__ == '__main__':
    unittest.main()
