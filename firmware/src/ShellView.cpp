#include "ShellView.h"
#include "Fonts.h"
#include "MuseText.h"
#include <cstdio>
#include <algorithm>
namespace shell {
namespace {
constexpr uint16_t rgb(unsigned r,unsigned g,unsigned b) { return ((r&248)<<8)|((g&252)<<3)|(b>>3); }
constexpr uint16_t bg=rgb(6,8,15),ink=rgb(233,241,223),muted=rgb(129,149,132),accent=rgb(193,225,156),line=rgb(43,55,53);
void font(Adafruit_GFX &d,const GFXfont *f) { d.setFont(f); d.setTextSize(1); d.setTextWrap(false); }
int width(Adafruit_GFX &d,const char *value) { int16_t x,y; uint16_t w,h; d.getTextBounds(value,0,0,&x,&y,&w,&h); return w; }
void text(Adafruit_GFX &d,const char *value,int x,int y,uint16_t color=ink,const GFXfont *f=BuntyFonts::kBody) {
  font(d,f); d.setTextColor(color); d.setCursor(x,y); d.print(value);
}
void small(Adafruit_GFX &d,const char *value,int x,int y,uint16_t color=muted) {
  char bounded[40]; const size_t count=std::min<size_t>(sizeof(bounded)-1,std::max(0,(228-x)/6));
  snprintf(bounded,sizeof(bounded),"%.*s",int(count),value);
  if(strlen(value)>count && count>=2) {bounded[count-2]='.';bounded[count-1]='.';}
  text(d,bounded,x,y,color,nullptr);
}
void fit(Adafruit_GFX &d,const char *value,int x,int y,int available,uint16_t color=ink,const GFXfont *f=BuntyFonts::kBody) {
  char bounded[128]; snprintf(bounded,sizeof(bounded),"%s",value); font(d,f);
  if(width(d,bounded)>available) {
    size_t n=strlen(bounded);
    while(n && width(d,bounded)>available-12) bounded[--n]=0;
    if(n+3<sizeof(bounded)) strcat(bounded,"..");
  }
  text(d,bounded,x,y,color,f);
}
void chevron(Adafruit_GFX &d,int x,int y,bool forward) {
  const int dx=forward?5:-5; d.drawLine(x,y-5,x+dx,y,muted); d.drawLine(x+dx,y,x,y+5,muted);
}
void footer(Adafruit_GFX &d,const Navigation &n) {
  small(d,n.selected>=0?"BACK":"HOME",18,295,muted);
  // One quiet microphone target; content stays the focus.
  d.fillRoundRect(201,282,8,14,4,ink);
  d.drawCircleHelper(205,293,8,0x4|0x8,ink);
  d.drawFastVLine(205,302,5,ink);d.drawFastHLine(200,307,11,ink);
}
void empty(Adafruit_GFX &d,const char *label,const char *hint,bool) {
  fit(d,label,16,144,208);small(d,hint,16,168);
}
size_t body(Adafruit_GFX &d,const char *value,size_t at,int top,size_t rows) {
  font(d,BuntyFonts::kBody);
  return muse::textPage(value,at,208,rows,[&](const char *s){return width(d,s);},
    [&](size_t row,const char *s){text(d,s,16,top+int(row)*22);});
}
void pager(Adafruit_GFX &d,unsigned page,unsigned pages) {
  if(pages<2) return;
  char label[20]; snprintf(label,sizeof(label),"%u / %u",page+1,pages);
  small(d,label,100,253); chevron(d,25,256,false); chevron(d,208,256,true);
}
}
namespace {
bool received(const Model *m,View v) {
  if(!m)return false;
  if(v==View::Card)return m->card.received;
  const auto *c=collection(*m,v);return c && c->received;
}
View recentAt(const Model *m,unsigned index) {
  for(auto v:{View::Agenda,View::Tasks,View::Notes,View::Card})
    if(received(m,v) && !index--)return v;
  return View::Face;
}
unsigned recentCount(const Model *m) {
  unsigned count=0;for(auto v:{View::Agenda,View::Tasks,View::Notes,View::Card})count+=received(m,v);
  return count;
}
}
void renderRecent(Adafruit_GFX &d,const Model *m) {
  d.fillScreen(bg);chevron(d,21,20,false);text(d,"Recent",16,62);
  const unsigned count=recentCount(m);
  if(!count) {fit(d,"Nothing here yet",16,144,208);small(d,"Ask Muse to show something",16,167);}
  for(unsigned i=0;i<count;++i) {
    const View v=recentAt(m,i);
    const char *label=v==View::Agenda?"Agenda":v==View::Tasks?"Tasks":v==View::Notes?"Notes":"Info";
    text(d,label,16,119+int(i)*36);chevron(d,211,113+int(i)*36,true);
  }
  small(d,"SETTINGS",16,267);Navigation n;footer(d,n);
}
Action tapRecent(const Model *m,Navigation &n,int x,int y) {
  if(x<0 || x>=240 || y<0 || y>=320)return Action::None;
  if(y<40)return Action::Face;
  if(y>=276)return x>=154?Action::Talk:Action::Face;
  if(y>=250)return Action::Settings;
  if(y>=98 && y<242) {
    const View v=recentAt(m,unsigned(y-98)/36);
    if(v!=View::Face) {n.open(v);return Action::Redraw;}
  }
  return Action::None;
}
size_t render(Adafruit_GFX &d,const Model &m,const Navigation &n,const Status &) {
  d.fillScreen(bg);
  if(n.view==View::Home || n.view==View::Apps) {renderRecent(d,&m);return 0;}
  chevron(d,21,20,false);
  size_t next=0;
  if(n.view==View::Card) {
    fit(d,m.card.title[0]?m.card.title:"Info",16,62,208);
    if(!m.card.received) empty(d,"A little room for anything","Ask Muse to show a card",false);
    else {
      small(d,m.card.source[0]?m.card.source:"FROM MUSE",16,82);

      fit(d,m.card.value,16,128,208,ink,BuntyFonts::kHeading);
      fit(d,m.card.subtitle,16,160,208,muted);
      next=body(d,m.card.body,n.textPage,193,3);
      if(n.textPage || m.card.body[next]) small(d,"<   TAP TO PAGE   >",60,254);
      if(!m.card.body[next]) next=0;
    }
  } else if(const auto *data=collection(m,n.view)) {
    const char *title=data->title[0]?data->title:n.view==View::Agenda?"Agenda":n.view==View::Tasks?"Tasks":"Notes";
    fit(d,title,14,62,212);
    small(d,pending(m)&&n.view==View::Tasks?"CHANGES WAITING FOR MUSE":data->source[0]?data->source:"FROM MUSE",16,79);
    if(!data->count) empty(d,!data->received?(n.view==View::Agenda?"Your agenda goes here":n.view==View::Tasks?"Your tasks go here":"Keep a thought here"):(n.view==View::Agenda?"Your schedule is clear":n.view==View::Tasks?"Room to breathe":"No notes saved"),
                            n.view==View::Agenda?"Ask Muse for your calendar":n.view==View::Tasks?"Ask Muse for your tasks":"Ask Muse to save a note",data->received);
    else if(n.selected>=0 && n.selected<data->count) {
      const auto &item=data->items[n.selected];
      char content[640]; snprintf(content,sizeof(content),"%s\n%s\n%s\n%s",item.title,item.time,item.meta,item.body);
      next=body(d,content,n.textPage,109,6);
      if(n.view==View::Tasks)small(d,item.pending?"Pending sync":item.done?"Completed":"Not completed",16,244);
      if(n.textPage || content[next]) {chevron(d,25,261,false);chevron(d,208,261,true);}
      if(!content[next]) next=0;
    } else {
      for(unsigned row=0;row<3;++row) {
        const size_t index=n.page*3+row; if(index>=data->count) break;
        const auto &item=data->items[index]; const int y=98+int(row)*48;
        int x=16;
        if(n.view==View::Tasks) {
          d.drawRoundRect(16,y+7,20,20,5,item.pending?accent:muted);
          if(item.done) {d.drawLine(20,y+17,25,y+22,accent);d.drawLine(25,y+22,33,y+11,accent);}
          x=48;
        }
        fit(d,item.title,x,y+18,222-x,item.done&&n.view==View::Tasks?muted:ink);
        small(d,n.view==View::Agenda?item.time:item.meta,x,y+29);
        d.drawFastHLine(16,y+43,208,line);
      }
      pager(d,n.page,(data->count+2)/3);
    }
  }
  footer(d,n); return next;
}
Action tap(Model &m,Navigation &n,int x,int y,size_t next) {
  if(x<0 || x>=240 || y<0 || y>=320) return Action::None;
  if(y<40) {if(n.selected>=0) {n.selected=-1;n.textPage=0;n.depth=0;return Action::Redraw;}return Action::Face;}
  if(y>=276) {
    if(x>=154) return Action::Talk;
    if(n.selected>=0) {n.selected=-1;n.textPage=0;n.depth=0;return Action::Redraw;}
    return Action::Face;
  }
  if(n.view==View::Home || n.view==View::Apps)return tapRecent(&m,n,x,y);
  if(n.view==View::Card || n.selected>=0) {
    if(y>=246) { if(x<120) n.back(); else n.forward(next); return Action::Redraw; }
    return Action::None;
  }
  const auto *data=collection(m,n.view); if(!data) return Action::None;
  if(y>=244 && y<276) {
    if(x<80 && n.page) --n.page;
    else if(x>=160 && (n.page+1)*3<data->count) ++n.page;
    return Action::Redraw;
  }
  if(y>=98 && y<242) {
    const size_t item=n.page*3+(y-98)/48;
    if(item>=data->count) return Action::None;
    if(n.view==View::Tasks && x<44) toggleTask(m,item);
    else {n.selected=int(item); n.textPage=0; n.depth=0;}
    return Action::Redraw;
  }
  return Action::None;
}
}
