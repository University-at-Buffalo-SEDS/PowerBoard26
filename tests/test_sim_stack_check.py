import shutil
import subprocess
import unittest
from pathlib import Path


class SimulatorStackCheckParity(unittest.TestCase):
    def test_threadx_stack_checking_remains_enabled_in_simulation(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "C preprocessor required for configuration validation")
        header = Path(__file__).resolve().parents[1] / "Core/Inc/tx_user.h"
        for simulator in (False, True):
            with self.subTest(simulator=simulator):
                command = [compiler, "-E", "-dM", "-x", "c", "-"]
                if simulator:
                    command.insert(1, "-DSEDS_FIRMWARE_SIM_TEST=1")
                result = subprocess.run(command, input=f'#include "{header}"\n', text=True, capture_output=True, check=True)
                self.assertIn("#define TX_ENABLE_STACK_CHECKING", result.stdout)


if __name__ == "__main__":
    unittest.main()
