#include "ShellView.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
class Raster : public Adafruit_GFX {
 public:
  std::vector<uint16_t> pixels=std::vector<uint16_t>(240*320);
  Raster():Adafruit_GFX(240,320) {}
  void drawPixel(int16_t x,int16_t y,uint16_t c) override {if(x>=0&&y>=0&&x<240&&y<320)pixels[y*240+x]=c;}
};
int main(int argc,char **argv) {
  if(argc!=2)return 2;
  shell::Model model;
  model.agenda.received=model.tasks.received=model.notes.received=true;
  model.agenda.count=4; model.tasks.count=4; model.notes.count=1;
  snprintf(model.agenda.title,41,"Tuesday, 6 October");snprintf(model.agenda.source,41,"WORK CALENDAR");
  const char *events[]={"Design catch-up","Lunch with Riya","Make something","Evening walk"};
  const char *times[]={"10:30 - 11:00","12:30 - 13:30","15:00 - 16:00","18:00 - 18:30"};
  const char *tasks[]={"Send the prototype","Pick up coffee","Plan the weekend","Read a chapter"};
  for(int i=0;i<4;++i) {
    snprintf(model.agenda.items[i].title,81,"%s",events[i]);snprintf(model.agenda.items[i].time,25,"%s",times[i]);
    snprintf(model.tasks.items[i].title,81,"%s",tasks[i]);snprintf(model.tasks.items[i].meta,65,"%s",i==0?"Today / work":"Personal");
  }
  model.tasks.items[2].done=true;
  snprintf(model.notes.items[0].title,81,"A thought for later");
  snprintf(model.notes.items[0].meta,65,"Saved this morning");
  snprintf(model.notes.items[0].body,385,"Make a little more room for the things that make an ordinary day feel good.\n\nCoffee. A walk. Something worth making.");
  model.card.received=true;
  snprintf(model.card.title,41,"Bengaluru");snprintf(model.card.value,25,"24 C");snprintf(model.card.subtitle,81,"A little rain later");
  snprintf(model.card.body,769,"A good morning for a walk. Take an umbrella if you head out after lunch.");snprintf(model.card.source,41,"WEATHER / FROM MUSE");
  shell::Status status;status.connected=status.clockValid=true;snprintf(status.clock,6,"09:41");snprintf(status.date,32,"Tuesday, 6 October");
  const shell::View views[]={shell::View::Home,shell::View::Apps,shell::View::Agenda,shell::View::Tasks,shell::View::Notes,shell::View::Card};
  std::vector<unsigned char> sheet(3*240*2*320*3);
  for(int i=0;i<6;++i) {
    Raster raster;shell::Navigation nav;nav.open(views[i]);if(i==4)nav.selected=0;
    if(i==0)shell::renderRecent(raster,nullptr);else shell::render(raster,model,nav,status);
    for(int y=0;y<320;++y)for(int x=0;x<240;++x) {
      const auto c=raster.pixels[y*240+x];size_t at=((i/3*320+y)*720+i%3*240+x)*3;
      sheet[at]=((c>>11)&31)*255/31;sheet[at+1]=((c>>5)&63)*255/63;sheet[at+2]=(c&31)*255/31;
    }
  }
  auto *out=fopen(argv[1],"wb");if(!out)return 3;fprintf(out,"P6\n720 640\n255\n");fwrite(sheet.data(),1,sheet.size(),out);fclose(out);
}
