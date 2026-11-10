import argparse
import os
import tempfile
import unittest

from tools import cbr_generator


def args(**overrides):
    values = dict(
        low_offset=0.25,
        step_offset=0.25,
        ramp_duration=10.0,
        osc_period=4.0,
        stable_duration=2.0,
        osc_duration=8.0,
        step_duration=2.0,
    )
    values.update(overrides)
    return argparse.Namespace(**values)


class BuildPhasesTests(unittest.TestCase):
    def test_phase_values_and_boundaries(self):
        phases = cbr_generator.build_phases(0.4, 0.03, args())
        self.assertEqual([p[0] for p in phases], [
            "stable_low", "ramp_to_threshold", "critical_oscillation", "step_beyond_threshold"
        ])
        self.assertAlmostEqual(phases[0][2](0), 0.15)
        self.assertAlmostEqual(phases[1][2](5), 0.275)
        self.assertAlmostEqual(phases[1][2](20), 0.4)
        self.assertAlmostEqual(phases[2][2](1), 0.43)
        self.assertAlmostEqual(phases[3][2](0), 0.65)

    def test_low_and_high_values_are_clamped(self):
        phases = cbr_generator.build_phases(0.1, 0.2, args(low_offset=0.5, step_offset=2.0))
        self.assertEqual(phases[0][2](0), 0.0)
        self.assertEqual(phases[3][2](0), 1.0)

    def test_zero_duration_ramp_reaches_threshold(self):
        phases = cbr_generator.build_phases(0.4, 0.03, args(ramp_duration=0.0))
        self.assertEqual(phases[1][2](0), 0.4)


class AtomicWriteTests(unittest.TestCase):
    def test_replaces_destination_and_leaves_no_temp_file(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = os.path.join(directory, "cbr")
            cbr_generator.atomic_write(destination, "0.2500\n")
            cbr_generator.atomic_write(destination, "0.7500\n")
            with open(destination, encoding="utf-8") as stream:
                self.assertEqual(stream.read(), "0.7500\n")
            self.assertEqual(os.listdir(directory), ["cbr"])


if __name__ == "__main__":
    unittest.main()
