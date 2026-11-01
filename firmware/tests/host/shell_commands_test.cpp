#include "Shell.h"
#include "Ui.h"
#include "flow32/graphics/Display.h"
#include <esp_heap_caps.h>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <cstdio>
namespace {
bool failBulk=false,remote=false;
size_t bulkCalls=0,frames=0;
struct Transport:DisplayTransport {
  bool begin(const DisplayPanel &) override {return true;}
  bool present(const uint16_t *,int16_t,int16_t,int16_t,int16_t,int16_t) override {++frames;return true;}
} transport;
DisplayPanel spec{"test",240,320,true};
Display panel(spec,transport);
}
void *heap_caps_malloc(size_t size,uint32_t caps) {++bulkCalls;assert(caps&MALLOC_CAP_SPIRAM || caps&MALLOC_CAP_DMA);return failBulk?nullptr:malloc(size);}
void *heap_caps_calloc(size_t n,size_t size,uint32_t) {return calloc(n,size);}
void heap_caps_free(void *p) {free(p);}
namespace ui {Guard::Guard() {locked_=true;} Guard::~Guard() {if(locked_)locked_=false;} Display &display(){return panel;}}
namespace muse {void dismissDisplay(){remote=false;} bool displayActive(){return remote;}}
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
Json call(const char *name,const char *input,bool ok=true) {
  Json params(cJSON_Parse(input),cJSON_Delete);assert(params);
  Json result(shell::command(name,params.get()),cJSON_Delete);assert(result);
  assert(bool(cJSON_IsTrue(cJSON_GetObjectItem(result.get(),"ok")))==ok);return result;
}
const cJSON *payload(const Json &j) {return cJSON_GetObjectItem(j.get(),"payload");}
int main() {
  failBulk=true;const size_t bootCalls=bulkCalls;assert(shell::begin());
  assert(bulkCalls==bootCalls && !shell::visible());
  call("ui.get_state","{}");call("ui.open",R"({"view":"home"})");
  call("ui.open",R"({"view":"agenda"})",false);
  call("ui.set_agenda",R"({"items":[]})",false); // lazy admission failure is recoverable
  assert(bulkCalls==bootCalls+2);
  failBulk=false;assert(panel.begin());
  Json commands(cJSON_CreateObject(),cJSON_Delete);shell::registerCommands(commands.get());assert(cJSON_GetArraySize(commands.get())==7);
  auto *card=cJSON_GetObjectItem(commands.get(),"ui.show_card");assert(card);
  assert(cJSON_GetObjectItem(cJSON_GetObjectItem(card,"required"),"title"));
  assert(!cJSON_GetObjectItem(cJSON_GetObjectItem(card,"optional"),"title"));
  call("ui.set_agenda",R"({"items":[{"id":"a","title":"Real event"}]})");
  call("ui.set_agenda",R"({"items":[{"id":"a","title":"Changed"},{"id":"b"}]})",false);
  auto state=call("ui.get_state","{}");assert(cJSON_GetObjectItem(payload(state),"agenda_count")->valueint==1);
  call("ui.set_tasks",R"({"items":[{"id":"task","title":"Check me"}]})");
  const size_t before=bulkCalls;
  touch::Event event;event.kind=touch::Event::Kind::kTap;event.x=25;event.y=110;
  assert(shell::service(event,true,false,1000)==shell::Action::Redraw);
  assert(bulkCalls==before && frames>0); // no bulk allocations while drawing
  state=call("ui.get_state","{}");auto *tasks=cJSON_GetObjectItem(payload(state),"tasks");
  assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(tasks,0),"pending")));
  assert(cJSON_GetObjectItem(payload(state),"task_revision")->valueint==2);
  call("ui.set_tasks",R"({"items":[]})",false);
  call("ui.set_tasks",R"({"items":[],"ack_revision":2})");
  call("ui.set_notes",R"({"items":[{"id":"note","title":"Notes"}],"open":false})");
  state=call("ui.get_state","{}");assert(!strcmp(cJSON_GetObjectItem(payload(state),"view")->valuestring,"tasks"));
  call("ui.open",R"({"view":"nope"})",false);
  call("ui.set_context",R"({"utc_offset_minutes":1.5})",false);
  call("ui.set_agenda",R"({"items":[],"open":"yes"})",false);
  remote=true;state=call("ui.get_state","{}");assert(!strcmp(cJSON_GetObjectItem(payload(state),"view")->valuestring,"image_or_text"));
  call("ui.open",R"({"view":"apps"})");assert(!remote && shell::visible());
  shell::hide();assert(!shell::visible() && shell::resume());
  call("ui.open",R"({"view":"face"})");assert(!shell::visible() && !shell::resume() && shell::takeFaceRequest());
  puts("Shell commands: registration, OOM, transactional updates, pending actions, screen ownership and allocation-free rendering passed");
}
