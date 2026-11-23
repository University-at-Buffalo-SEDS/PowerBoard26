"""Power-monitor startup: a missing LTC2990 never blocks the other or the board.

The production sensor_try_init / sensor_service_monitors (sensor_thread.c) and
power_monitor_retry.h run on a virtual millisecond clock with the LTC2990 init
and sampling mocked per device. Host logic evidence only.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "power_monitor_retry.h"
typedef struct { int id; } LTC2990_Handle_t;
typedef enum { VOLTAGE, CURRENT } LTC2990_ROLE;
typedef struct { int unused; } I2C_HandleTypeDef;
#define LTC2990_I2C_ADDRESS_VOLTAGE (0x4C)
#define LTC2990_I2C_ADDRESS_CURRENT (0x4D)
static I2C_HandleTypeDef hi2c2;
static int present[2];
static unsigned inits[2], samples[2];
int LTC2990_Init(LTC2990_Handle_t *h, I2C_HandleTypeDef *bus, uint8_t addr, LTC2990_ROLE role) {
    assert(bus == &hi2c2 && h->id == (int)role && addr == (role == VOLTAGE ? 0x4C : 0x4D));
    inits[role]++; return present[role] ? 0 : 1;
}
void telemetry_ltc2990_update_voltage(LTC2990_Handle_t *h) { assert(h->id == 0 && present[0]); samples[0]++; }
void telemetry_ltc2990_update_current(LTC2990_Handle_t *h) { assert(h->id == 1 && present[1]); samples[1]++; }
FUNCTIONS
#define PERIOD_MS 1000U
int main(void) {
    LTC2990_Handle_t v = {0}, c = {1};
    uint32_t now = 1000;
    /* Voltage monitor absent at boot, current present: startup returns at
     * once (one attempt each) and the current monitor is up. */
    present[1] = 1;
    sensor_service_monitors(&v, &c, now, 0);
    assert(inits[0] == 1 && inits[1] == 1);
    assert(g_power_ltc2990_present == POWER_MONITOR_CURRENT);
    assert(g_power_ltc2990_retry_delay_ms == POWER_MONITOR_RETRY_MIN_MS);
    /* Ten minutes absent: current sampled every period, voltage retried on
     * backoff (1, 2, 4, 8, 16, then every 32 s), never sampled. */
    for (unsigned i = 0; i < 600; i++) { now += PERIOD_MS; sensor_service_monitors(&v, &c, now, 1); }
    printf("voltage inits=%u current samples=%u retries=%u delay=%u\n", inits[0], samples[1],
           g_power_ltc2990_retries, g_power_ltc2990_retry_delay_ms);
    assert(samples[1] == 600 && samples[0] == 0 && inits[1] == 1);
    assert(inits[0] >= 20 && inits[0] <= 25);
    assert(g_power_ltc2990_retries == inits[0] - 1);
    assert(g_power_ltc2990_retry_delay_ms == POWER_MONITOR_RETRY_MAX_MS);
    assert(g_power_ltc2990_present == POWER_MONITOR_CURRENT);
    /* The monitor appears: picked up within one maximum delay. */
    present[0] = 1; const uint32_t appeared = now; unsigned before = samples[0];
    while (samples[0] == before) { now += PERIOD_MS; sensor_service_monitors(&v, &c, now, 1); }
    assert(now - appeared <= POWER_MONITOR_RETRY_MAX_MS + PERIOD_MS);
    assert(g_power_ltc2990_present == (POWER_MONITOR_VOLTAGE | POWER_MONITOR_CURRENT));
    assert(g_power_ltc2990_retry_delay_ms == 0U);
    const unsigned settled = inits[0];
    for (unsigned i = 0; i < 100; i++) { now += PERIOD_MS; sensor_service_monitors(&v, &c, now, 1); }
    assert(inits[0] == settled && samples[0] == before + 101);
    /* The reverse: current absent never blocks voltage. Clock wraps too. */
    sensor_voltage_retry = (power_monitor_retry_t){0}; sensor_current_retry = (power_monitor_retry_t){0};
    present[0] = 1; present[1] = 0; samples[0] = samples[1] = 0; now = UINT32_MAX - 5000U;
    sensor_service_monitors(&v, &c, now, 0);
    for (unsigned i = 0; i < 120; i++) { now += PERIOD_MS; sensor_service_monitors(&v, &c, now, 1); }
    assert(samples[0] == 120 && samples[1] == 0 && g_power_ltc2990_present == POWER_MONITOR_VOLTAGE);
    return 0;
}
'''


def function(source, name):
    match = re.search(r'^static void ' + re.escape(name) + r'\([^)]*\)\s*\{', source, re.M)
    assert match, name
    start = match.start(); end, depth = source.index('{', match.start()) + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end]


class PowerMonitorStartup(unittest.TestCase):
    def test_missing_voltage_monitor_never_blocks_the_current_monitor(self):
        source = (ROOT / 'Core/Src/sensor_thread.c').read_text()
        state = re.search(r'/\* Power-monitor health.*?static power_monitor_retry_t [^;]*;', source, re.S).group()
        functions = state + '\n' + function(source, 'sensor_try_init') + '\n' + function(source, 'sensor_service_monitors')
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'startup'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'Core/Inc'), '-x', 'c', '-', '-o', str(binary)],
                           input=HARNESS.replace('FUNCTIONS', functions), text=True, check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_startup_has_no_unbounded_init_loop(self):
        source = (ROOT / 'Core/Src/sensor_thread.c').read_text()
        self.assertNotRegex(source, r'while\s*\(\s*LTC2990_Init')


if __name__ == '__main__':
    unittest.main()
