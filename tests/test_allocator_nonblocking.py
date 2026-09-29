import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class AllocatorTests(unittest.TestCase):
    def test_allocator_never_waits_and_records_exact_failure(self):
        src = (ROOT/'Core/Src/telemetry_hooks.c').read_text()
        allocator = src[src.index('void *telemetryMalloc('):src.index('void telemetryFree(')]
        code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned UINT;
#define TX_SUCCESS 0
#define TX_NO_WAIT 0
#define TX_NO_MEMORY 16
static unsigned g_telemetry_reserve_recoveries;
static void *telemetry_reserve_allocate(size_t n) { (void)n; return NULL; }
static int pool;
static void *rust_byte_pool_external=&pool;
static unsigned calls, result, requested;
static uint32_t g_telemetry_max_alloc_request, g_telemetry_alloc_failure_status;
static uint32_t g_telemetry_alloc_failure_available, g_telemetry_alloc_failure_fragments;
static uint32_t g_telemetry_alloc_failure_request, g_telemetry_alloc_fail, g_telemetry_alloc_count;
static unsigned g_telemetry_pool_available=4096, g_telemetry_pool_fragments=200;
static void telemetry_memory_profile_sample(void) {}
static UINT tx_byte_allocate(void *p, void **out, size_t size, UINT wait) {
    assert(p==&pool); assert(wait==TX_NO_WAIT); calls++; requested=size;
    *out=result ? NULL : &pool; return result;
}
''' + allocator + r'''
int main(void) {
    assert(telemetryMalloc(0)==&pool); assert(requested==1);
    assert(telemetryMalloc(3652)==&pool); assert(g_telemetry_max_alloc_request==3652);
    result=16;
    assert(telemetryMalloc(4000)==NULL);
    assert(g_telemetry_alloc_failure_status==16 && g_telemetry_alloc_failure_request==4000);
    assert(g_telemetry_alloc_failure_available==4096 && g_telemetry_alloc_failure_fragments==200);
    assert(g_telemetry_alloc_count==2 && g_telemetry_alloc_fail==1);
    result=4; assert(telemetryMalloc(64)==NULL); assert(g_telemetry_alloc_failure_status==4);
    rust_byte_pool_external=NULL; assert(telemetryMalloc(64)==NULL); assert(calls==4);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            out=str(pathlib.Path(tmp)/'allocator')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-x','c','-','-o',out],input=code,text=True,check=True)
            subprocess.run([out],check=True)
