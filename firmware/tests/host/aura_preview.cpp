#include "ShellView.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "VoiceAura.h"
#include "flow32/graphics/Display.h"
extern "C" void *__real_malloc(size_t size) { return std::malloc(size); }
extern "C" void *__real_calloc(size_t count,size_t size) { return std::calloc(count,size); }
void *heap_caps_malloc(size_t bytes,uint32_t) { return std::malloc(bytes); }
void *heap_caps_calloc(size_t count,size_t size,uint32_t) { return std::calloc(count,size); }
void heap_caps_free(void *p) {std::free(p);}
struct Panel : DisplayTransport {
  std::vector<uint16_t> pixels=std::vector<uint16_t>(240*320);
  bool begin(const DisplayPanel &) override {return true;}
  void end() override {}
  bool present(const uint16_t *data,int16_t x,int16_t y,int16_t w,int16_t h,int16_t stride) override {
    for(int row=0;row<h;++row)for(int col=0;col<w;++col)
      pixels[(y+row)*240+x+col]=data[row*stride+col];
    return true;
  }
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
    Panel panel;DisplayPanel spec;spec.width=240;spec.height=320;
    Display raster(spec,panel);if(!raster.begin())return 1;
    shell::Navigation nav;nav.open(views[i]);if(i==4)nav.selected=0;
    shell::render(raster,model,nav,status);
    raster.present();
    const voiceAura::State states[]={voiceAura::State::Listening,voiceAura::State::Thinking,voiceAura::State::Speaking};
    const auto aura=voiceAura::frame(states[i%3],800,70);
    raster.setAura(aura.color,aura.opacity);
    for(int y=0;y<320;++y)for(int x=0;x<240;++x) {
      const auto c=panel.pixels[y*240+x];size_t at=((i/3*320+y)*720+i%3*240+x)*3;
      sheet[at]=((c>>11)&31)*255/31;sheet[at+1]=((c>>5)&63)*255/63;sheet[at+2]=(c&31)*255/31;
    }
  }
  auto *out=fopen(argv[1],"wb");if(!out)return 3;fprintf(out,"P6\n720 640\n255\n");fwrite(sheet.data(),1,sheet.size(),out);fclose(out);
}
