#include "ShellData.h"
#include "MuseText.h"
#include <cmath>
#include <cstdio>
namespace shell {
namespace {
template<size_t N> bool text(const cJSON *object,const char *key,char (&out)[N],bool required=false) {
  const auto *value=cJSON_GetObjectItemCaseSensitive(object,key);
  if(!value) { out[0]=0; return !required; }
  if(!cJSON_IsString(value) || !value->valuestring || strnlen(value->valuestring,N)>=N ||
     (required && !value->valuestring[0])) return false;
  muse::screenAscii(value->valuestring,out,N);
  if(strcmp(key,"body")) for(char *p=out;*p;++p) if(*p=='\n') *p=' ';
  return !required || out[0];
}
bool boolean(const cJSON *object,const char *key,bool &out) {
  const auto *v=cJSON_GetObjectItemCaseSensitive(object,key);
  if(!v) { out=false; return true; }
  if(!cJSON_IsBool(v)) return false;
  out=cJSON_IsTrue(v); return true;
}
}
const char *context(Model &model,const cJSON *params) {
  const auto *offset=cJSON_GetObjectItemCaseSensitive(params,"utc_offset_minutes");
  if(!cJSON_IsNumber(offset) || !std::isfinite(offset->valuedouble) ||
     offset->valuedouble!=offset->valueint || offset->valueint< -720 || offset->valueint>840)
    return "utc_offset_minutes must be an integer from -720 to 840";
  model.utcOffsetMinutes=offset->valueint;
  return nullptr;
}
const char *update(Model &model,View view,const cJSON *params) {
  if(!cJSON_IsObject(params)) return "params must be an object";
  if(view==View::Card) {
    auto &card=model.card; card=Card{};
    if(!text(params,"title",card.title,true) || !text(params,"value",card.value) ||
       !text(params,"subtitle",card.subtitle) || !text(params,"body",card.body) || !text(params,"source",card.source))
      return "card limits: title/source 40, value 24, subtitle 80, body 768 UTF-8 bytes";
    card.received=true; return nullptr;
  }
  Collection *data=view==View::Agenda?&model.agenda:view==View::Tasks?&model.tasks:view==View::Notes?&model.notes:nullptr;
  if(!data) return "unsupported view";
  if(view==View::Tasks && pending(model)) {
    const auto *ack=cJSON_GetObjectItemCaseSensitive(params,"ack_revision");
    if(!cJSON_IsNumber(ack) || ack->valuedouble!=double(model.taskRevision))
      return "pending local task changes: read ui.get_state, sync provider, then acknowledge task_revision";
  }
  const auto *items=cJSON_GetObjectItemCaseSensitive(params,"items");
  if(!cJSON_IsArray(items) || cJSON_GetArraySize(items)>int(kMaxItems)) return "items must be an array of at most 12 objects";
  *data=Collection{};
  if(!text(params,"title",data->title) || !text(params,"source",data->source)) return "title/source limit is 40 UTF-8 bytes";
  const cJSON *entry=nullptr;
  cJSON_ArrayForEach(entry,items) {
    auto &item=data->items[data->count];
    if(!cJSON_IsObject(entry) || !text(entry,"title",item.title,true) || !text(entry,"meta",item.meta) ||
       !text(entry,"body",item.body) || !text(entry,"time",item.time) || !boolean(entry,"done",item.done))
      return "item limits: title 80 (required), meta 64, body 384, time 24 UTF-8 bytes; done must be boolean";
    // IDs round-trip unchanged to the provider, unlike display strings.
    const auto *id=cJSON_GetObjectItemCaseSensitive(entry,"id");
    if(!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0] || strnlen(id->valuestring,sizeof(item.id))>=sizeof(item.id))
      return "every item needs a unique id of 1 to 48 bytes";
    memcpy(item.id,id->valuestring,strlen(id->valuestring)+1);
    for(size_t i=0;i<data->count;++i) if(!strcmp(data->items[i].id,item.id)) return "item ids must be unique";
    ++data->count;
  }
  data->received=true;
  if(view==View::Tasks) ++model.taskRevision;
  return nullptr;
}
}
