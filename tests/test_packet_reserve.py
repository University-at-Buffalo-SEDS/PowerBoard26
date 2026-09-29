"""Exercise the real reserve and allocator code against a bounded pool model."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

class PacketReserveTests(unittest.TestCase):
    def test_exhaustion_reuse_and_allocator_fallback(self):
        hooks = (ROOT / 'Core/Src/telemetry_hooks.c').read_text()
        allocator = hooks[hooks.index('void *telemetryMalloc('):hooks.index('void seds_error_msg(')]
        stub = r'''
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
typedef unsigned UINT;
typedef unsigned long ULONG;
#define TX_SUCCESS 0
#define TX_NO_WAIT 0
#define TX_NO_MEMORY 16
#define TX_CALLER_ERROR 19
#define TX_NULL NULL
typedef struct { unsigned char *start; size_t stride; unsigned count, used[4]; } TX_BLOCK_POOL;
static UINT tx_block_pool_create(TX_BLOCK_POOL *p, char *name, size_t size, void *storage, size_t bytes) {
    (void)name; p->start=storage; p->stride=size+sizeof(void *); p->count=bytes/p->stride;
    assert(p->count<=4); for(unsigned i=0;i<p->count;i++) p->used[i]=0; return TX_SUCCESS;
}
static UINT tx_block_pool_delete(TX_BLOCK_POOL *p) { (void)p; return TX_SUCCESS; }
static UINT tx_block_allocate(TX_BLOCK_POOL *p, void **out, UINT wait) {
    assert(wait==TX_NO_WAIT);
    for(unsigned i=0;i<p->count;i++) if(!p->used[i]) {
        p->used[i]=1; unsigned char *base=p->start+i*p->stride;
        *(TX_BLOCK_POOL **)base=p; *out=base+sizeof(void *); return TX_SUCCESS;
    }
    return TX_NO_MEMORY;
}
static UINT tx_block_release(void *ptr) {
    unsigned char *base=(unsigned char *)ptr-sizeof(void *);
    TX_BLOCK_POOL *p=*(TX_BLOCK_POOL **)base;
    size_t index=(base-p->start)/p->stride; assert(index<p->count && p->used[index]);
    p->used[index]=0; return TX_SUCCESS;
}
'''
        code = r'''
#include "telemetry_reserve.h"
static int ordinary_pool;
static void *rust_byte_pool_external=&ordinary_pool;
static unsigned byte_result=TX_NO_MEMORY, byte_releases;
static uint32_t g_telemetry_max_alloc_request, g_telemetry_alloc_failure_status;
static uint32_t g_telemetry_alloc_failure_available, g_telemetry_alloc_failure_fragments;
static uint32_t g_telemetry_alloc_failure_request, g_telemetry_alloc_fail, g_telemetry_alloc_count, g_telemetry_free_count;
static ULONG g_telemetry_pool_available=10000, g_telemetry_pool_fragments=100;
static void telemetry_memory_profile_sample(void) {}
static UINT tx_byte_allocate(void *pool, void **ptr, size_t size, UINT wait) {
    (void)size; assert(pool==&ordinary_pool && wait==TX_NO_WAIT);
    *ptr=byte_result==TX_SUCCESS ? &ordinary_pool : NULL; return byte_result;
}
static UINT tx_byte_release(void *ptr) { assert(ptr==&ordinary_pool); byte_releases++; return TX_SUCCESS; }
''' + allocator + r'''
int main(void) {
    assert(telemetryMalloc(3652)==NULL); /* not initialized */
    telemetry_reserve_init(); telemetry_reserve_init();
    assert(g_telemetry_reserve_init_status==TX_SUCCESS);
    assert(telemetryMalloc(1023)==NULL && telemetryMalloc(8193)==NULL);
    void *blocks[5];
    for(unsigned i=0;i<4;i++) { blocks[i]=telemetryMalloc(3652); assert(blocks[i]); }
    blocks[4]=telemetryMalloc(8192); assert(blocks[4]);
    assert(telemetryMalloc(4096)==NULL);
    telemetryFree(blocks[1]); blocks[1]=telemetryMalloc(4096); assert(blocks[1]);
    for(unsigned i=0;i<5;i++) telemetryFree(blocks[i]);
    /* A smaller packet may borrow the 8K block when all 4K blocks are busy. */
    for(unsigned i=0;i<5;i++) { blocks[i]=telemetryMalloc(1024); assert(blocks[i]); }
    assert(telemetryMalloc(1024)==NULL);
    for(unsigned i=0;i<5;i++) telemetryFree(blocks[i]);
    byte_result=TX_CALLER_ERROR;
    assert(telemetryMalloc(3652)==NULL); /* reserve must not hide invalid context */
    assert(g_telemetry_alloc_failure_status==TX_CALLER_ERROR);
    byte_result=TX_SUCCESS;
    void *normal=telemetryMalloc(3652); assert(normal==&ordinary_pool);
    telemetryFree(normal); assert(byte_releases==1);
    assert(g_telemetry_reserve_recoveries==11);
    assert(g_telemetry_alloc_count==g_telemetry_free_count);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp)
            (p/'tx_api.h').write_text(stub)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',tmp,'-I',str(ROOT/'Core/Inc'),'-x','c','-','-o',str(p/'test')],input=code,text=True,check=True)
            subprocess.run([str(p/'test')],check=True)
