import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class SensorDeadlineTests(unittest.TestCase):
    def test_slow_ready_stalled_bus_wrap_and_valid_sample(self):
        source = (ROOT / 'Core/Src/ltc2990.c').read_text()
        def function(name):
            return re.search(r'(?:static inline uint8_t|uint8_t|int8_t) ' + name + r'\(.*?\n\}', source, re.S).group()
        header = (ROOT / 'Core/Inc/ltc2990.h').read_text()
        constants = '\n'.join(re.findall(r'^#define LTC2990_(?:I2C|DATA_READY)_TIMEOUT_MS .+$', header, re.M))
        harness = r'''
#include <stdint.h>
#include <assert.h>
#define STATUS_REG 0
#define V1_MSB_REG 6
#define V2_MSB_REG 8
#define V3_MSB_REG 10
#define V4_MSB_REG 12
#define I2C_MEMADD_SIZE_8BIT 1
#define HAL_OK 0
#define TX_TIMER_TICKS_PER_SECOND 100
// Deliberately coarser scheduling than production: each 1 ms sleep costs 10 ms.
typedef int HAL_StatusTypeDef;
typedef struct {void *hi2c; unsigned i2c_address;} LTC2990_Handle_t;
static uint32_t now, delay, sleep_quantum=10;
static unsigned ready, failure, locks, reads, status_mask;
uint32_t HAL_GetTick(void){return now;}
void sleep_ms(unsigned ms){(void)ms;now+=sleep_quantum;}
void i2c_lock(LTC2990_Handle_t*h){(void)h;assert(!locks);locks++;}
void i2c_unlock(LTC2990_Handle_t*h){(void)h;assert(locks==1);locks--;}
int HAL_I2C_Mem_Read(void*h,unsigned addr,unsigned reg,unsigned sz,uint8_t*d,unsigned n,unsigned timeout){
 (void)h;(void)addr;(void)sz;assert(n==1);assert(timeout==50);reads++;
 now+=delay>timeout?timeout:delay;
 if(failure)return 1;
 *d=reg==0?(ready?status_mask:0):(reg%2==0?0x80:0x42);return HAL_OK;
}
int HAL_I2C_Mem_Write(void*h,unsigned addr,unsigned reg,unsigned sz,uint8_t*d,unsigned n,unsigned timeout){
 (void)h;(void)addr;(void)reg;(void)sz;(void)d;(void)n;assert(timeout==50);now+=timeout;return 1;
}
'''
        code = harness + constants + '\n' + function('status_bit_from_msb') + '\n' + function('LTC2990_Read_Register') + '\n' + function('LTC2990_Write_Register') + '\n' + function('LTC2990_ADC_Read_New_Data') + r'''
int main(void){
 LTC2990_Handle_t h={0}; uint16_t value=99; int8_t valid=0;
 // Not-ready status with slow, successful reads must stop by elapsed time.
 now=0;delay=40;ready=0;
 assert(LTC2990_ADC_Read_New_Data(&h,V1_MSB_REG,&value,&valid)==1);
 assert(now>=250&&now<=300);assert(reads<10);assert(!locks);
 // Six channels cannot consume a watchdog interval, even with coarse ticks.
 uint32_t start=now;for(unsigned i=0;i<6;i++)assert(LTC2990_ADC_Read_New_Data(&h,V1_MSB_REG,&value,&valid)==1);
 assert(now-start<=1800);
 now=UINT32_MAX-100;start=now;
 assert(LTC2990_ADC_Read_New_Data(&h,V1_MSB_REG,&value,&valid)==1);assert((uint32_t)(now-start)<=300);
 failure=1;delay=1000;now=0;
 assert(LTC2990_ADC_Read_New_Data(&h,V1_MSB_REG,&value,&valid)==1);assert(now==50&&!locks);
 assert(LTC2990_Write_Register(&h,1,0)==1);assert(now==100&&!locks);
 failure=0;delay=1;ready=1;status_mask=1U<<2;
 assert(LTC2990_ADC_Read_New_Data(&h,V1_MSB_REG,&value,&valid)==0);assert(value==0x42&&valid==1&&!locks);
 for(unsigned i=0;i<4;i++){
  status_mask=1U<<(i+2);assert(LTC2990_ADC_Read_New_Data(&h,6+2*i,&value,&valid)==0);
 }
 // BUSY and TINT ready do not prove that V1 has a new conversion.
 status_mask=3;start=now;assert(LTC2990_ADC_Read_New_Data(&h,6,&value,&valid)==1);
 assert((uint32_t)(now-start)>=250);
 reads=0;assert(LTC2990_ADC_Read_New_Data(&h,7,&value,&valid)==1);assert(!reads);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            (path/'test.c').write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
