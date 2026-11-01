#include "Shell.h"
#include "ShellData.h"
#include "MuseDisplay.h"
#include "Memory.h"
#include "Ui.h"
#include "flow32/graphics/Display.h"
#include <new>
namespace shell {
namespace {
Model *live=nullptr,*staging=nullptr;
Navigation nav;
bool visible_=false,returnToShell=false,dirty=true,faceRequested=false;
uint32_t activity=0;
size_t nextPage=0;
void open(View view) {
  muse::dismissDisplay();faceRequested=false;
  if(view==View::Face) {visible_=false;returnToShell=false;faceRequested=true;}
  else {nav.open(view);visible_=returnToShell=true;}
  dirty=true;activity=millis();
}
cJSON *reply(bool ok,const char *message=nullptr) {
  auto *r=cJSON_CreateObject(); if(!r)return nullptr;
  cJSON_AddBoolToObject(r,"ok",ok);
  if(message) {
    auto *error=cJSON_AddObjectToObject(r,"error");
    cJSON_AddStringToObject(error,"code","invalid_params");cJSON_AddStringToObject(error,"message",message);
  } else cJSON_AddObjectToObject(r,"payload");
  return r;
}
void definition(cJSON *commands,const char *name,const char *description,const char *requiredKey,const char *type,const char *paramDescription,bool update=false) {
  auto *tool=cJSON_AddObjectToObject(commands,name);cJSON_AddStringToObject(tool,"description",description);
  auto *required=cJSON_AddObjectToObject(tool,"required");
  if(requiredKey) {
    auto *param=cJSON_AddObjectToObject(required,requiredKey);
    cJSON_AddStringToObject(param,"type",type);cJSON_AddStringToObject(param,"description",paramDescription);
  }
  auto *optional=cJSON_AddObjectToObject(tool,"optional");
  if(update) {
    for(const char *key:{"title","source"}) {
      auto *p=cJSON_AddObjectToObject(optional,key);cJSON_AddStringToObject(p,"type","string");
      cJSON_AddStringToObject(p,"description","Display label, at most 40 UTF-8 bytes. Source should identify the real provider; do not imply live sync.");
    }
    auto *p=cJSON_AddObjectToObject(optional,"open");cJSON_AddStringToObject(p,"type","boolean");
    cJSON_AddStringToObject(p,"description","Show this view now (default true). False updates the stored view without changing the screen.");
  }
}
}
bool begin() {return true;} // Content storage is admitted only on first data request.
bool ensureStorage() {
  if(live)return true;
  auto *a=memory::external(sizeof(Model)),*b=memory::external(sizeof(Model));
  if(!a || !b) {heap_caps_free(a);heap_caps_free(b);return false;}
  live=new(a) Model;staging=new(b) Model;return true;
}
bool visible() {return visible_;}
bool resume() {if(!returnToShell)return false;visible_=true;dirty=true;return true;}
void hide() {visible_=false;}
void showHome() {open(View::Apps);}
void showFace() {open(View::Face);}
bool takeFaceRequest() {const bool result=faceRequested;faceRequested=false;return result;}
void invalidate() {dirty=true;}
uint32_t activityAt() {return activity;}
void wake(uint32_t now) {activity=now;}
Action service(const touch::Event &event,bool,bool,uint32_t now) {
  if(!visible())return Action::None;
  Action action=Action::None;
  if(event.kind!=touch::Event::Kind::kNone) {
    activity=now;
    action=event.kind==touch::Event::Kind::kLongPress?Action::Settings:
      (live?tap(*live,nav,event.x,event.y,nextPage):tapRecent(nullptr,nav,event.x,event.y));
    if(action==Action::Redraw)dirty=true;
  }
  if(dirty) {
    Status status;
    if(live)nextPage=render(ui::display(),*live,nav,status);
    else {renderRecent(ui::display(),nullptr);nextPage=0;}
    ui::display().present();
    dirty=false;
  }
  return action;
}
void registerCommands(cJSON *commands) {
  constexpr const char *items="Array of up to 12 objects. Each requires unique id (1-48 bytes), title (1-80 bytes). Optional time (24 bytes), meta (64 bytes), body (384 bytes), done (boolean). Send formatted times/dates in the user's timezone, ordered for display. Text limits are UTF-8 bytes; bitmap glyphs are ASCII.";
  definition(commands,"ui.set_agenda","Replace Bunty's agenda snapshot. Fetch the user's real calendar first. Send future events in chronological order, include day/date in the title or time. Empty items means no events. This does not edit a calendar.","items","array",items,true);
  definition(commands,"ui.set_tasks","Replace Bunty's task snapshot. Fetch real tasks first. Local checkmarks are pending actions, not confirmed provider changes. If ui.get_state reports pending changes, apply them through the provider and include its task_revision as ack_revision before replacing. Never acknowledge a failed provider update.","items","array",items,true);
  auto *optional=cJSON_GetObjectItem(cJSON_GetObjectItem(commands,"ui.set_tasks"),"optional");
  auto *ack=cJSON_AddObjectToObject(optional,"ack_revision");cJSON_AddStringToObject(ack,"type","number");
  cJSON_AddStringToObject(ack,"description","Exact task_revision from ui.get_state, only after pending local changes have been synchronized with the provider.");
  definition(commands,"ui.set_notes","Replace Bunty's saved-on-screen notes snapshot. Use title/meta for the list and body for the paged detail. Data is held in RAM and must be resent after reboot; this does not save to an external notes provider.","items","array",items,true);
  definition(commands,"ui.show_card","Show a structured information card for weather, a reminder, a metric, travel status, or a short briefing. Use truthful source data. No automatic refresh; resend when values change.","title","string","Required card title, 1-40 UTF-8 bytes.",true);
  optional=cJSON_GetObjectItem(cJSON_GetObjectItem(commands,"ui.show_card"),"optional");
  cJSON_DeleteItemFromObject(optional,"title");
  for(const char *key:{"value","subtitle","body"}) {
    auto *p=cJSON_AddObjectToObject(optional,key);cJSON_AddStringToObject(p,"type","string");
    cJSON_AddStringToObject(p,"description",!strcmp(key,"value")?"Primary value, up to 24 UTF-8 bytes.":!strcmp(key,"subtitle")?"Supporting line, up to 80 UTF-8 bytes.":"Paged detail, up to 768 UTF-8 bytes.");
  }
  definition(commands,"ui.open","Open only previously requested content without fetching anything. home/face restores the companion; apps lists loaded content only. Opening a view dismisses images. Unloaded views are rejected.","view","string","home, apps, agenda, tasks, notes, card, or face");
  definition(commands,"ui.set_context","Set the clock's UTC offset. Muse should update this when the user's timezone or daylight saving changes; it does not change UTC system time.","utc_offset_minutes","number","Integer -720 to 840. Initial default is +330 (India).");
  definition(commands,"ui.get_state","Read active view, item counts, clock offset, task_revision and all tasks with done/pending flags. Poll to retrieve local checkmarks before modifying provider tasks. Reading does not acknowledge or discard actions. Display snapshots are volatile and do not establish a provider connection.",nullptr,nullptr,nullptr);
}
cJSON *command(const char *name,const cJSON *params) {
  ui::Guard guard;
  if(!strcmp(name,"ui.get_state")) {
    auto *r=reply(true);auto *p=cJSON_GetObjectItem(r,"payload");
    cJSON_AddStringToObject(p,"view",muse::displayActive()?"image_or_text":visible()?shell::name(nav.view):"face_or_voice");
    cJSON_AddNumberToObject(p,"tasks_count",live?live->tasks.count:0);
    cJSON_AddBoolToObject(p,"card_received",live && live->card.received);
    cJSON_AddBoolToObject(p,"content_storage_allocated",live!=nullptr);
    cJSON_AddNumberToObject(p,"agenda_count",live?live->agenda.count:0);cJSON_AddNumberToObject(p,"notes_count",live?live->notes.count:0);
    cJSON_AddBoolToObject(p,"agenda_received",live && live->agenda.received);cJSON_AddBoolToObject(p,"tasks_received",live && live->tasks.received);cJSON_AddBoolToObject(p,"notes_received",live && live->notes.received);
    cJSON_AddBoolToObject(p,"volatile",true);cJSON_AddNumberToObject(p,"utc_offset_minutes",live?live->utcOffsetMinutes:330);
    cJSON_AddNumberToObject(p,"task_revision",live?live->taskRevision:0);
    auto *tasks=cJSON_AddArrayToObject(p,"tasks");
    for(size_t i=0;live && i<live->tasks.count;++i) {
      const auto &item=live->tasks.items[i];auto *entry=cJSON_CreateObject();
      cJSON_AddStringToObject(entry,"id",item.id);cJSON_AddStringToObject(entry,"title",item.title);
      cJSON_AddBoolToObject(entry,"done",item.done);cJSON_AddBoolToObject(entry,"pending",item.pending);cJSON_AddItemToArray(tasks,entry);
    }
    return r;
  }
  if(!cJSON_IsObject(params))return reply(false,"params must be an object");
  if(!strcmp(name,"ui.open")) {
    const auto *value=cJSON_GetObjectItemCaseSensitive(params,"view");View view;
    if(!cJSON_IsString(value) || !parseView(value->valuestring,view))return reply(false,"unknown view");
    if(view==View::Home)view=View::Face;
    if(view!=View::Face && view!=View::Apps && (!live ||
       (view==View::Card?!live->card.received:!collection(*live,view)->received)))
      return reply(false,"View not loaded; send its requested data first");
    open(view);return reply(true);
  }
  View view;
  const bool isContext=!strcmp(name,"ui.set_context");
  if(!strcmp(name,"ui.set_agenda"))view=View::Agenda;
  else if(!strcmp(name,"ui.set_tasks"))view=View::Tasks;
  else if(!strcmp(name,"ui.set_notes"))view=View::Notes;
  else if(!strcmp(name,"ui.show_card"))view=View::Card;
  else if(isContext)view=nav.view;
  else return reply(false,"unknown UI command");
  const auto *show=cJSON_GetObjectItemCaseSensitive(params,"open");
  if(show && !cJSON_IsBool(show))return reply(false,"open must be boolean");
  if(!ensureStorage())return reply(false,"Bunty UI storage unavailable");
  *staging=*live;
  const auto *error=isContext?context(*staging,params):update(*staging,view,params);
  if(error)return reply(false,error);
  std::swap(live,staging);dirty=true;
  // A background replacement resets pagination only for the changed view.
  if(!isContext && nav.view==view)nav.open(view);
  if(!isContext && (!show || cJSON_IsTrue(show)))open(view);
  return reply(true);
}
}
