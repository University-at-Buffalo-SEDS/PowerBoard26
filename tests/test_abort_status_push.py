"""An applied change to Aborted pushes one BOARD_STATE_STATUS to the station.

The station's ordered recovery queries can wait behind a radio query, so an
Abort is otherwise seen only at the next answered QUERY. The applied change
to FLIGHT_STATE 15 now pushes exactly one unsolicited status: nonce 0,
ABORT_LATCHED, the live abort epoch, the QUERY reply's generation and the
last valid QUERY's issuer. A repeated Aborted write pushes none, and a push the router refused is retried
from the poll while the board is still Aborted. An accepted push can still be
cancelled with the queued group behind a lost-ACK status, so while still
Aborted the board re-pushes twice, about 1 s and 2.5 s after the change.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_cache_reset_owner import HARNESS as RESET_HARNESS, HEADERS as RESET_HEADERS

ROOT = Path(__file__).resolve().parents[1]


def _function(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


CACHE_CASES = r'''
static unsigned pushes,refuse;static SedsRouter *pushed_router;
SedsResult board_recovery_push_abort_status(SedsRouter *r){++pushes;pushed_router=r;return refuse?SEDS_BAD_ARG:SEDS_OK;}
int main(void){
 SedsRouter router=0;
 assert(flight_state_cache_init(&router)==0&&flight_state_cache_value()==6);
 assert(update(15)==0&&flight_state_cache_value()==15&&pushes==1&&pushed_router==&router);
 assert(update(15)==0&&pushes==1);           /* a repeated Aborted write pushes none */
 assert(update(1)==0&&pushes==1);            /* other states push none */
 assert(update(15)==0&&pushes==2);           /* a new change to Aborted pushes again */
 /* A refused push is retried from the poll, once accepted never again. */
 SedsRouter poll_router=1;
 assert(update(1)==0);refuse=1;assert(update(15)==0&&pushes==3);
 assert(flight_state_cache_poll(&poll_router)==0&&pushes==4&&pushed_router==&poll_router);
 refuse=0;assert(flight_state_cache_poll(&poll_router)==0&&pushes==5);
 assert(flight_state_cache_poll(&poll_router)==0&&pushes==5);
 /* No retry once the state has left Aborted. */
 assert(update(1)==0);refuse=1;assert(update(15)==0&&pushes==6);refuse=0;
 assert(update(1)==0&&flight_state_cache_poll(&poll_router)==0&&pushes==6);
 return 0;
}
'''

#: Model of the SDK's Ordered BOARD_STATE_STATUS stream: one status in flight,
#: later ones queued behind it. A group cancel (the head's group ends without
#: delivery, or its deferral expires) removes every queued status of the type,
#: as SEDSnet 4.3.14 cancel_accepted_type does. A status queued after the
#: cancel starts a new group and is delivered. Bench and sim traces show the
#: Abort push lost exactly this way behind a query reply whose ACKs were lost.
REPUSH_CASES = r'''
enum{QUERY_REPLY=1,PUSH=2};
static int queue[16];static unsigned queued,delivered_pushes,pushes;static int head_in_flight;
static void enqueue(int kind){assert(queued<16);queue[queued++]=kind;}
SedsResult board_recovery_push_abort_status(SedsRouter *r){assert(r);++pushes;enqueue(PUSH);return SEDS_OK;}
static void group_cancel(void){queued=0;}
static void drain(void){            /* head retired: queued statuses go on air in order */
 head_in_flight=0;for(unsigned i=0;i<queued;++i)if(queue[i]==PUSH)++delivered_pushes;queued=0;}
static void run_polls(SedsRouter*r,unsigned from,unsigned to){
 for(tick=from;tick<=to;tick+=10){assert(flight_state_cache_poll(r)==0);
  if(!head_in_flight)drain();}}
int main(void){
 SedsRouter router=0;
 assert(flight_state_cache_init(&router)==0);
 tick=1000;assert(update(4)==0);
 /* A query reply is in flight, its ACKs lost; the Abort lands behind it. */
 head_in_flight=1;enqueue(QUERY_REPLY);
 assert(update(15)==0&&pushes==1&&queued==2);
 group_cancel();                    /* the head's group ends: the push is gone */
 enqueue(QUERY_REPLY);              /* a later reply survives (state 15) */
 drain();assert(delivered_pushes==0);
 /* Bounded re-push while Aborted: about +1 s, +2.5 s and +4 s after the edge. */
 run_polls(&router,1010,1990);assert(pushes==1);
 run_polls(&router,2000,2000);assert(pushes==2&&delivered_pushes==1);
 run_polls(&router,2010,3490);assert(pushes==2);
 run_polls(&router,3500,3500);assert(pushes==3&&delivered_pushes==2);
 run_polls(&router,3510,4990);assert(pushes==3);
 run_polls(&router,5000,5000);assert(pushes==4&&delivered_pushes==3);
 run_polls(&router,5010,20000);assert(pushes==4);  /* never more than three re-pushes */
 assert(update(15)==0&&pushes==4);  /* a repeated Aborted write re-arms nothing */
 run_polls(&router,20010,30000);assert(pushes==4);
 /* Leaving Aborted cancels the re-pushes that are still owed. */
 assert(update(1)==0);tick=40000;assert(update(15)==0&&pushes==5);
 tick=40500;assert(update(0)==0);
 run_polls(&router,40510,50000);assert(pushes==5);
 /* A new edge re-arms the schedule from its own time. */
 tick=60000;assert(update(15)==0&&pushes==6);
 run_polls(&router,60010,60990);assert(pushes==6);
 run_polls(&router,61000,61000);assert(pushes==7);
 run_polls(&router,61010,62500);assert(pushes==8);
 /* A reset to Startup cancels the owed re-push too. */
 assert(update(1)==0);tick=70000;assert(update(15)==0&&pushes==9);
 assert(flight_state_cache_reset_startup()==0);
 run_polls(&router,70010,80000);assert(pushes==9);
 return 0;
}
'''

LOST_CASES = r'''
/* The SDK's pending rows as the poll sees them: one status of this board
 * awaiting its receipt (st_pending) at retry count st_retries. */
static bool st_pending;static uint32_t st_retries;static unsigned pushes;
SedsResult board_recovery_push_abort_status(SedsRouter *r){assert(r);++pushes;return SEDS_OK;}
bool board_recovery_status_pending(SedsRouter *r,uint32_t *retries){(void)r;if(st_pending&&st_retries>*retries)*retries=st_retries;return st_pending;}
static void polls(SedsRouter*r,unsigned from,unsigned to){for(tick=from;tick<=to;tick+=10)assert(flight_state_cache_poll(r)==0);}
int main(void){
 SedsRouter router=0;
 assert(flight_state_cache_init(&router)==0);
 /* A: the edge push's 4 copies are all lost (T30), so is the +1 s copy; the
  * +2.5 s copy is acknowledged on its first try: no +4 s push, nothing after. */
 tick=1000;assert(update(4)==0);assert(update(15)==0&&pushes==1);
 st_pending=true;
 st_retries=0;polls(&router,1010,1390);st_retries=1;polls(&router,1400,1790);st_retries=2;polls(&router,1800,1990);
 polls(&router,2000,2000);assert(pushes==2);   /* +1 s */
 st_retries=3;polls(&router,2010,3490);        /* every try of both copies lost */
 polls(&router,3500,3500);assert(pushes==3);   /* +2.5 s */
 st_retries=0;polls(&router,3510,3590);st_pending=false;polls(&router,3600,12000);
 assert(pushes==3);
 /* B: never acknowledged: pushes at the edge, +1, +2.5 and +4 s (its tries
  * run past the station's 4.8 s Abort window), then none. */
 assert(update(1)==0);tick=20000;assert(update(15)==0&&pushes==4);
 st_pending=true;st_retries=3;
 polls(&router,20010,23990);assert(pushes==6);
 polls(&router,24000,24000);assert(pushes==7);   /* +4 s: its tries run to 25.2 s */
 polls(&router,24010,40000);assert(pushes==7);
 return 0;
}
'''

PUSH_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "board_recovery.h"
#include "board_recovery_wire.h"
typedef int SedsResult; typedef int SedsRouter; typedef int SedsDataType; typedef unsigned long ULONG;
#define SEDS_OK 0
#define SEDS_BAD_ARG -10
#define SEDS_DT_BOARD_STATE_STATUS 77
#define SEDS_EK_UNSIGNED 1
#define TX_TIMER_TICKS_PER_SECOND 1000U
static ULONG tx_time_get(void){return 654321U;}
static uint8_t sent[64]; static size_t sent_len; static unsigned sends;
static SedsResult seds_router_log_queue_typed(SedsRouter*r,SedsDataType t,const void*d,size_t n,size_t e,int k){
 assert(r&&t==SEDS_DT_BOARD_STATE_STATUS&&e==1&&k==SEDS_EK_UNSIGNED);
 memcpy(sent,d,n);sent_len=n;++sends;return SEDS_OK;}
int32_t board_recovery_snapshot(BoardRecoverySnapshot*o){
 *o=(BoardRecoverySnapshot){.actual_state=15U,.flags=BOARD_RECOVERY_SUPPORTED|BOARD_RECOVERY_STATE_VALID,.abort_epoch=3U};return 0;}
static uint64_t board_commissioning_query_generation(void){return 0x0102030405060708ULL;}
static bool board_commissioning_uncertain_first(void){return false;}
static bool board_commissioning_pending(void){return false;}
static uint64_t last_query_issuer=0xB1B2B3B4B5B6B7B8ULL;
@WIRE@
@STATUS@
@PUSH@
static uint64_t le64(const uint8_t*p){uint64_t v=0;for(int i=7;i>=0;--i)v=v<<8|p[i];return v;}
static uint32_t le32(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main(void){
 SedsRouter router=1;
 assert(board_recovery_push_abort_status(NULL)==SEDS_BAD_ARG&&sends==0);
 assert(board_recovery_push_abort_status(&router)==SEDS_OK&&sends==1&&sent_len==40U);
 assert(sent[0]==BOARD_RECOVERY_VERSION&&sent[1]==BOARD_RECOVERY_RESULT_OK&&sent[2]==15U);
 assert(sent[3]&BOARD_RECOVERY_ABORT_LATCHED);
 assert(sent[3]&BOARD_RECOVERY_STATE_VALID);
 assert(le64(sent+4)==0U);                              /* nonce 0: unsolicited */
 assert(le64(sent+12)==0x0102030405060708ULL);          /* the QUERY reply's generation */
 assert(le32(sent+20)==3U&&le32(sent+24)==654321U&&le32(sent+28)==0U);
 assert(le64(sent+32)==0xB1B2B3B4B5B6B7B8ULL);          /* last valid QUERY issuer */
 return 0;
}
'''


def _build_and_run(code: str, headers=None, sources=()) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp)
        for name, text in (headers or {}).items():
            (p / name).write_text(text)
        (p / 'harness.c').write_text(code)
        exe = p / 'test'
        subprocess.run(['cc', '-DTELEMETRY_ENABLED=1', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-I', str(p), '-I', str(ROOT / 'Core/Inc'),
                        *[str(ROOT / s) for s in sources], str(p / 'harness.c'), '-o', str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


class AbortStatusPushTests(unittest.TestCase):
    def test_only_an_applied_change_to_aborted_pushes_once(self):
        prelude = RESET_HARNESS[:RESET_HARNESS.index('int main(int argc,char **argv){')]
        _build_and_run(prelude + CACHE_CASES, RESET_HEADERS,
                       ('Core/Src/flight_state_cache.c', 'Core/Src/board_recovery_query.c'))

    def test_an_unacknowledged_abort_push_covers_the_station_abort_window(self):
        # T30: all 4 copies of the push lost at 30% loss. Re-pushes until a
        # status is acknowledged, the last one's tries running past 4.8 s.
        prelude = RESET_HARNESS[:RESET_HARNESS.index('int main(int argc,char **argv){')]
        _build_and_run(prelude + LOST_CASES, RESET_HEADERS,
                       ('Core/Src/flight_state_cache.c', 'Core/Src/board_recovery_query.c'))

    def test_push_lost_to_a_group_cancel_is_re_pushed_twice(self):
        prelude = RESET_HARNESS[:RESET_HARNESS.index('int main(int argc,char **argv){')]
        _build_and_run(prelude + REPUSH_CASES, RESET_HEADERS,
                       ('Core/Src/flight_state_cache.c', 'Core/Src/board_recovery_query.c'))

    def test_push_is_a_nonce_zero_abort_latched_status(self):
        transport = (ROOT / 'Core/Src/board_recovery_transport.c').read_text()
        wire = (ROOT / 'Core/Src/board_recovery_wire.c').read_text()
        wire = '\n'.join(l for l in wire.splitlines() if not l.startswith('#include'))
        code = (PUSH_HARNESS.replace('@WIRE@', wire)
                .replace('@STATUS@', _function(transport, 'static SedsResult commissioning_status('))
                .replace('@PUSH@', _function(transport, 'SedsResult board_recovery_push_abort_status(')))
        _build_and_run(code)

    def test_issuer_is_cached_from_each_valid_query(self):
        transport = (ROOT / 'Core/Src/board_recovery_transport.c').read_text()
        handler = _function(transport, 'SedsResult board_recovery_handle(')
        query = handler.index('if(request.operation==BOARD_RECOVERY_QUERY){')
        self.assertLess(handler.index('authorized_primary(router,packet)'), query)
        self.assertLess(handler.index('board_recovery_decode_request('), query)
        self.assertIn('last_query_issuer=request.issuer_session;', handler[query:query + 100])


if __name__ == '__main__':
    unittest.main()
