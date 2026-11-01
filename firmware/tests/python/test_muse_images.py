"""Exercise the adapted SDK downloader against its scripted HTTP transport.

Uses the pinned SDK's harness/fakes; compiles our actual cached image_fetch.c
with Bunty hooks enabled. No network, device, token, or cloud calls.
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / '.deps/muse/esp32'


class MuseImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for script in ('prepare_deps.py', 'adapt_muse.py'):
            run = subprocess.run([sys.executable, str(ROOT / 'scripts' / script)],
                                 capture_output=True, text=True)
            if run.returncode:
                raise RuntimeError(run.stdout + run.stderr)

    def test_download_commit_bounds_redirects_deadline_and_cancel(self):
        harness = (SDK / 'tests/link_image_fetch_harness.c').read_text()
        harness = harness.replace('// ---- Clock', 'static int g_jpeg_mode;\nstatic int g_gif_mode;\n\n// ---- Clock', 1)
        harness = harness.replace('    g_response_count = 0;', '    g_response_count = 0;\n    g_jpeg_mode = 0;\n    g_gif_mode = 0;')
        harness = harness.replace('buffer[got + i] = (char)(c->sent + i);',
                                  'buffer[got + i] = g_gif_mode==3 && c->sent+i<2 ? "GI"[c->sent+i] : (g_gif_mode==1 || g_gif_mode==2) && c->sent+i<6 ? "GIF89a"[c->sent+i] : g_jpeg_mode && c->sent+i<2 ? (char)(c->sent+i==0?0xff:0xd8) : (char)(c->sent + i);')
        hooks = r'''
static bool g_image_current;
const char *bunty_muse_gif_load(int (*read)(void *,uint8_t *,size_t),void *ctx,int row,int *width,int *height) {
    uint8_t data[32];int n=read(ctx,data,sizeof(data));
    CHECK(row==0 && n==32 && !memcmp(data,"GIF89a",6), "GIF sniff bytes were lost");
    for(int i=6;i<32;i++)CHECK(data[i]==i,"GIF fragmented read order wrong");
    *width=16;*height=16;
    return g_gif_mode==2 ? "invalid GIF" : NULL;
}
static int g_commits;
static bool g_cancel_after_draw;
bool bunty_muse_image_begin(void) { g_image_current=true; return true; }
bool bunty_muse_image_current(void) { return g_image_current; }
bool bunty_muse_image_end(bool ok) {
    bool commit=ok && g_image_current;
    if(commit) ++g_commits;
    g_image_current=false;
    return commit;
}
'''
        harness = harness.replace('static int g_rows_drawn;', 'static int g_rows_drawn;\n' + hooks)
        harness = harness.replace('    g_rows_drawn += h;', r'''
    // Assert every raw byte survived sniffing and fragmented socket reads.
    const uint8_t *bytes=(const uint8_t *)pixels;
    if(!g_jpeg_mode) {
        for(int i=0;i<w*h*2;++i) CHECK(bytes[i]==(g_gif_mode==3 && y==0 && i<2 ? (uint8_t)"GI"[i] : (uint8_t)(y*ROW_BYTES+i)), "raw pixels out of order");
    } else {
        const uint8_t expected[]={0xf8,0,0x07,0xe0,0,0x1f,0xff,0xff};
        CHECK(x==0 && y==3 && w==2 && h==2, "JPEG placement wrong");
        CHECK(memcmp(bytes,expected,sizeof(expected))==0, "JPEG RGB to wire RGB565 conversion wrong");
    }
    g_rows_drawn += h;
    if(g_cancel_after_draw) g_image_current=false;
''')
        # Keep the actual downloader's JPEG callbacks/scaling; only stub the
        # ESP32 ROM decoder, which cannot execute on the host architecture.
        first = harness.index('JRESULT jd_prepare(')
        last = harness.index('// ---- Cases', first)
        harness = harness[:first] + r'''
JRESULT jd_prepare(JDEC *jd, UINT (*input)(JDEC *, BYTE *, UINT), void *pool, UINT size, void *device) {
    (void)pool; (void)size; jd->device=device;
    BYTE header[2]; CHECK(input(jd,header,2)==2 && header[0]==0xff && header[1]==0xd8, "JPEG sniff lost header");
    if(g_jpeg_mode==2) return JDR_FMT3;
    jd->width=g_jpeg_mode==3?65535:16; jd->height=g_jpeg_mode==3?65535:20;
    return JDR_OK;
}
JRESULT jd_decomp(JDEC *jd, UINT (*output)(JDEC *, void *, JRECT *), BYTE scale) {
    CHECK(scale==1, "JPEG fit scale wrong");
    uint8_t rgb[]={255,0,0,0,255,0,0,0,255,255,255,255};
    JRECT rect={.left=0,.right=1,.top=0,.bottom=1};
    return output(jd,rgb,&rect)?JDR_OK:JDR_INTR;
}
''' + harness[last:]
        cases = r'''
static void test_bunty_atomic_commit(void) {
    reset_server(); int before=g_commits;
    add_response((response_t){.status=200,.body_len=8*ROW_BYTES,.chunk=1});
    fetch("https://example.com/picture.raw");
    CHECK(g_result.ok && g_rows_drawn==8 && g_commits==before+1, "complete image did not commit");
    reset_server(); before=g_commits;
    add_response((response_t){.status=200,.body_len=5*ROW_BYTES+1,.chunk=3});
    fetch("http://example.com/truncated.raw");
    CHECK(!g_result.ok && g_commits==before, "partial raw row committed");
    reset_server();
    add_response((response_t){.status=200,.body_len=(HEIGHT+1)*ROW_BYTES});
    fetch("http://example.com/oversized.raw");
    CHECK(!g_result.ok && g_commits==before, "oversized image committed");
    reset_server();
    add_response((response_t){.status=200,.body_len=0});
    fetch("http://example.com/empty.raw");
    CHECK(!g_result.ok && g_commits==before, "empty response committed");
    reset_server(); g_cancel_after_draw=true;
    add_response((response_t){.status=200,.body_len=8*ROW_BYTES});
    fetch("http://example.com/cancel.raw");
    CHECK(!g_result.ok && strcmp(g_result.code,"cancelled")==0 && g_commits==before, "dismissed image committed");
    reset_server(); g_cancel_after_draw=false;
    add_response((response_t){.status=200,.body_len=ROW_BYTES});
    fetch("http://example.com/retry.raw");
    CHECK(g_result.ok && g_commits==before+1, "retry after dismissal failed");
}
static void test_bunty_gif_callbacks(void) {
    reset_server();g_gif_mode=1;int before=g_commits;
    add_response((response_t){.status=200,.body_len=32,.chunk=1});fetch("https://example.com/animation.gif");
    CHECK(g_result.ok && !strcmp(g_result.format,"gif") && g_commits==before+1,"GIF did not commit");
    reset_server();g_gif_mode=2;before=g_commits;
    add_response((response_t){.status=200,.body_len=32,.chunk=3});fetch("http://example.com/bad.gif");
    CHECK(!g_result.ok && g_commits==before,"invalid GIF committed");
    reset_server();g_gif_mode=3;before=g_commits;
    add_response((response_t){.status=200,.body_len=ROW_BYTES,.chunk=1});fetch("http://example.com/picture.raw");
    CHECK(g_result.ok && !strcmp(g_result.format,"rgb565") && g_commits==before+1,"raw GI prefix mistaken for GIF");
}
static void test_bunty_jpeg_callbacks(void) {
    reset_server(); g_jpeg_mode=1;
    add_response((response_t){.status=200,.body_len=32});
    const char *code=NULL,*message=NULL;
    g_deadline_hint_us=g_now_us+60*1000000LL;
    CHECK(image_fetch_start("https://example.com/image.jpg",IMAGE_FETCH_CENTRE,on_done,NULL,&code,&message), "JPEG start failed");
    CHECK(g_result.ok && strcmp(g_result.format,"jpeg")==0 && g_result.scale==2 && g_result.width==8 && g_result.height==10,
          "JPEG downscaling/result wrong");
    reset_server(); g_jpeg_mode=2;
    add_response((response_t){.status=200,.body_len=32}); fetch("http://example.com/progressive.jpg");
    CHECK(!g_result.ok && strstr(g_result.message,"progressive"), "progressive JPEG not rejected");
    reset_server(); g_jpeg_mode=3;
    add_response((response_t){.status=200,.body_len=32}); fetch("http://example.com/huge.jpg");
    CHECK(!g_result.ok && strstr(g_result.message,"too large"), "oversized JPEG not rejected");
}
'''
        harness = harness.replace('int main(void) {', cases + '\nint main(void) {\n    test_bunty_atomic_commit();\n    test_bunty_jpeg_callbacks();\n    test_bunty_gif_callbacks();')
        with tempfile.TemporaryDirectory() as folder:
            temp = Path(folder)
            source, binary = temp / 'harness.c', temp / 'harness'
            source.write_text(harness)
            command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_GNU_SOURCE',
                       '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-variable',
                       '-fsanitize=undefined,bounds', '-fno-sanitize-recover=all',
                       '-DCONFIG_BUNTY_MUSE=1', '-DLINK_FAKE_CUSTOM_TASKS', '-DLINK_FAKE_CUSTOM_HEAP_CAPS',
                       '-I', str(SDK / 'tests/image_fetch_fakes'), '-I', str(SDK / 'tests/link_fakes'),
                       '-I', str(SDK / 'main'), '-I', str(ROOT / 'lib/muse_core'),
                       str(source), str(SDK / 'main/image_fetch.c'), '-o', str(binary)]
            build = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
