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
static unsigned mask, sensor, calls, fail, fail_type, inject;
static uint64_t now_ms, published_at[3];
uint64_t telemetry_now_ms(void){return now_ms;}
static void stage_sensor_report(uint32_t bit,float value);
static float published[3];
static uint32_t g_sim_voltage_publish_attempts,g_sim_voltage_publish_ok;
typedef struct {float value;} LTC2990_Handle_t;
uint32_t __get_PRIMASK(void){return mask;}
void __disable_irq(void){mask=1;}
void __set_PRIMASK(uint32_t m){mask=m;}
void LTC2990_Step(LTC2990_Handle_t*h){assert(sensor);(void)h;}
void LTC2990_Get_Voltage(LTC2990_Handle_t*h,float*out){assert(sensor);out[0]=h->value;}
int log_telemetry_asynchronous_at(unsigned type,const void*value,size_t count,size_t width,uint64_t sampled_ms){
 assert(!sensor&&!mask&&count==1&&width==sizeof(float));calls++;published[type]=*(const float*)value;published_at[type]=sampled_ms;
 if(inject){inject=0;++now_ms;stage_sensor_report(1U,88.0f);}
 return fail&&(!fail_type||fail_type==type);
}
int log_telemetry_asynchronous(unsigned type,const void*value,size_t count,size_t width){
 return log_telemetry_asynchronous_at(type,value,count,width,now_ms);
}
'''
        stub += next(line for line in (ROOT / "Core/Inc/ltc2990.h").read_text().splitlines() if line.startswith("#define CURRENT_DRAW_POLARITY")) + "\n"
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
 now_ms=5000;sensor=1;telemetry_ltc2990_update_voltage(&voltage);sensor=0;fail=1;
 telemetry_ltc2990_publish_pending();assert(calls==4&&g_sim_voltage_publish_attempts==2&&g_sim_voltage_publish_ok==1);
 now_ms=5099;telemetry_ltc2990_publish_pending();assert(calls==4); // bounded retry pacing
 now_ms=5100;fail=0;telemetry_ltc2990_publish_pending();assert(calls==5); // first pressure failure must not lose the5s sample
 assert(published_at[1]==5000&&g_sim_voltage_publish_ok==2); // acquisition time, not retry time
 telemetry_ltc2990_publish_pending();assert(calls==5);
 now_ms=10000;sensor=1;telemetry_ltc2990_update_voltage(&voltage);sensor=0;fail=1;
 telemetry_ltc2990_publish_pending();assert(calls==6);
 now_ms=11000;fail=0;telemetry_ltc2990_publish_pending();assert(calls==6); // expired measurement is never restamped as fresh
 now_ms=12000;sensor=1;voltage.value=7;telemetry_ltc2990_update_voltage(&voltage);sensor=0;
 telemetry_ltc2990_publish_pending();assert(calls==7&&published_at[1]==12000&&fabsf(published[1]-19.6f)<0.001f);
 now_ms=20000;sensor=1;telemetry_ltc2990_update_voltage(&voltage);sensor=0;fail=1;inject=1;
 telemetry_ltc2990_publish_pending();assert(calls==8);
 now_ms=20100;fail=0;telemetry_ltc2990_publish_pending();
 assert(calls==9&&published[1]==88.0f&&published_at[1]==20001); // failed old snapshot must not overwrite newer acquisition
 now_ms=30000;sensor=1;telemetry_ltc2990_update_voltage(&voltage);sensor=0;inject=1;
 telemetry_ltc2990_publish_pending();assert(calls==10);
 telemetry_ltc2990_publish_pending();assert(calls==11&&published[1]==88.0f&&published_at[1]==30001); // success must not clear a concurrently staged reading
 now_ms=40000;sensor=1;telemetry_ltc2990_update_voltage(&voltage);current.value=4;
 telemetry_ltc2990_update_current(&current);sensor=0;fail=1;fail_type=1;
 telemetry_ltc2990_publish_pending();assert(calls==13&&published[2]==4&&published_at[2]==40000);
 now_ms=40100;fail=0;telemetry_ltc2990_publish_pending();assert(calls==14&&published_at[2]==40000); // retry only failed channel
 now_ms=50000;sensor=1;telemetry_ltc2990_update_current(&current);sensor=0;fail=1;fail_type=2;
 for(;now_ms<=51000;now_ms++)telemetry_ltc2990_publish_pending();
 assert(calls==24); // at most ten attempts; no 1ms busy loop or stale publication
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'test.c').write_text(stub+source+main)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    def test_retry_timestamp_retains_acquisition_age_in_the_current_clock_domain(self):
        source=(ROOT/'Core/Src/telemetry.c').read_text()
        function=source[source.index('SedsResult log_telemetry_asynchronous_at('):
                        source.index('SedsResult log_telemetry_string_asynchronous(')]
        harness=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define TELEMETRY_ENABLED 1
typedef unsigned SedsDataType;
typedef int SedsResult;
#define SEDS_OK 0
#define SEDS_ERR 1
#define SEDS_BAD_ARG 2
static struct {void *r;uint64_t start_time;} g_router={(void*)1,100};
static uint64_t now_ms,network_ms,last_timestamp;
static unsigned calls;
uint64_t telemetry_now_ms(void){return now_ms;}
uint64_t telemetry_unix_ms(void){return network_ms;}
int init_telemetry_router(void){return SEDS_ERR;}
int guess_kind_from_elem_size(size_t size){assert(size==sizeof(float));return 2;}
int seds_router_log_typed_ex(void*router,unsigned type,const void*value,size_t count,size_t size,int kind,const uint64_t*time,int queue){
 assert(router==(void*)1&&type==1&&value&&count==1&&size==sizeof(float)&&kind==2&&time&&!queue);
 ++calls;last_timestamp=*time;return SEDS_OK;
}
''' + function + r'''
int main(void){
 float value=16;
 now_ms=1500;network_ms=100000;
 assert(log_telemetry_asynchronous_at(1,&value,1,sizeof(value),1000)==SEDS_OK);
 assert(calls==1&&last_timestamp==99500); // retry delay is not erased by UTC mapping
 network_ms=0;
 assert(log_telemetry_asynchronous_at(1,&value,1,sizeof(value),1000)==SEDS_OK);
 assert(calls==2&&last_timestamp==900); // monotonic time before network sync
 assert(log_telemetry_asynchronous_at(1,&value,1,sizeof(value),50)==SEDS_BAD_ARG&&calls==2);
 assert(log_telemetry_asynchronous_at(1,&value,1,sizeof(value),1600)==SEDS_BAD_ARG&&calls==2);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'test.c').write_text(harness)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
