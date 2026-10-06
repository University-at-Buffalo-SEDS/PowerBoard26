import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class SensorPublicationTests(unittest.TestCase):
    def test_acquisition_never_enters_router_and_latest_values_publish_once(self):
        source=(ROOT/'Core/Src/ltc2990.c').read_text()
        source=source[source.index('/* Sensor acquisition runs'):]
        stub=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <math.h>
#define TELEMETRY_ENABLED 1
#define SEDS_DT_BATTERY_VOLTAGE 1
#define SEDS_DT_BATTERY_CURRENT 2
#define SEDS_OK 0
static unsigned mask, sensor, calls, fail;
static float published[3];
static uint32_t g_sim_voltage_publish_attempts,g_sim_voltage_publish_ok;
typedef struct {float value;} LTC2990_Handle_t;
uint32_t __get_PRIMASK(void){return mask;}
void __disable_irq(void){mask=1;}
void __set_PRIMASK(uint32_t m){mask=m;}
void LTC2990_Step(LTC2990_Handle_t*h){assert(sensor);(void)h;}
void LTC2990_Get_Voltage(LTC2990_Handle_t*h,float*out){assert(sensor);out[0]=h->value;}
int log_telemetry_asynchronous(unsigned type,const void*value,size_t count,size_t width){
 assert(!sensor&&!mask&&count==1&&width==sizeof(float));calls++;published[type]=*(const float*)value;return fail;
}
'''
        main=r'''
int main(void){
 LTC2990_Handle_t voltage={5},current={2};sensor=1;
 telemetry_ltc2990_update_voltage(&voltage);telemetry_ltc2990_update_current(&current);assert(!calls&&!mask);
 voltage.value=6;telemetry_ltc2990_update_voltage(&voltage);assert(!calls);
 sensor=0;telemetry_ltc2990_publish_pending();assert(calls==2);assert(fabsf(published[1]-16.8f)<0.001f&&published[2]==2);
 assert(g_sim_voltage_publish_attempts==1&&g_sim_voltage_publish_ok==1);
 telemetry_ltc2990_publish_pending();assert(calls==2);
 sensor=1;current.value=3;telemetry_ltc2990_update_current(&current);sensor=0;
 telemetry_ltc2990_publish_pending();assert(calls==3&&published[2]==3);
 sensor=1;telemetry_ltc2990_update_voltage(&voltage);sensor=0;fail=1;
 telemetry_ltc2990_publish_pending();assert(calls==4&&g_sim_voltage_publish_attempts==2&&g_sim_voltage_publish_ok==1);
 telemetry_ltc2990_publish_pending();assert(calls==4); // no busy retry loop on a disconnected bus
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'test.c').write_text(stub+source+main)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
