"""GS heartbeats reach the PB's station clock in every build.

The PB hosted the HEART_BEAT endpoint only in simulator builds, so its
station clock followed the GS only on recovery queries and could stay hours
off after a GS clock step. The endpoint is now hosted in every build and its
handler feeds GS heartbeats to the station clock.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from router_lifecycle_fixture import function

ROOT = Path(__file__).resolve().parents[1]


class HeartbeatStationClock(unittest.TestCase):
    def test_heart_beat_endpoint_is_hosted_outside_simulator_builds(self):
        source = (ROOT / "Core/Src/telemetry.c").read_text()
        init = function(source, "init_telemetry_router_locked")
        locals_table = re.search(r"const SedsLocalEndpointDesc locals\[\] = \{(.*?)\n  \};", init, re.S).group(1)
        entry = re.search(r"\{\.endpoint = SEDS_EP_HEART_BEAT,.*?\}", locals_table, re.S)
        self.assertIsNotNone(entry)
        self.assertIn("Heartbeat_handler", entry.group(0))
        before = locals_table[:entry.start()]
        self.assertEqual(before.count("#if"), before.count("#endif"))

    def test_handler_feeds_heartbeats_to_the_station_clock(self):
        source = (ROOT / "Core/Src/telemetry.c").read_text()
        harness = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef int SedsResult;typedef int SedsRouter;
enum {SEDS_OK=0};
#define SEDS_DT_HEARTBEAT 106U
typedef struct {uint32_t ty;const char*sender;size_t sender_len;uint64_t timestamp;} SedsPacketView;
static SedsRouter cookie;static struct {SedsRouter*r;} g_router={&cookie};
static unsigned observed,probed;
static SedsResult sim_probe_heartbeat_handler(const SedsPacketView*p,void*u){(void)p;(void)u;probed++;return SEDS_OK;}
static void board_recovery_observe_station_time(SedsRouter*r,const SedsPacketView*p){assert(r==&cookie&&p->ty==SEDS_DT_HEARTBEAT);observed++;}
""" + function(source, "Heartbeat_handler") + r"""
int main(void){
 SedsPacketView hb={SEDS_DT_HEARTBEAT,"GS",2,1000},other={7U,"GS",2,1000};
 assert(Heartbeat_handler(&hb,0)==SEDS_OK&&observed==1&&probed==1);
 assert(Heartbeat_handler(&other,0)==SEDS_OK&&observed==1&&probed==2);
 assert(Heartbeat_handler(0,0)==SEDS_OK&&observed==1);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "hb"
            build = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", "-x", "c", "-", "-o", str(exe)],
                                   input=harness, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
