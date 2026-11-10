"""Argument validation before any hardware is opened (full Linux build only)."""
from pathlib import Path
import subprocess
import unittest


@unittest.skipUnless((Path(__file__).resolve().parents[1] / 'dcc').is_file(),
                     'requires the Linux application build')
class CliTests(unittest.TestCase):
    def run_invalid(self, *options):
        binary = Path(__file__).resolve().parents[1] / 'dcc'
        result = subprocess.run([str(binary), 'unused0', 'unused1', *options],
                                capture_output=True, text=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('[MAIN] Avvio del thread DCC Worker', result.stdout)
        return result.stdout + result.stderr

    def test_net_eirp_requires_opt_in(self):
        self.assertIn('richiede --dcc-net', self.run_invalid('--dcc-net-eirp-offset-db', '1'))

    def test_invalid_net_eirp(self):
        for value in ('nan', 'inf', '32', '-32', '1x'):
            with self.subTest(value=value):
                self.assertIn('Offset EIRP non valido', self.run_invalid('--dcc-net', '--dcc-net-eirp-offset-db', value))
        self.assertIn('Valore mancante', self.run_invalid('--dcc-net', '--dcc-net-eirp-offset-db'))

    def test_existing_option_constraints_with_net(self):
        self.assertIn('solo in modalità reattiva', self.run_invalid('--dcc-net', '--limeric', '--fake-cbr'))
        self.assertIn('richiede --cam-static', self.run_invalid('--dcc-net', '--cam-force-100ms'))


if __name__ == '__main__':
    unittest.main()
