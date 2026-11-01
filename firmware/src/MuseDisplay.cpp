#include "MuseDisplay.h"
#include "MuseScreenBuffer.h"
#include "GifCanvas.h"
#include "GifDownload.h"
#include "GifTransfer.h"
#include "SpeechGesture.h"
#include "BuntyMuseHooks.h"
#include "MuseText.h"
#include "Shell.h"
#include "Ui.h"
#include "Fonts.h"
#include "Config.h"
#include "Memory.h"
#include "Log.h"
#include "flow32/graphics/Display.h"
#include <atomic>

namespace {
muse::ScreenBuffer image;
std::atomic<bool> active{false},fetching{false},speechPicture{false};
bool previousActive=false,textVisible=false;
enum class NetworkOwner { None, Tts, Image };
std::atomic<NetworkOwner> networkOwner{NetworkOwner::None};
bool claimNetwork(NetworkOwner owner) {
  auto expected=NetworkOwner::None;
  return networkOwner.compare_exchange_strong(expected,owner);
}
void releaseNetwork(NetworkOwner owner) {
  networkOwner.compare_exchange_strong(owner,NetworkOwner::None);
}
struct GifStorage {
  muse::GifCanvas player;
  uint8_t *data=nullptr;uint16_t *canvas=nullptr,*restore=nullptr;
  ~GifStorage(){heap_caps_free(data);heap_caps_free(canvas);heap_caps_free(restore);}
};
GifStorage *gif=nullptr,*pendingGif=nullptr;
TaskHandle_t gifTask=nullptr;
muse::GifTransfer transfer;
char completedTransfer[33]={};
size_t completedBytes=0;
uint32_t completedAt=0,completedCrc=0;
int completedRow=-1;
void clearTransfer() { heap_caps_free(transfer.release()); }
void destroyGif(GifStorage *&value) {
  if(value){value->~GifStorage();heap_caps_free(value);value=nullptr;}
}
void drawGif() {
  uint16_t line[240];
  for(int y=0;y<320;++y) {
    gif->player.screenRow(y,line);
    ui::display().writeRow(0,y,line,240);
  }
  ui::display().present();gif->player.displayed(millis());
}
void gifWorker(void *) {
  for(;;) {
    bool playing=false;
    {
      ui::Guard guard;
      playing=gif && gif->player.running();
      if(playing && !fetching && active && gif->player.due(millis())) {
        if(gif->player.next(millis()))drawGif();
        else if(!gif->player.running()) {
          logPrintf("[muse] GIF %s\n",gif->player.failed()?"decode failed":"complete");
          destroyGif(gif);playing=false;
        }
      }
    }
    if(playing)vTaskDelay(pdMS_TO_TICKS(20));
    else ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  }
}
const char *prepareGif(GifStorage *storage,size_t bytes,int row) {
  unsigned frames=0;
  const char *error=nullptr;
  if(!muse::validateGif(storage->data,bytes,frames))
    error="Invalid GIF structure, dimensions or frame bounds";
  if(!error) {
    storage->canvas=static_cast<uint16_t *>(memory::external(240*320*2));
    storage->restore=static_cast<uint16_t *>(memory::external(240*320*2));
    if(!storage->canvas||!storage->restore)error="GIF memory unavailable";
  }
  if(!error && row>=0 && row+int(storage->data[8]|unsigned(storage->data[9])<<8)>320)
    error="GIF row placement exceeds screen; use row -1";
  if(!error && !storage->player.open(storage->data,bytes,storage->canvas,storage->restore,row,cfg::ui::kBackground))
    error="GIF decoder could not open file";
  if(!error && !storage->player.next(millis()))error="GIF first frame decode failed";
  logPrintf("[muse] GIF candidate: bytes=%u frames=%u result=%s\n",unsigned(bytes),frames,error?error:"ready");
  return error;
}
bool ensureGifWorker() {
  return gifTask || xTaskCreate(gifWorker,"muse-gif",6144,nullptr,2,&gifTask)==pdPASS;
}
bool imagesAvailable=false;
char text[4097]={},title[97]={};
size_t pageAt=0,nextPage=0;
uint16_t foreground=cfg::ui::kText,background=cfg::ui::kBackground;
void drawTextPage() {
  auto &panel=ui::display(); panel.clear(background);
  int top=12;
  if(title[0]) {
    ui::drawText(BuntyFonts::kBody,ui::fitText(BuntyFonts::kBody,title,216),12,28,foreground);
    panel.drawFastHLine(12,40,216,foreground); top=54;
  }
  const size_t lines=(280-top)/21;
  nextPage=muse::textPage(text,pageAt,216,lines,
    [](const char *line) { return ui::textWidth(BuntyFonts::kBody,line); },
    [&](size_t row,const char *line) { ui::drawText(BuntyFonts::kBody,line,12,top+16+int(row)*21,foreground); });
  ui::drawCentered(BuntyFonts::kBody,text[nextPage]?"Tap for next page":"Tap to return",294,foreground);
  panel.present();
}
}
namespace muse {
bool beginDisplay() {
  auto *pixels=static_cast<uint16_t *>(memory::external(size_t(cfg::ui::kScreenWidth)*cfg::ui::kScreenHeight*2));
  if(!pixels) { Serial.println("[muse] image staging buffer unavailable"); return false; }
  ui::Guard guard; image.bind(pixels,cfg::ui::kScreenWidth,cfg::ui::kScreenHeight); imagesAvailable=true;
  return true;
}
void serviceDisplay() {
  ui::Guard guard;
  if(transfer.expired(millis())) {clearTransfer();logPrintf("[muse] GIF transfer expired\n");}
}
GifTransferReply transferGif(const char *op,const char *id,size_t bytes,uint32_t crc,int row,size_t offset,const char *data) {
  ui::Guard guard;
  const uint32_t now=millis();
  if(transfer.expired(now))clearTransfer();
  if(completedTransfer[0]&&uint32_t(now-completedAt)>=GifTransfer::TotalMs)completedTransfer[0]=0;
  auto reply=[&](const char *error) {return GifTransferReply{error,transfer.received(),transfer.size(),transfer.active(),false};};
  if(!op||!GifTransfer::validId(id))return reply("Invalid operation or transfer ID");
  if(!strcmp(id,completedTransfer)) {
    if(!strcmp(op,"commit")||!strcmp(op,"status")||(!strcmp(op,"show")&&bytes==completedBytes&&crc==completedCrc&&row==completedRow))return {nullptr,completedBytes,completedBytes,false,true};
    return reply("Transfer ID already completed; use a new ID");
  }
  if(!strcmp(op,"show")) {
    if(!bytes||bytes>GifTransfer::ChunkBytes)return reply("Single-call GIF exceeds 6144 bytes; use URL or chunks");
    if(transfer.active())return reply("Another GIF transfer is active");
    auto state=transferGif("begin",id,bytes,crc,row,0,nullptr);
    if(state.error)return state;
    const char *error=transfer.append(id,0,data,now);
    if(!error)error=transfer.ready(id,now);
    if(error) {clearTransfer();return reply(error);}
    return transferGif("commit",id,bytes,crc,row,0,nullptr);
  }
  if(!strcmp(op,"begin")) {
    if(!bytes||bytes>GifTransfer::MaxBytes||row< -1||row>319)return reply("Invalid GIF size or row");
    if(transfer.active())return reply(transfer.same(id,bytes,crc,row)?nullptr:"Another GIF transfer is active");
    if(fetching||!imagesAvailable)return reply("Display busy or unavailable");
    // Include canvases/decoder and reserve 1 MiB for voice/content work.
    if(heap_caps_get_free_size(memory::kExternal)<bytes+1024*1024+2*240*320*2+sizeof(GifStorage))
      return reply("GIF memory unavailable");
    auto *buffer=static_cast<uint8_t *>(memory::external(bytes));
    if(!buffer)return reply("GIF memory unavailable");
    completedTransfer[0]=0;
    transfer.begin(buffer,id,bytes,crc,row,now);return reply(nullptr);
  }
  if(!transfer.matches(id))return reply("Unknown transfer");
  if(!strcmp(op,"chunk"))return reply(transfer.append(id,offset,data,now));
  if(!strcmp(op,"status"))return reply(nullptr);
  if(!strcmp(op,"cancel")) {clearTransfer();return reply(nullptr);}
  if(strcmp(op,"commit"))return reply("Unknown operation");
  const char *error=transfer.ready(id,now);
  if(error) {if(!strcmp(error,"GIF checksum mismatch"))clearTransfer();return reply(error);}
  if(fetching||!ui::display().ready())return reply("Display busy or unavailable");
  auto *candidate=static_cast<GifStorage *>(memory::external(sizeof(GifStorage)));
  if(!candidate) {clearTransfer();return reply("GIF memory unavailable");}
  new(candidate) GifStorage;
  const size_t size=transfer.size();const int placement=transfer.row();
  const uint32_t checksum=transfer.checksum();
  candidate->data=transfer.release();
  error=prepareGif(candidate,size,placement);
  if(!error&&!ensureGifWorker())error="GIF worker unavailable";
  if(!error) {
    destroyGif(gif);gif=candidate;candidate=nullptr;
    active=true;speechPicture=false;textVisible=false;drawGif();speechGesture::onDisplay(millis());xTaskNotifyGive(gifTask);
    strcpy(completedTransfer,id);completedBytes=size;completedAt=now;completedCrc=checksum;completedRow=placement;
    logPrintf("[muse] direct GIF displayed: bytes=%u\n",unsigned(size));
  }
  destroyGif(candidate);
  return error?reply(error):GifTransferReply{nullptr,size,size,false,true};
}
const char *showGifBase64(const char *data,int row) {
  ui::Guard guard;
  const size_t capacity=GifTransfer::inlineCapacity(data);
  if(!capacity)return "GIF data must be base64 of at most 6144 bytes";
  if(row< -1||row>319)return "Invalid GIF row";
  if(fetching||!imagesAvailable||!ui::display().ready())return "Display busy or unavailable";
  if(heap_caps_get_free_size(memory::kExternal)<capacity+1024*1024+2*240*320*2+sizeof(GifStorage))
    return "GIF memory unavailable";
  auto *candidate=static_cast<GifStorage *>(memory::external(sizeof(GifStorage)));
  if(!candidate)return "GIF memory unavailable";
  new(candidate) GifStorage;
  candidate->data=static_cast<uint8_t *>(memory::external(capacity));
  size_t bytes=0;
  const char *error=candidate->data?GifTransfer::decodeInline(data,candidate->data,capacity,bytes):"GIF memory unavailable";
  if(!error)error=prepareGif(candidate,bytes,row);
  if(!error&&!ensureGifWorker())error="GIF worker unavailable";
  if(!error) {
    clearTransfer();completedTransfer[0]=0;
    destroyGif(gif);gif=candidate;candidate=nullptr;
    active=true;speechPicture=false;textVisible=false;drawGif();speechGesture::onDisplay(millis());xTaskNotifyGive(gifTask);
    logPrintf("[muse] fast GIF displayed: bytes=%u\n",unsigned(bytes));
  }
  destroyGif(candidate);return error;
}
bool showSpeechPicture(const uint8_t *data,size_t bytes) {
  ui::Guard guard;
  if((active&&!speechPicture)||fetching||!imagesAvailable||!ui::display().ready())return false;
  if(heap_caps_get_free_size(memory::kExternal)<bytes+1024*1024+2*240*320*2+sizeof(GifStorage))return false;
  auto *candidate=static_cast<GifStorage *>(memory::external(sizeof(GifStorage)));
  if(!candidate)return false;
  new(candidate) GifStorage;
  candidate->data=static_cast<uint8_t *>(memory::external(bytes));
  const char *error=candidate->data?nullptr:"GIF memory unavailable";
  if(!error) {memcpy(candidate->data,data,bytes);error=prepareGif(candidate,bytes,-1);}
  if(!error&&!ensureGifWorker())error="GIF worker unavailable";
  if(!error) {
    destroyGif(gif);gif=candidate;candidate=nullptr;
    active=true;speechPicture=true;textVisible=false;drawGif();xTaskNotifyGive(gifTask);
  }
  destroyGif(candidate);return !error;
}
bool speechPictureActive() { return speechPicture; }
void endSpeechPicture() {
  ui::Guard guard;
  if(speechPicture)dismissDisplay();
}
bool displayActive() { return active; }
bool imageFetching() { return fetching; }
bool claimTtsNetwork() { return claimNetwork(NetworkOwner::Tts); }
void releaseTtsNetwork() { releaseNetwork(NetworkOwner::Tts); }
void dismissDisplay() {
  ui::Guard guard; speechGesture::clearVisual(); clearTransfer(); image.cancel(); active=false; speechPicture=false; textVisible=false; destroyGif(gif);
}
void tapDisplay() {
  ui::Guard guard;
  if(!fetching && textVisible && text[nextPage]) { pageAt=nextPage; drawTextPage(); }
  else dismissDisplay();
}
bool showText(const char *value,const char *heading,uint16_t color,uint16_t fill) {
  ui::Guard guard;
  if(fetching || !ui::display().ready()) return false;
  screenAscii(value,text,sizeof(text)); screenAscii(heading,title,sizeof(title));
  destroyGif(gif);
  foreground=color; background=fill; pageAt=0; textVisible=true; active=true;
  drawTextPage(); return true;
}
}
extern "C" {
bool bunty_muse_image_begin() {
  ui::Guard guard;
  if(!claimNetwork(NetworkOwner::Image)) return false;
  if(!image.begin(cfg::ui::kBackground)) { releaseNetwork(NetworkOwner::Image); return false; }
  previousActive=active; active=true; fetching=true;
  return true;
}
bool bunty_muse_image_current() { ui::Guard guard; return image.current(); }
bool bunty_muse_image_end(bool success) {
  ui::Guard guard;
  const bool current=image.current();
  const bool commit=image.finish(success); fetching=false;
  releaseNetwork(NetworkOwner::Image);
  if(commit && pendingGif) {
    destroyGif(gif);gif=pendingGif;pendingGif=nullptr;
    drawGif();textVisible=false;active=true;speechPicture=false;speechGesture::onDisplay(millis());xTaskNotifyGive(gifTask);
  } else if(commit) {
    destroyGif(gif);
    for(int row=0;row<cfg::ui::kScreenHeight;++row)
      ui::display().writeRow(0,row,image.row(row),cfg::ui::kScreenWidth);
    ui::display().present(); textVisible=false; active=true;speechPicture=false;speechGesture::onDisplay(millis());
  } else if(current) active=previousActive;
  destroyGif(pendingGif);
  return commit;
}
const char *bunty_muse_gif_load(bunty_gif_read read,void *context,int row,int *width,int *height) {
  auto *storage=static_cast<GifStorage *>(memory::external(sizeof(GifStorage)));
  if(!storage)return "GIF memory unavailable";
  new(storage) GifStorage;
  struct Download {
    bunty_gif_read reader;void *context;size_t capacity=0;
    int read(uint8_t *data,size_t bytes){return reader(context,data,bytes);}
    uint8_t *resize(uint8_t *data,size_t bytes) {
      // Reserve 1 MiB for voice/content work, plus both canvas allocations.
      // Budget the entire new block because realloc may move the old one.
      if(bytes>capacity && heap_caps_get_free_size(memory::kExternal)<bytes+1024*1024+2*240*320*2)
        return nullptr;
      auto *next=static_cast<uint8_t *>(heap_caps_realloc(data,bytes,memory::kExternal));
      if(next)capacity=bytes;
      return next;
    }
  } download{read,context};
  size_t bytes=0;
  const char *error=muse::downloadGif(download,storage->data,bytes,muse::kGifMaxBytes);
  if(!error)error=prepareGif(storage,bytes,row);
  {
    ui::Guard guard;
    if(!error && !image.current())error="GIF dismissed";
    if(!error && !ensureGifWorker())
      error="GIF worker unavailable";
    if(!error) {
      *width=storage->player.width();*height=storage->player.height();
      destroyGif(pendingGif);pendingGif=storage;storage=nullptr;
    }
  }
  destroyGif(storage);return error;
}
bool led_status_display_info(int *width,int *height) {
  if(!imagesAvailable || !ui::display().ready()) return false;
  if(width) *width=cfg::ui::kScreenWidth;
  if(height) *height=cfg::ui::kScreenHeight;
  return true;
}
int led_status_display_bits() { return 16; }
bool led_status_draw_rect(int x,int y,int w,int h,const uint16_t *pixels) {
  ui::Guard guard;
  return image.write(x,y,w,h,reinterpret_cast<const uint8_t *>(pixels));
}
void led_status_draw_done() {} // Atomic commit is made from the fetch result.
void led_status_show_animation() { ui::Guard guard; muse::dismissDisplay(); shell::showFace(); }
}
