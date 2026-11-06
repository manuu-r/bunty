#include "ShellData.h"
#include "ShellView.h"
#include <cassert>
#include <memory>
#include <string>
#include <cstdio>
#include <vector>
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
Json json(const char *value) {Json p(cJSON_Parse(value),cJSON_Delete);assert(p);return p;}
class Raster:public Adafruit_GFX {
 public:
  Raster():Adafruit_GFX(240,320) {}
  size_t writes=0;
  void drawPixel(int16_t x,int16_t y,uint16_t) override {
    assert(x>=0 && x<240 && y>=0 && y<320);++writes;
  }
};
int main() {
  auto model=std::make_unique<shell::Model>();
  const auto valid=json(R"({"source":"Tasks","items":[{"id":"one","title":"First task","meta":"Today","body":"Details"},{"id":"two","title":"Second task","done":true}]})");
  assert(!shell::update(*model,shell::View::Tasks,valid.get()));
  assert(model->tasks.count==2 && model->tasks.received && model->taskRevision==1);
  assert(shell::toggleTask(*model,0));assert(shell::pending(*model));
  assert(model->tasks.items[0].done && model->taskRevision==2);
  assert(shell::update(*model,shell::View::Tasks,valid.get())); // pending edits cannot disappear
  cJSON_AddNumberToObject(valid.get(),"ack_revision",1);
  assert(shell::update(*model,shell::View::Tasks,valid.get())); // stale ack
  cJSON_ReplaceItemInObject(valid.get(),"ack_revision",cJSON_CreateNumber(2));
  assert(!shell::update(*model,shell::View::Tasks,valid.get()));
  assert(!shell::pending(*model) && model->taskRevision==3);
  for(const char *bad:{R"({"items":{}})",R"({"items":[{"title":"No id"}]})",R"({"items":[{"id":"x","title":""}]})",R"({"items":[{"id":"x","title":"One"},{"id":"x","title":"Two"}]})",R"({"items":[{"id":"x","title":"One","done":"yes"}]})"}) {
    auto copy=std::make_unique<shell::Model>(*model);const auto input=json(bad);
    assert(shell::update(*copy,shell::View::Notes,input.get()));
    assert(model->tasks.count==2 && !model->notes.received);
  }
  auto many=json(R"({"items":[]})");auto *items=cJSON_GetObjectItem(many.get(),"items");
  for(int i=0;i<13;++i) {
    auto *item=cJSON_CreateObject();const auto id=std::to_string(i);cJSON_AddStringToObject(item,"id",id.c_str());
    cJSON_AddStringToObject(item,"title","Long enough");cJSON_AddItemToArray(items,item);
  }
  assert(shell::update(*model,shell::View::Notes,many.get()));
  cJSON_DeleteItemFromArray(items,12);assert(!shell::update(*model,shell::View::Notes,many.get()));
  assert(model->notes.count==12);
  auto *entry=cJSON_GetArrayItem(items,0);
  cJSON_ReplaceItemInObject(entry,"title",cJSON_CreateString(std::string(81,'x').c_str()));
  assert(shell::update(*model,shell::View::Notes,many.get()));
  auto unicode=json(R"({"items":[{"id":"provider-é","title":"hello\nworld é","body":"line one\nline two"}]})");
  assert(!shell::update(*model,shell::View::Notes,unicode.get()));
  assert(!strcmp(model->notes.items[0].id,"provider-é"));
  assert(!strcmp(model->notes.items[0].title,"hello world ?"));
  assert(strchr(model->notes.items[0].body,'\n'));
  for(const char *bad:{R"({"utc_offset_minutes":841})",R"({"utc_offset_minutes":-721})",R"({"utc_offset_minutes":1.5})"}) {
    auto input=json(bad);assert(shell::context(*model,input.get()));assert(model->utcOffsetMinutes==330);
  }
  auto offset=json(R"({"utc_offset_minutes":-300})");assert(!shell::context(*model,offset.get()));
  shell::Navigation nav;nav.open(shell::View::Tasks);
  assert(shell::tap(*model,nav,25,110,0)==shell::Action::Redraw && shell::pending(*model));
  assert(shell::tap(*model,nav,90,110,0)==shell::Action::Redraw && nav.selected==0);
  nav.forward(120);nav.forward(240);nav.back();assert(nav.textPage==120);nav.back();assert(!nav.textPage);
  nav.forward(100);nav.forward(0);assert(!nav.textPage && !nav.depth);
  assert(shell::tap(*model,nav,110,299,0)==shell::Action::Redraw && nav.selected==-1);
  assert(shell::tap(*model,nav,180,299,0)==shell::Action::Face); // the old mic corner is just Home now
  assert(shell::tap(*model,nav,239,0,0)==shell::Action::Face);
  assert(shell::tap(*model,nav,-1,0,0)==shell::Action::None);
  nav.open(shell::View::Apps);shell::tap(*model,nav,160,205,0);assert(nav.view==shell::View::Apps); // unloaded card is absent
  shell::tap(*model,nav,100,110,0);assert(nav.view==shell::View::Tasks);
  shell::Navigation emptyNav;emptyNav.open(shell::View::Apps);
  assert(shell::tapRecent(nullptr,emptyNav,100,110)==shell::Action::None);
  assert(shell::tapRecent(nullptr,emptyNav,100,260)==shell::Action::Settings);
  // Maximum-length display fields, paged bodies, empty states, all view layouts.
  auto card=json(R"({"title":"Forecast","value":"24 C","subtitle":"Light rain","body":""})");
  cJSON_ReplaceItemInObject(card.get(),"body",cJSON_CreateString(std::string(768,'W').c_str()));
  assert(!shell::update(*model,shell::View::Card,card.get()));
  Raster raster;shell::renderRecent(raster,nullptr);shell::Status status;status.clockValid=true;snprintf(status.clock,6,"23:59");
  for(auto view:{shell::View::Home,shell::View::Apps,shell::View::Agenda,shell::View::Tasks,shell::View::Notes,shell::View::Card}) {
    nav.open(view);shell::render(raster,*model,nav,status);
  }
  nav.open(shell::View::Card);size_t pages=0;
  for(;;) {
    const auto next=shell::render(raster,*model,nav,status);++pages;
    if(!next) break;
    assert(next>nav.textPage);nav.forward(next);assert(pages<32);
  }
  assert(pages>1 && raster.writes>0);
  printf("Shell: atomic staging validation, provider acknowledgements, pagination, touch navigation and render bounds passed (%zu-byte model)\n",sizeof(shell::Model));
}
