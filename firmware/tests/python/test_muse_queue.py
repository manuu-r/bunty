"""Exercise the adapted production request-open function with a full TX queue."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MuseQueueTests(unittest.TestCase):
    def test_reply_subscription_waits_for_queued_audio(self):
        source = (ROOT / '.deps/muse/esp32/main/noise_control.cpp').read_text()
        begin = source.index('extern "C" int64_t noise_ctrl_req_open(')
        end = source.index('extern "C" bool noise_ctrl_req_send(', begin)
        code = r'''
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cassert>
#define CONFIG_BUNTY_MUSE 1
#define pdMS_TO_TICKS(ms) (ms)
constexpr unsigned REQ_MAX_HEADERS=16;
using noise_ctrl_req_cb=void (*)(void *,int,const uint8_t *,size_t,bool);
enum class req_op_kind {Open};
struct req_op {req_op_kind kind;bool end;int64_t id;uint8_t *data;size_t len;noise_ctrl_req_cb cb;void *ctx;};
static bool s_req_q=true,s_connected=true;
static std::atomic<int64_t> s_req_next_id{100};
static unsigned releaseAfter=300,waitUsed=0,allocations=0;
static void *allocate(size_t n){++allocations;return std::malloc(n);}
static void release(void *p){if(p){assert(allocations);--allocations;std::free(p);}}
static bool req_put(const req_op &op,unsigned wait){
    waitUsed=wait;
    if(wait<releaseAfter)return false;
    assert(op.id>=100 && !strcmp(reinterpret_cast<char *>(op.data),"POST"));
    release(op.data);return true;
}
#define malloc allocate
#define free release
''' + source[begin:end] + r'''
#undef malloc
#undef free
static void callback(void *,int,const uint8_t *,size_t,bool){}
int main(){
    const char *headers[]={"Accept","application/x-ndjson",nullptr};
    assert(noise_ctrl_req_open("POST","/chat/subscribe",headers,false,callback,nullptr)>=100);
    assert(waitUsed==1000 && allocations==0); // 300 ms congestion would reject a zero-wait open
    releaseAfter=1100;
    assert(!noise_ctrl_req_open("POST","/chat/subscribe",headers,false,callback,nullptr));
    assert(waitUsed==1000 && allocations==0); // bounded failure frees request storage
    s_connected=false;
    assert(!noise_ctrl_req_open("POST","/chat/subscribe",headers,false,callback,nullptr));
    assert(allocations==0);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            c = Path(folder) / 'queue.cpp'
            binary = Path(folder) / 'queue'
            c.write_text(code)
            build = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=undefined,bounds', '-fno-sanitize-recover=all',
                                    str(c), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
