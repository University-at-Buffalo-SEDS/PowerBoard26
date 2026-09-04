import json
import unittest
from pathlib import Path

import build


class QualificationContractTests(unittest.TestCase):
    def test_telemetry_stack_covers_profiled_sedsnet_call_depth(self):
        root = Path(build.__file__).resolve().parent
        source = (root / "Core" / "Src" / "telemetry_thread.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("TELEMETRY_THREAD_STACK_SIZE (11U * 1024U)", source)

    def test_can_transport_starts_before_router(self):
        root = Path(build.__file__).resolve().parent
        source = (root / "Core" / "Src" / "telemetry_thread.c").read_text(
            encoding="utf-8"
        )
        self.assertLess(
            source.index("can_bus_init(&hfdcan2);"),
            source.index("init_telemetry_router();"),
        )

    def test_sensor_thread_cannot_be_starved_by_telemetry(self):
        root = Path(build.__file__).resolve().parent
        source = (root / "Core" / "Src" / "sensor_thread.c").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            source,
            r"SENSOR_THREAD_STACK_SIZE,\s*// must match allocation size\s*2,\s*// priority\s*2,",
        )

    def test_telemetry_loop_services_received_and_transmit_work_together(self):
        root = Path(build.__file__).resolve().parent
        thread = (root / "Core" / "Src" / "telemetry_thread.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("process_all_queues_timeout(TELEMETRY_QUEUE_SERVICE_BUDGET_MS)", thread)
        self.assertNotIn("dispatch_tx_queue_timeout(50)", thread)

    def test_full_runner_profiles_memory_and_linked_network(self):
        root = Path(build.__file__).resolve().parent
        runner = (root / "sim" / "run_full.py").read_text(encoding="utf-8")
        script = (root / "build.py").read_text(encoding="utf-8")

        self.assertIn('"profile"', runner)
        self.assertIn('"--sample-count", "20"', runner)
        self.assertIn('"--traffic-iterations", "1000000"', runner)
        self.assertIn('"bay"', runner)
        self.assertIn('"tx_probe": "fdcan_tx_ok"', runner)
        self.assertIn('"rx_probe": "fdcan_rx"', runner)
        self.assertIn('"host_nodes"', runner)
        self.assertIn('"groundstation"', runner)
        self.assertIn('"rocket_radio"', runner)
        self.assertIn('"fill_pico"', runner)
        self.assertIn('"GS_SIM_VALIDATE_VALVE_ROUNDTRIP": "1"', runner)
        self.assertIn('"probe": "valve_commands_received", "minimum": 1', runner)
        self.assertIn("forwarded status ACK to GroundStation", runner)
        self.assertIn('simulation_env["SEDS_FIRMWARE_SIM_TEST"] = "1"', runner)
        self.assertIn('run_live(command, "firmware simulation")', runner)
        self.assertIn('running ({int(now - started)}s elapsed)', runner)
        self.assertIn("Long-duration memory profile", script)
        self.assertIn("Network discovery and time sync", script)

    def test_layout_exposes_network_convergence(self):
        root = Path(build.__file__).resolve().parent
        layout = json.loads((root / "sim" / "board.json").read_text(encoding="utf-8"))
        self.assertLess(layout["execution"].get("memory_probe_warmup_samples", 0), layout["execution"]["sample_count"])
        probes = {
            probe["name"]: probe["symbol"]
            for probe in layout["execution"]["memory_probes"]
        }
        self.assertEqual(probes["network_ready"], "g_telemetry_network_ready")
        self.assertEqual(probes["discovery_seen"], "g_telemetry_discovery_seen")
        self.assertEqual(probes["timesync_valid"], "g_telemetry_timesync_valid")
        self.assertEqual(probes["fdcan_rx"], "g_fdcan_rx_count")

        telemetry = (root / "Core" / "Src" / "telemetry.c").read_text(encoding="utf-8")
        for symbol in (
            "g_telemetry_network_ready",
            "g_telemetry_discovery_seen",
            "g_telemetry_timesync_valid",
        ):
            self.assertIn(symbol, telemetry)

        can_bus = (root / "Core" / "Src" / "can_bus.c").read_text(encoding="utf-8")
        self.assertIn("g_fdcan_rx_count++", can_bus)

    def test_shared_can_avoids_hop_retry_storms(self):
        root = Path(build.__file__).resolve().parent
        telemetry = (root / "Core" / "Src" / "telemetry.c").read_text(encoding="utf-8")
        self.assertIn('seds_router_add_side_packed(r, "can", 3U, tx_send, NULL, false)', telemetry)

    def test_underglow_uses_only_native_network_variable_apis(self):
        root = Path(build.__file__).resolve().parent
        source = (root / "Core" / "Src" / "av_bay_underglow.c").read_text(encoding="utf-8")
        self.assertIn("seds_router_enable_network_variable", source)
        self.assertIn("seds_router_get_network_variable_packed_len", source)
        self.assertNotIn("seds_router_request_managed_variable", source)


    def test_periodic_health_check_does_not_serialize_topology(self):
        root = Path(build.__file__).resolve().parent
        telemetry = (root / "Core" / "Src" / "telemetry.c").read_text(encoding="utf-8")
        self.assertNotIn("seds_router_export_topology_len", telemetry)
        self.assertIn("g_telemetry_discovery_seen = 1U", telemetry)

if __name__ == "__main__":
    unittest.main()
