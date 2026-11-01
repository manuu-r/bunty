"""Run the pinned SDK's real chat parser/turn tests with Bunty hooks enabled."""
from pathlib import Path
import io
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / '.deps/muse/esp32'


class MuseChatTests(unittest.TestCase):
    def test_chat_recovery_and_checked_initialization(self):
        for script in ('prepare_deps.py', 'adapt_muse.py'):
            result = subprocess.run([sys.executable, str(ROOT / 'scripts' / script)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        path = SDK / 'tests/test_muse_chat_link_errors.py'
        harness = path.read_text()
        harness = harness.replace("cjson = ROOT / 'managed_components/espressif__cjson/cJSON'",
                                  'cjson = Path(' + repr(str(ROOT / 'managed_components/espressif__cjson/cJSON')) + ')')
        harness = harness.replace('#include \"muse_chat_priv.h\"', '#define CONFIG_BUNTY_MUSE 1\n#include \"muse_chat_priv.h\"')
        harness = harness.replace('typedef int SemaphoreHandle_t;', r'''
static unsigned bunty_reply_count;
static char bunty_reply_text[1024];
static void bunty_muse_reply(const char *text) {
    assert(text && text[0]); ++bunty_reply_count;
    strlcpy(bunty_reply_text,text,sizeof(bunty_reply_text));
}
static bool fail_mutex, fail_queue;
static unsigned mutex_deletes, queue_deletes;
typedef void *SemaphoreHandle_t;''')
        harness = harness.replace('static int xSemaphoreCreateMutex(void) { return 1; }', r'''
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return fail_mutex ? NULL : (void *)1; }
static void vSemaphoreDelete(SemaphoreHandle_t lock) { assert(lock); ++mutex_deletes; }''')
        harness = harness.replace('xSemaphoreTake(int lock,', 'xSemaphoreTake(SemaphoreHandle_t lock,')
        harness = harness.replace('xSemaphoreGive(int lock)', 'xSemaphoreGive(SemaphoreHandle_t lock)')
        harness = harness.replace('assert(count == 8 && size <= 80);',
                                  'if(fail_queue) return NULL; assert(count == 8 && size <= 80);')
        harness = harness.replace('static void xQueueReset(',
                                  'static void vQueueDelete(QueueHandle_t q) { assert(q); ++queue_deletes; }\nstatic void xQueueReset(')
        harness = harness.replace('assert(argc==2); muse_hatch_start();', r'''
    assert(argc==2);
    fail_mutex=true;
    assert(!bunty_muse_chat_init() && !s_rx_lock && !s_events);
    assert(queue_deletes==1 && mutex_deletes==0);
    fail_mutex=false; fail_queue=true;
    assert(!bunty_muse_chat_init() && !s_rx_lock && !s_events);
    assert(queue_deletes==1 && mutex_deletes==1);
    fail_queue=false;
    assert(bunty_muse_chat_init());
    SemaphoreHandle_t lock=s_rx_lock; QueueHandle_t events=s_events;
    assert(bunty_muse_chat_init() && lock==s_rx_lock && events==s_events);
''')
        harness = harness.replace('static int64_t muse_link_req_open(', r'''
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static void *heap_caps_malloc(size_t size,unsigned caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));return test_malloc(size);
}
static int64_t muse_link_req_open(''')
        harness = harness.replace('cJSON *text=cJSON_Parse(MUSE_HATCH_NOTE_HEAD MUSE_HATCH_NOTE_TAIL);', r'''
    cJSON *text=cJSON_Parse(MUSE_HATCH_NOTE_HEAD MUSE_HATCH_NOTE_TAIL);
    assert(text);
    const char *instruction=cJSON_GetStringValue(cJSON_GetObjectItem(text,"message"));
    assert(instruction && strstr(instruction,"bunty.gesture") && strstr(instruction,"servo movement"));
    assert(strstr(instruction,"ui tools") && strstr(instruction,"out of the spoken text"));
    assert(strstr(instruction,"Only fetch or show screen content when the user asks"));
    assert(strstr(instruction,"use an existing direct URL") && strstr(instruction,"approval is handled in the Muse app"));
    cJSON *file=cJSON_GetArrayItem(cJSON_GetObjectItem(text,"items"),0);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(file,"mime_type")),"audio/wav"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(file,"filename")),"voice_note.wav"));
''')
        harness = harness.replace('now+=20000000; pump(); assert(s_turn.phase==T_IDLE);',
                                  'assert(bunty_reply_count==1 && !strcmp(bunty_reply_text,"Hello world"));\n    now+=20000000; pump(); assert(s_turn.phase==T_IDLE);')
        harness = harness.replace('static void text_modality(void) {', r'''
static void reply_progress(void) {
    begin();release();note_ack();
    now+=59000000;delta("text_append","reply","note","Working ");pump();
    now+=59000000;pump();assert(s_turn.phase==T_REPLY && !s_turn.error[0]);
    final("reply","note","Long streamed response");assert_replied("Long streamed response");
    muse_hatch_turn_cancel();begin();release();note_ack();
    now+=59000000;delta("text_append","other","elsewhere","Unrelated");pump();
    now+=REPLY_TIMEOUT_US;pump();assert(s_turn.phase==T_IDLE && s_turn.error[0]);
    begin();release();note_ack();
    for(int i=0;i<4;i++) {now+=50000000;delta("text_append","reply","note","More ");pump();}
    assert(s_turn.phase==T_IDLE && s_turn.error[0]);
}
static void reply_then_disconnect(void) {
    for(int early=0;early<2;early++) {
        begin(); release(); if(!early) note_ack();
        final("reply","note","Delivered reply");
        feed(RX_SUB,-1,NULL,0,true);
        if(early) {pump();assert(s_turn.phase==T_ACK);note_ack();} else pump();
        assert(s_turn.phase==T_IDLE && s_turn.replied && !s_turn.error[0]);
        assert(!strcmp(bunty_reply_text,"Delivered reply"));
        char text[72]; muse_hatch_ev_t ev; bool done=false;
        while((ev=muse_hatch_turn_event(text,sizeof(text)))!=MUSE_HATCH_EV_NONE) {
            assert(ev!=MUSE_HATCH_EV_ERROR); if(ev==MUSE_HATCH_EV_DONE)done=true;
        }
        assert(done);
    }
    begin();release();note_ack();final("other","elsewhere","Unrelated");
    feed(RX_SUB,-1,NULL,0,true);pump();
    assert(s_turn.phase==T_REPLY && !s_turn.error[0] && !s_turn.replied);
}
static void text_modality(void) {
''')
        harness = harness.replace('int statuses[]={403,401,404,-1};', 'int statuses[]={403,401,404};')
        harness = harness.replace('for(unsigned i=0;i<4;i++) {', 'for(unsigned i=0;i<3;i++) {')
        harness = harness.replace('assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"LOST CONNECTION"));',
            'assert(s_turn.phase==T_REPLY && !s_turn.error[0]); muse_hatch_turn_cancel();')
        harness = harness.replace('static void text_modality(void) {', r'''static void recovery(void) {
    begin();release();note_ack();final("reply","note","Complete reply");
    feed(RX_SUB,503,NULL,0,true);pump();
    assert(s_turn.phase==T_IDLE && s_turn.replied && !s_turn.error[0]);
    muse_hatch_turn_cancel();
    for(int status=-1;status<=503;status+=504) {
        begin();release();note_ack();int note=requests[RX_NOTE].id;
        feed(RX_SUB,status,NULL,0,true);pump();
        assert(s_turn.phase==T_REPLY && !s_turn.error[0]);
        now+=500001;pump();assert(s_turn.sub_retries==1);
        assert(requests[RX_NOTE].id==note); // no repeat of the user's upload
        final("reply","note","Recovered reply");assert_replied("Recovered reply");
        muse_hatch_turn_cancel();
    }
    begin();release();note_ack();int note=requests[RX_NOTE].id;
    int64_t started=now;
    for(int i=0;i<3;i++) {
        feed(RX_SUB,503,NULL,0,true);pump();now+=4000000;pump();
        assert(s_turn.phase==T_REPLY && s_turn.sub_retries==(unsigned)(i+1));
    }
    feed(RX_SUB,503,NULL,0,true);pump();
    assert(s_turn.phase==T_REPLY && !s_turn.error[0]);
    now+=5000000;pump();assert(s_turn.sub_retries==4 && requests[RX_NOTE].id==note);
    final("reply","note","Approved after retries");assert_replied("Approved after retries");
    muse_hatch_turn_cancel();begin();release();note_ack();started=now;note=requests[RX_NOTE].id;
    for(int i=0;i<180;i++) {
        feed(RX_SUB,503,NULL,0,true);pump();
        if(s_turn.phase==T_IDLE)break;
        assert(!s_turn.error[0] && requests[RX_NOTE].id==note);
        now+=1000000;
    }
    pump();assert(now-started>=180000000 && s_turn.phase==T_IDLE && strstr(s_turn.error,"NO REPLY"));
    assert(s_turn.sub_retries<=10);
    muse_hatch_turn_cancel();begin();release();note_ack();started=now;fail_send=50;
    feed(RX_SUB,-1,NULL,0,true);
    while(now-started<180000000) {pump();assert(s_turn.phase==T_REPLY);now+=1000000;}
    pump();assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"NO REPLY"));fail_send=0;
    begin();release();note_ack();now+=61000000;pump();
    assert(s_turn.phase==T_REPLY && !s_turn.error[0]);
    final("reply","note","Approved later");assert_replied("Approved later");
}
static void text_modality(void) {''')
        harness = harness.replace('case 10: bounded_rejections(); break;', 
                                  'case 10: bounded_rejections(); break; case 11: reply_then_disconnect(); break; case 12: reply_progress(); break; case 13: recovery(); break;')
        harness += '\ndef test_reply_then_disconnect(self): self.run_case(11)\nChatLinkErrors.test_reply_then_disconnect=test_reply_then_disconnect\ndef test_reply_progress(self): self.run_case(12)\nChatLinkErrors.test_reply_progress=test_reply_progress\n'
        harness = harness.replace('feed(RX_SUB,200,NULL,0,true); assert_replied("Terminal line");',
                                  'feed(RX_SUB,200,NULL,0,true); pump(); assert(s_turn.phase==T_IDLE && s_turn.replied && !s_turn.error[0]);')
        harness = harness.replace('int16_t pcm[1024]={0}; muse_hatch_turn_audio(pcm,1024);',
                                  'int16_t pcm[STAGE_BYTES/2]={0}; muse_hatch_turn_audio(pcm,STAGE_BYTES/2);')
        harness += '\ndef test_transient_stream_recovery(self): self.run_case(13)\nChatLinkErrors.test_transient_stream_recovery=test_transient_stream_recovery\n'
        namespace = {'__file__': str(path), '__name__': 'bunty_sdk_chat_harness'}
        exec(compile(harness, str(path), 'exec'), namespace)
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(namespace['ChatLinkErrors'])
        output = io.StringIO()
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(suite)
        self.assertTrue(result.wasSuccessful(), output.getvalue())
        self.assertEqual(result.testsRun, 14)
