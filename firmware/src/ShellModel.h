#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
namespace shell {
constexpr size_t kMaxItems=12;
enum class View : uint8_t { Home, Apps, Agenda, Tasks, Notes, Card, Face };
struct Item {
  char id[49]{},title[81]{},meta[65]{},body[385]{},time[25]{};
  bool done=false,pending=false;
};
struct Collection {
  char title[41]{},source[41]{};
  Item items[kMaxItems]{};
  uint8_t count=0;
  bool received=false;
};
struct Card { char title[41]{},value[25]{},subtitle[81]{},body[769]{},source[41]{}; bool received=false; };
struct Model {
  Collection agenda,tasks,notes;
  Card card;
  int utcOffsetMinutes=330;
  uint32_t taskRevision=0;
};
struct Navigation {
  View view=View::Home;
  uint8_t page=0;
  int selected=-1;
  size_t textPage=0, history[32]{};
  uint8_t depth=0;
  void forward(size_t next) {
    if(!next) {textPage=0;depth=0;return;}
    if(next>textPage && depth<32) {history[depth++]=textPage;textPage=next;}
  }
  void back() { if(depth) textPage=history[--depth]; }
  void open(View next) { view=next; page=0; selected=-1; textPage=0; depth=0; }
};
inline const char *name(View view) {
  switch(view) {
    case View::Home:return "home"; case View::Apps:return "apps"; case View::Agenda:return "agenda";
    case View::Tasks:return "tasks"; case View::Notes:return "notes";
    case View::Card:return "card"; case View::Face:return "face";
  }
  return "home";
}
inline bool parseView(const char *value,View &view) {
  if(!value) return false;
  for(auto v:{View::Home,View::Apps,View::Agenda,View::Tasks,View::Notes,View::Card,View::Face})
    if(!strcmp(value,name(v))) { view=v; return true; }
  return false;
}
inline const Collection *collection(const Model &model,View view) {
  return view==View::Agenda?&model.agenda:view==View::Tasks?&model.tasks:view==View::Notes?&model.notes:nullptr;
}
inline bool pending(const Model &model) {
  for(size_t i=0;i<model.tasks.count;++i) if(model.tasks.items[i].pending) return true;
  return false;
}
inline bool toggleTask(Model &model,size_t index) {
  if(index>=model.tasks.count) return false;
  auto &item=model.tasks.items[index]; item.done=!item.done; item.pending=true;
  ++model.taskRevision; return true;
}
}
