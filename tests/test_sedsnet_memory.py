import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SedsnetMemoryTests(unittest.TestCase):
    def test_threadx_pool_leaves_profiled_router_headroom(self):
        config = (ROOT / "AZURE_RTOS" / "App" / "app_azure_rtos_config.h").read_text(
            encoding="utf-8"
        )
        pool = int(re.search(r"TX_APP_MEM_POOL_SIZE\s+(\d+)", config).group(1))
        thread_stacks = 16 * 1024 + 8 * 1024
        self.assertEqual(pool, 61440)
        self.assertGreaterEqual(pool - thread_stacks, 36 * 1024)

    def test_sedsnet_shared_budget_fits_embedded_discovery(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        budget = int(
            re.search(r'set\(SEDSNET_MAX_QUEUE_BUDGET "(\d+)"', cmake).group(1)
        )
        recent = int(
            re.search(r'set\(SEDSNET_MAX_RECENT_RX_IDS "(\d+)"', cmake).group(1)
        )
        start = int(
            re.search(r'set\(SEDSNET_ENV_STARTING_QUEUE_SIZE "(\d+)"', cmake).group(1)
        )
        self.assertEqual(budget, 12288)
        self.assertEqual(recent, 16)
        self.assertEqual(start, 128)
        self.assertGreater(budget, recent * 8 + 2 * start)


if __name__ == "__main__":
    unittest.main()
