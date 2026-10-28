"""Denser sampling must preserve the existing startup warm-up duration."""
import copy
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest import mock
from sim import run_full

class MemoryProfileSampling(unittest.TestCase):
    def test_profile_preserves_twelve_second_startup_warmup(self):
        layout = {"execution": {"virtual_time_ms":15000,"sample_count":5,
            "memory_probe_warmup_samples":4,
            "memory_probes":[{"name":"watchdog_feeds","minimum":2}]}}
        captured=[]
        with mock.patch.object(run_full,"require_docker",return_value="docker"), \
             mock.patch.object(run_full,"resolve_simulator_image",return_value="fixture"), \
             mock.patch.object(run_full,"load_layout_for_build",return_value=copy.deepcopy(layout)), \
             mock.patch.object(run_full,"write_container_layout",side_effect=lambda path,data:captured.append(copy.deepcopy(data))), \
             mock.patch.object(run_full,"run_live"):
            run_full.run_memory_profile(SimpleNamespace(say=lambda *a:None),Path("/fixture"),"stm32g4","Release")
        execution=captured[0]["execution"]
        self.assertEqual(execution["sample_count"],20)
        self.assertEqual(execution["memory_probe_warmup_samples"],16)
        self.assertEqual(execution["memory_probes"],[{"name":"watchdog_feeds","minimum":2}])
        self.assertEqual(execution["virtual_time_ms"],15000)
