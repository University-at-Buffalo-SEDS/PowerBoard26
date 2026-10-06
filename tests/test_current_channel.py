import re
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class CurrentChannelTests(unittest.TestCase):
    def test_shunt_is_not_overwritten_by_remote_temperature_pair(self):
        source=(ROOT/'Core/Src/ltc2990.c').read_text()
        step=re.search(r"void LTC2990_Step\(LTC2990_Handle_t \*h\)\s*\{.*?\n\}",source,re.S).group()
        stub=r"""
#include <stdint.h>
#include <math.h>
#include <assert.h>
#define VOLTAGE 0
#define V1_MSB_REG 6
#define V2_MSB_REG 8
#define V3_MSB_REG 10
#define V4_MSB_REG 12
static int failed;
typedef struct {int role;float last_voltages[4];} LTC2990_Handle_t;
void LTC2990_Trigger_Conversion(LTC2990_Handle_t*h){(void)h;}
void sleep_ms(int n){(void)n;}
uint8_t LTC2990_ADC_Read_New_Data(LTC2990_Handle_t*h,uint8_t reg,uint16_t*out,int8_t*valid){
 (void)h;*valid=!failed;*out=reg==V1_MSB_REG?42:999;return failed;
}
float LTC2990_Code15_To_CurrentA(uint16_t raw){return raw;}
float LTC2990_Code_To_Single_Ended_Voltage(LTC2990_Handle_t*h,uint16_t raw){(void)h;return raw;}
"""
        main=r"""
int main(void){LTC2990_Handle_t h={.role=1};LTC2990_Step(&h);
 assert(h.last_voltages[0]==42);assert(h.last_voltages[1]==999);
 failed=1;LTC2990_Step(&h);assert(isnan(h.last_voltages[0]));}
"""
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'test.c').write_text(stub+step+main)
            subprocess.run(['cc','-std=c11','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
