import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class TelemetryRateTests(unittest.TestCase):
    def test_configured_periods_and_tick_rounding(self):
        for rate, period in [(None, 5000), (0.5, 2000), (4, 250), (1000, 1)]:
            with self.subTest(rate=rate), tempfile.TemporaryDirectory() as tmp:
                binary = pathlib.Path(tmp) / "rate"
                code = '#include "telemetry_rate.h"\n#include <assert.h>\nint main(void) {'
                code += f'assert(pb_telemetry_period_ms() == {period});'
                code += f'assert(pb_telemetry_period_ticks(100) == {(period * 100 + 999) // 1000});'
                code += 'assert(pb_telemetry_period_ticks(0) == 1);}'
                cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "Core/Inc")]
                if rate is not None:
                    cmd += [f"-DPB_TELEMETRY_RATE_HZ={rate}"]
                subprocess.run(cmd + ["-x", "c", "-", "-o", str(binary)], input=code, text=True, check=True)
                subprocess.run([str(binary)], check=True)

    def test_invalid_rate_rejected(self):
        for rate in (0, -1, 1001):
            with self.subTest(rate=rate), tempfile.TemporaryDirectory() as tmp:
                result = subprocess.run(["cc", "-std=c11", "-I", str(ROOT / "Core/Inc"),
                    f"-DPB_TELEMETRY_RATE_HZ={rate}", "-x", "c", "-", "-c", "-o", str(pathlib.Path(tmp) / "bad.o")],
                    input='#include "telemetry_rate.h"', text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("must be between", result.stderr)
