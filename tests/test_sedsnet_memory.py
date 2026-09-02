import re
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SedsnetMemoryTests(unittest.TestCase):
    def test_threadx_pool_leaves_profiled_router_headroom(self):
        config = (ROOT / "AZURE_RTOS" / "App" / "app_azure_rtos_config.h").read_text(
            encoding="utf-8"
        )
        pool = int(re.search(r"TX_APP_MEM_POOL_SIZE\s+(\d+)", config).group(1))
        usb_pool = int(
            re.search(r"UX_DEVICE_APP_MEM_POOL_SIZE\s+(\d+)", config).group(1)
        )
        thread_stacks = 12 * 1024 + 10 * 1024
        self.assertEqual(pool, 65536)
        self.assertEqual(usb_pool, 26624)
        self.assertEqual(pool + usb_pool, 65536 + 26624)
        self.assertGreaterEqual(pool - thread_stacks, 40 * 1024)

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
        self.assertEqual(budget, 8192)
        self.assertEqual(recent, 16)
        self.assertEqual(start, 512)
        self.assertGreater(budget, recent * 8 + 2 * start)

    def test_sedsnet_shares_the_application_pool_without_nested_pool_metadata(self):
        app = (ROOT / "Core" / "Src" / "app_threadx.c").read_text(encoding="utf-8")
        self.assertIn("telemetry_set_byte_pool(byte_pool)", app)
        self.assertNotIn("static TX_BYTE_POOL sedsnet_byte_pool", app)
        self.assertNotIn("tx_byte_pool_create(&sedsnet_byte_pool", app)

    def test_reclaimed_stack_space_is_guarded_by_threadx(self):
        telemetry_thread = (ROOT / "Core" / "Src" / "telemetry_thread.c").read_text(
            encoding="utf-8"
        )
        threadx = (ROOT / "Core" / "Inc" / "tx_user.h").read_text(encoding="utf-8")
        app = (ROOT / "Core" / "Src" / "app_threadx.c").read_text(encoding="utf-8")
        self.assertIn("TELEMETRY_THREAD_STACK_SIZE (12U * 1024U)", telemetry_thread)
        sensor_thread = (ROOT / "Core" / "Src" / "sensor_thread.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("SENSOR_THREAD_STACK_SIZE (10U * 1024U)", sensor_thread)
        self.assertRegex(threadx, r"(?m)^#define TX_ENABLE_STACK_CHECKING$")
        self.assertIn("tx_thread_stack_error_notify(thread_stack_error_handler)", app)

    def test_normal_traffic_avoids_redundant_sedsnet_queues(self):
        telemetry = (ROOT / "Core" / "Src" / "telemetry.c").read_text(encoding="utf-8")
        self.assertIn("seds_router_receive_packed_from_side", telemetry)
        self.assertNotIn("seds_router_rx_packed_packet_to_queue", telemetry)
        async_body = telemetry.split("SedsResult log_telemetry_asynchronous", 1)[1]
        async_body = async_body.split("SedsResult dispatch_tx_queue", 1)[0]
        self.assertIn("seds_router_log_typed", async_body)
        self.assertNotIn("seds_router_log_queue_typed", async_body)
        thread = (ROOT / "Core" / "Src" / "telemetry_thread.c").read_text(encoding="utf-8")
        self.assertNotIn("process_all_queues_timeout", thread)

    def test_profile_allows_one_in_flight_packet_but_keeps_hard_reserve(self):
        layout = json.loads((ROOT / "sim" / "board.json").read_text())
        probes = {
            probe["name"]: probe
            for probe in layout["execution"]["memory_probes"]
        }
        self.assertEqual(probes["pool_available"]["minimum"], 4096)
        self.assertEqual(probes["pool_available"]["max_end_drop"], 4096)
        self.assertEqual(probes["alloc_failures"]["maximum"], 0)


if __name__ == "__main__":
    unittest.main()
