#include "sdkconfig.h"
#if CONFIG_BUNTY_MUSE
#include "Muse.h"
#include "MuseDisplay.h"
#include "GifCanvas.h"
#include "GifTransfer.h"
#include "MuseText.h"
#include "Shell.h"
#include "SpeechGesture.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "Ui.h"
#include "Touch.h"
#include "Power.h"
#include "Log.h"
#include "Config.h"
#include "Emotion.h"
#include "freertos/semphr.h"
extern "C" {
#include "app.h"
#include "button.h"
#include "config_store.h"
#include "led_status.h"
#include "link_pairing.h"
#include "noise_control.h"
#include "wifi_mgr.h"
#include "muse_link.h"
#include "muse_state.h"
#include "muse_chat.h"
#include "muse_chat_priv.h"
#include "BuntyMuseHooks.h"
}
#include <atomic>
#include <algorithm>

namespace {
std::atomic<led_state_t> linkState{LED_STATE_BOOT};
std::atomic<bool> appStarted{false};
std::atomic<button_cb> shortPress{nullptr};
std::atomic<bool> setupActive_{false};
struct Motion { float yaw, pitch; };
QueueHandle_t motionQueue = nullptr;
portMUX_TYPE captionLock = portMUX_INITIALIZER_UNLOCKED;
char captionText[1024] = {};
uint32_t captionAt=0;
void appTask(void *) { app_run(); vTaskDelete(nullptr); }
cJSON *result(bool ok, const char *error = nullptr, const char *code = "invalid_params") {
  cJSON *r=cJSON_CreateObject(); cJSON_AddBoolToObject(r,"ok",ok);
  if(error) {
    cJSON *detail=cJSON_AddObjectToObject(r,"error");
    cJSON_AddStringToObject(detail,"code",code); cJSON_AddStringToObject(detail,"message",error);
  }
  else cJSON_AddItemToObject(r,"payload",cJSON_CreateObject());
  return r;
}
}
namespace muse {
bool connected() { return noise_ctrl_is_connected(); }
const char *network() { static char name[33]; config_get_str("ssid",name,sizeof(name)); return name; }
void disconnect() { noise_ctrl_disconnect(); wifi_mgr_disconnect(); }
bool paired() { return config_setup_complete() && config_is_provisioned(); }
void resetPairing() {
  if (!config_clear_setup()) { ui::showError("Muse reset failed"); return; }
  ESP.restart();
}
void clearCaption() { portENTER_CRITICAL(&captionLock); captionText[0]=0; portEXIT_CRITICAL(&captionLock); }
void caption(char *out,size_t size) {
  if(!out || !size) return;
  char text[1024]; uint32_t since;
  portENTER_CRITICAL(&captionLock); strlcpy(text,captionText,sizeof(text)); since=captionAt; portEXIT_CRITICAL(&captionLock);
  out[0]=0;
  if(text[0]) muse_hatch_caption_at(text,std::min<size_t>(size_t(uint64_t(millis()-since)*14/1000),strlen(text)-1),out,size);
}
void service(ServoMotion *yaw,ServoMotion *pitch,float yawHome,float pitchHome,bool conversationActive) {
  static Motion motion={};
  static int stage=0;
  // The conversation controller owns both axes until it has homed. Keep
  // manual requests queued and release any preempted single-axis reservation.
  if(conversationActive) {
    if(stage==1) power::release(power::Load::kTurnServo);
    if(stage==3) power::release(power::Load::kTiltServo);
    if(stage==1 || stage==3 || stage==2) stage=4;
    return;
  }
  if(stage==1 && yaw && !yaw->moving()) { power::release(power::Load::kTurnServo); stage=2; }
  if(stage==3 && pitch && !pitch->moving()) { power::release(power::Load::kTiltServo); stage=0; }
  if(stage==0 && motionQueue && xQueueReceive(motionQueue,&motion,0)==pdTRUE) stage=4;
  if(stage==4) {
    if(!yaw) stage=2;
    else if(power::claim(power::Load::kTurnServo)) {
      power::markBusy(); yaw->moveTo(yawHome+motion.yaw); stage=1;
    }
  }
  if(stage==2) {
    if(!pitch) stage=0;
    else if(power::claim(power::Load::kTiltServo)) {
      power::markBusy(); pitch->moveTo(pitchHome+motion.pitch); stage=3;
    }
  }
}
}
namespace muse {
bool begin() {
  if(appStarted.exchange(true)) return true;
  muse::beginDisplay();
  motionQueue=xQueueCreate(1,sizeof(Motion));
  setupActive_=!paired();
  if(!motionQueue || xTaskCreate(appTask,"muse_link",12288,nullptr,4,nullptr)!=pdPASS) {
    if(motionQueue) { vQueueDelete(motionQueue); motionQueue=nullptr; }
    appStarted=false; return false;
  }
  return true;
}
bool setupActive() { return setupActive_; }
bool serviceSetup() {
  static led_state_t shown=LED_STATE_BOOT;
  led_state_t state=linkState;
  if(state!=shown) {
    shown=state;
    if(state==LED_STATE_PAIRING_CONFIRM_REQUIRED) ui::showConfirm("PAIR WITH MUSE?","Tap Yes to confirm");
    else if(state==LED_STATE_ERROR) ui::showError("Muse setup failed");
    else if(state==LED_STATE_BLE_ADVERTISING) ui::showPairing();
    else if(state==LED_STATE_WIFI_CONNECTING) ui::showPairingConfirmed();
  }
  const auto event=touch::poll();
  if(state==LED_STATE_PAIRING_CONFIRM_REQUIRED && event.kind==touch::Event::Kind::kTap) {
    const auto callback=shortPress.load();
    if(ui::confirmAcceptedAt(event.x,event.y) && callback) callback();
    else if(ui::confirmRejectedAt(event.x,event.y)) muse::resetPairing();
  }
  ui::servicePairingActivity();
  // Saved credentials alone do not mean BLE/TLS handoff is finished. Fresh
  // provisioning reboots once; do not allocate mic/voice DMA before that reboot.
  if(muse::paired() && muse::connected()) { setupActive_=false; return true; }
  return false;
}
}
extern "C" {
bool button_init(button_cb onShort,button_cb,button_cb) { shortPress=onShort; return true; }
void button_set_press_cb(button_press_cb) {}
bool led_status_init() { return true; }
void led_status_set_state(led_state_t state) { linkState=state; }
void led_status_set_title(const char *title) { if(title) logPrintf("[muse] connected to %s\n",title); }
void led_status_set_voice(led_voice_t) {}
void led_status_set_level(float) {}
void led_status_show_volume(int) {}
void voice_init() {}
cJSON *voice_configure_command(cJSON *) { return result(false,"Use Bunty's configured volume"); }
bool muse_link_hatch_linked() { return config_is_provisioned(); }
bool muse_wifi_connected() { return wifi_mgr_is_connected(); }
bool muse_link_req_ready() { return noise_ctrl_is_connected(); }
int64_t muse_link_req_open(const char *verb,const char *path,const char *const *headers,bool end,muse_link_req_cb cb,void *ctx) {
  return noise_ctrl_req_open(verb,path,headers,end,cb,ctx);
}
bool muse_link_req_send(int64_t id,const void *data,size_t len,bool end,int wait) { return noise_ctrl_req_send(id,data,len,end,wait); }
void muse_link_req_cancel(int64_t id) { noise_ctrl_req_cancel(id); }
void muse_state_page(bool,int *cols,int *lines) { *cols=26; *lines=2; }
muse_power_t muse_state_power() { return {-1,0,false,false}; } // No battery telemetry hardware.
void bunty_muse_register(cJSON *commands) {
  shell::registerCommands(commands);
  // Override the generic LED-board description with Bunty's actual geometry.
  cJSON *draw=cJSON_GetObjectItem(commands,"display.draw_url");
  if(draw) {
    cJSON_ReplaceItemInObject(draw,"description",cJSON_CreateString(
      "Download and display an image on Bunty's rectangular 240x320 colour LCD. "
      "Bunty downloads the URL directly; it does not require uploading the image "
      "to another service. Prefer an existing direct image/GIF URL when it meets "
      "the limits. Do not re-upload or generate a replacement for a usable URL. "
      "For text or structured information, prefer ui.show_card or display.show_text. "
      "Use a baseline (not progressive) JPEG, downscaled by 1/2, 1/4 or 1/8 to fit "
      "and centred by default. Or supply raw RGB565, high byte first, 480 bytes "
      "per row, at most 320 rows. Animated GIF87a/GIF89a also supported: "
      "canvas at most 240x320, file at most 1 MiB, at most 512 frames; "
      "GIFs zoom to fill the full screen by default, preserving proportions with centred cropping. "
      "An explicit row disables GIF zoom and places it at native size. "
      "With transparency and disposal, file loop count respected, "
      "maximum 10 fps. Convert PNG, WebP, SVG and progressive JPEG "
      "to baseline JPEG or RGB565 before sending. HTTP and HTTPS URLs supported; "
      "same-scheme redirects only. Replies after a complete, successful image is "
      "displayed. Failed downloads keep the previous screen. Tap or call "
      "display.show_animation to return to Bunty's face. Busy while speech downloads."));
    cJSON *row=cJSON_GetObjectItem(cJSON_GetObjectItem(draw,"optional"),"row");
    if(row) cJSON_ReplaceItemInObject(row,"description",cJSON_CreateString(
      "Integer -1 to 319. Default -1 centres JPEGs, zooms GIFs edge-to-edge with centred cropping; RGB565 starts at 0. Explicit row places GIFs at native size."));
  }
  if(draw) {
    cJSON *options=cJSON_GetObjectItem(draw,"optional");
    for(const char *name:{"gesture","speed"}) {
      cJSON *param=cJSON_AddObjectToObject(options,name);
      cJSON_AddStringToObject(param,"type","string");
      cJSON_AddStringToObject(param,"description",!strcmp(name,"gesture")?
        "Optional meme reaction: neutral, nod, shake, curious or happy. Starts when the image is displayed, including silent GIFs.":
        "Movement tempo: fast (default, 1300 ms), normal (1800 ms) or slow (2400 ms). Match the meme; use fast for energetic reactions.");
    }
  }
  cJSON *gifShow=cJSON_AddObjectToObject(commands,"display.show_gif");
  cJSON_AddStringToObject(gifShow,"description",
    "Fastest direct GIF display: send only data_base64, ONE call, no transfer ID, byte count, "
    "CRC32, begin/chunks/commit or preparatory display.info call. Read the real file and "
    "base64-encode it with a file/code tool; do not invent bytes. Max 6144 decoded bytes. "
    "GIF is already compressed: no ZIP/gzip. Bunty immediately decodes and zooms it edge-to-edge "
    "on the full 240x320 screen with proportions preserved and centred crop. "
    "Keep row omitted for fullscreen. Invalid files keep the previous screen. "
    "Prefer this over bunty.gif_transfer for small files. For larger GIFs prefer one "
    "display.draw_url call with an existing usable direct URL. Do not re-upload a usable URL. "
    "Source canvas max 240x320, max 512 frames, 10 fps. No device confirmation step. "
    "For memes include gesture and speed in THIS call, instead of a separate gesture call. "
    "Choose happy for funny/excited, curious for confused/surprised, nod for agreement, shake for disapproval "
    "and neutral for stillness; match context. Use fast for quick memes. Movement starts on display, even without speech.");
  cJSON *gifData=cJSON_AddObjectToObject(cJSON_AddObjectToObject(gifShow,"required"),"data_base64");
  cJSON_AddStringToObject(gifData,"type","string");
  cJSON_AddStringToObject(gifData,"description","Base64 of the GIF file, at most 8192 characters, no data URL prefix or whitespace.");
  cJSON *gifRow=cJSON_AddObjectToObject(cJSON_AddObjectToObject(gifShow,"optional"),"row");
  cJSON_AddStringToObject(gifRow,"type","number");
  cJSON_AddStringToObject(gifRow,"description","Omit or -1 for fullscreen zoom. Integer 0 to 319 uses native size at that row.");
  for(const char *name:{"gesture","speed"}) {
    cJSON *param=cJSON_AddObjectToObject(cJSON_GetObjectItem(gifShow,"optional"),name);
    cJSON_AddStringToObject(param,"type","string");
    cJSON_AddStringToObject(param,"description",!strcmp(name,"gesture")?
      "Optional meme reaction: neutral, nod, shake, curious or happy. Starts as soon as this GIF appears.":
      "Movement tempo: fast (default, 1300 ms), normal (1800 ms) or slow (2400 ms); match the meme's energy.");
  }
  cJSON *transfer=cJSON_AddObjectToObject(commands,"bunty.gif_transfer");
  cJSON_AddStringToObject(transfer,"description",
    "Legacy chunked GIF transfer. For small files prefer display.show_gif: one call, data_base64 only, no metadata or preflight calls. "
    "Use only when you can read the actual file bytes and compute CRC32/base64 with a file/code tool; "
    "never invent file data. GIF is already compressed: send native GIF bytes, not ZIP/gzip. "
    "GIFs zoom edge-to-edge on the full screen by default, preserving proportions with centre crop. "
    "Keep row omitted or -1 for fullscreen. "
    "Use this tool only for larger files without a usable direct URL or for compatibility with existing transfers. "
    "For larger files prefer an existing usable URL and display.draw_url: a single HTTP fetch "
    "avoids many slow model/tool round trips. Use chunking only when no usable URL exists. "
    "One transfer at a time. Call begin with a unique ID, byte count and CRC32; "
    "then sequential chunk calls with byte offsets and at most 6144 decoded bytes "
    "(8192 base64 characters), awaiting received_bytes acknowledgement each time. "
    "Then commit validates and displays; incomplete or invalid files preserve the screen. "
    "Use status to resume and cancel to discard. Identical retries are safe. "
    "30-second idle timeout and five-minute total deadline. Prefer small GIFs (under 64 KiB) "
    "to reduce tool calls. Max 1 MiB, 240x320 canvas, 512 frames, 10 fps; resize/optimize first. "
    "If bytes are unavailable use display.draw_url with an existing usable URL. "
    "This tool does not grant or bypass Muse permissions.");
  cJSON *transferRequired=cJSON_AddObjectToObject(transfer,"required");
  cJSON *transferOptional=cJSON_AddObjectToObject(transfer,"optional");
  const char *names[]={"op","id","bytes","crc32","row","offset","data_base64"};
  const char *descriptions[]={
    "Operation: show (one call for <=6144 bytes), begin, chunk, commit, cancel or status.",
    "Unique transfer ID, 1 to 32 ASCII letters, digits, hyphen or underscore. Reuse for all calls in this transfer.",
    "Required on begin/show: exact GIF file size, 1 to 1048576 bytes.",
    "Required on begin/show: standard CRC32 of the complete GIF, eight hexadecimal digits (e.g. cbf43926).",
    "Optional begin/show placement: -1 (default) zooms GIF edge-to-edge with centre crop; explicit 0 to 319 uses native size at that row.",
    "Required on chunk: decoded byte offset; append at received_bytes or retry identical earlier data.",
    "Required on chunk/show: canonical base64 of at most 6144 GIF bytes, no whitespace or data URL prefix."};
  for(unsigned i=0;i<7;++i) {
    cJSON *param=cJSON_AddObjectToObject(i<2?transferRequired:transferOptional,names[i]);
    cJSON_AddStringToObject(param,"type",i==2||i==4||i==5?"number":"string");
    cJSON_AddStringToObject(param,"description",descriptions[i]);
  }
  cJSON *show=cJSON_CreateObject();
  cJSON_AddStringToObject(show,"description",
    "Display wrapped text on Bunty's 240x320 screen, up to 4096 UTF-8 bytes. "
    "Tap to page long text, then return to the face. Optional title and #RRGGBB "
    "foreground/background. Bitmap font supports ASCII; unsupported Unicode "
    "becomes '?'. For multilingual text, emoji, rich layouts or charts, render "
    "an image and use display.draw_url. Rejected while an image downloads.");
  cJSON *textRequired=cJSON_AddObjectToObject(show,"required");
  cJSON *textParam=cJSON_AddObjectToObject(textRequired,"text");
  cJSON_AddStringToObject(textParam,"type","string");
  cJSON_AddStringToObject(textParam,"description","Text to show, 1 to 4096 UTF-8 bytes, with optional newlines.");
  cJSON *textOptional=cJSON_AddObjectToObject(show,"optional");
  for(const char *name:{"title","foreground","background"}) {
    cJSON *param=cJSON_AddObjectToObject(textOptional,name);
    cJSON_AddStringToObject(param,"type","string");
    cJSON_AddStringToObject(param,"description",!strcmp(name,"title")?"Optional heading, up to 96 UTF-8 bytes.":"Optional colour as #RRGGBB.");
  }
  cJSON_AddItemToObject(commands,"display.show_text",show);
  cJSON *info=cJSON_CreateObject();
  cJSON_AddStringToObject(info,"description","Read Bunty's screen dimensions, supported image/text formats, download state, previous reset reason and free memory. No reboot or hardware changes.");
  cJSON_AddItemToObject(commands,"display.info",info);
  cJSON *gesture=cJSON_AddObjectToObject(commands,"bunty.gesture");
  cJSON_AddStringToObject(gesture,"description",
    "Choose one small head gesture for the current voice reply: neutral, nod, shake, curious or happy. "
    "Default auto starts with the next spoken reply or image, whichever comes first; "
    "if an image is already displayed, starts immediately. Set when speech, display or now to choose explicitly. "
    "For memes prefer gesture and speed in display.show_gif or display.draw_url: no extra tool call. "
    "Media uses clear held poses up to 18 degrees yaw and 15 pitch, fast 1300 ms, normal 1800 ms, slow 2400 ms. "
    "Speech remains gentle, fast 900 ms, normal 1400 ms, slow 2000 ms; device controls power/rate. "
    "Speech requests expire after 20 seconds; display requests after 90 seconds. "
    "Use neutral for stillness; keep movement markup out of spoken text.");
  cJSON *gestureParam=cJSON_AddObjectToObject(cJSON_AddObjectToObject(gesture,"required"),"gesture");
  cJSON_AddStringToObject(gestureParam,"type","string");
  cJSON_AddStringToObject(gestureParam,"description","One of: neutral, nod, shake, curious, happy.");
  cJSON *gestureOptions=cJSON_AddObjectToObject(gesture,"optional");
  for(const char *name:{"speed","when"}) {
    cJSON *param=cJSON_AddObjectToObject(gestureOptions,name);
    cJSON_AddStringToObject(param,"type","string");
    cJSON_AddStringToObject(param,"description",!strcmp(name,"speed")?
      "fast (default), normal or slow. Media: 1300/1800/2400 ms; speech: 900/1400/2000 ms.":
      "auto (default): next speech/image, or immediately on current image; speech: next spoken reply; display: next displayed image; now: immediately, including silence.");
  }
  cJSON *look=cJSON_CreateObject();
  cJSON_AddStringToObject(look,"description","Turn Bunty's head while idle (queued until the conversation has finished). For voice replies use bunty.gesture. Relative yaw and pitch degrees are each -40 to 40; zero homes the head. Returns after queuing a smooth movement.");
  cJSON *required=cJSON_CreateObject();
  for(const char *name:{"yaw","pitch"}) {
    cJSON *param=cJSON_CreateObject(); cJSON_AddStringToObject(param,"type","number");
    cJSON_AddStringToObject(param,"description","Degrees relative to home, -40 to 40.");
    cJSON_AddItemToObject(required,name,param);
  }
  cJSON_AddItemToObject(look,"required",required); cJSON_AddItemToObject(look,"optional",cJSON_CreateObject());
  cJSON_AddItemToObject(commands,"bunty.look",look);
}
bool bunty_muse_parse_image_gesture(cJSON *params,int *kind,int *speed) {
  *kind=-1;*speed=int(speechGesture::Speed::Fast);
  cJSON *gesture=cJSON_GetObjectItem(params,"gesture"),*tempo=cJSON_GetObjectItem(params,"speed");
  speechGesture::Kind selected;speechGesture::Speed pace=speechGesture::Speed::Fast;
  if(gesture) {
    if(!cJSON_IsString(gesture)||!speechGesture::parse(gesture->valuestring,selected))return false;
    *kind=int(selected);
  }
  if(tempo&&(!cJSON_IsString(tempo)||!speechGesture::parseSpeed(tempo->valuestring,pace)))return false;
  *speed=int(pace);return true;
}
void bunty_muse_image_gesture(int kind,int speed) {
  if(kind<0) {logPrintf("[motion] no reaction attached to image\n");return;}
  if(kind<0||kind>int(speechGesture::Kind::Happy)||speed<0||speed>int(speechGesture::Speed::Fast)||!muse::displayActive()||muse::imageFetching())return;
  speechGesture::clearSpeech();
  speechGesture::requestVisual(static_cast<speechGesture::Kind>(kind),millis(),static_cast<speechGesture::Speed>(speed));
}
cJSON *bunty_muse_command(const char *name,cJSON *params) {
  if(!strncmp(name,"ui.",3))return shell::command(name,params);
  if(!strcmp(name,"display.show_gif")) {
    cJSON *data=cJSON_GetObjectItem(params,"data_base64"),*row=cJSON_GetObjectItem(params,"row");
    if(!cJSON_IsString(data))return result(false,"data_base64 is required");
    if(row&&(!cJSON_IsNumber(row)||!isfinite(row->valuedouble)||row->valuedouble< -1||
       row->valuedouble>319||floor(row->valuedouble)!=row->valuedouble))
      return result(false,"row must be an integer from -1 to 319");
    int gesture=-1,speed=2;
    if(!bunty_muse_parse_image_gesture(params,&gesture,&speed))return result(false,"Invalid gesture or speed");
    const char *error=muse::showGifBase64(data->valuestring,row?int(row->valuedouble):-1);
    if(error)return result(false,error,"display_failed");
    bunty_muse_image_gesture(gesture,speed);
    cJSON *r=result(true);cJSON_AddBoolToObject(cJSON_GetObjectItem(r,"payload"),"displayed",true);
    return r;
  }
  if(!strcmp(name,"bunty.gif_transfer")) {
    cJSON *op=cJSON_GetObjectItem(params,"op"),*id=cJSON_GetObjectItem(params,"id");
    if(!cJSON_IsString(op)||!cJSON_IsString(id)||!muse::GifTransfer::validId(id->valuestring))
      return result(false,"op and valid id are required");
    auto integer=[&](const char *key,double min,double max,double &value,bool required) {
      cJSON *item=cJSON_GetObjectItem(params,key);
      if(!item)return !required;
      if(!cJSON_IsNumber(item)||!isfinite(item->valuedouble)||item->valuedouble<min||
         item->valuedouble>max||floor(item->valuedouble)!=item->valuedouble)return false;
      value=item->valuedouble;return true;
    };
    const bool inlineGif=!strcmp(op->valuestring,"show");
    const bool metadata=inlineGif||!strcmp(op->valuestring,"begin");
    double bytes=0,offset=0,row=-1;uint32_t crc=0;
    cJSON *checksum=cJSON_GetObjectItem(params,"crc32"),*data=cJSON_GetObjectItem(params,"data_base64");
    if(!integer("bytes",1,muse::GifTransfer::MaxBytes,bytes,metadata)||
       !integer("row",-1,319,row,false)||
       !integer("offset",0,muse::GifTransfer::MaxBytes,offset,!strcmp(op->valuestring,"chunk")))
      return result(false,"Invalid or missing byte count, offset or row");
    if(metadata&&(!cJSON_IsString(checksum)||!muse::GifTransfer::parseCrc(checksum->valuestring,crc)))
      return result(false,"begin/show requires crc32 as eight hexadecimal digits");
    if((inlineGif||!strcmp(op->valuestring,"chunk"))&&!cJSON_IsString(data))
      return result(false,"chunk/show requires data_base64");
    const auto state=muse::transferGif(op->valuestring,id->valuestring,size_t(bytes),crc,int(row),size_t(offset),
      cJSON_IsString(data)?data->valuestring:nullptr);
    if(state.error)return result(false,state.error,"transfer_failed");
    cJSON *r=result(true),*payload=cJSON_GetObjectItem(r,"payload");
    cJSON_AddNumberToObject(payload,"received_bytes",state.received);
    cJSON_AddNumberToObject(payload,"total_bytes",state.total);
    cJSON_AddBoolToObject(payload,"active",state.active);
    cJSON_AddBoolToObject(payload,"displayed",state.displayed);
    return r;
  }
  if(!strcmp(name,"display.info")) {
    cJSON *r=result(true),*payload=cJSON_GetObjectItem(r,"payload");
    cJSON_AddNumberToObject(payload,"width",cfg::ui::kScreenWidth);
    cJSON_AddNumberToObject(payload,"height",cfg::ui::kScreenHeight);
    cJSON_AddNumberToObject(payload,"bits_per_pixel",16);
    cJSON *formats=cJSON_AddArrayToObject(payload,"image_formats");
    cJSON_AddItemToArray(formats,cJSON_CreateString("baseline_jpeg"));
    cJSON_AddItemToArray(formats,cJSON_CreateString("rgb565_big_endian"));
    cJSON_AddItemToArray(formats,cJSON_CreateString("gif"));
    cJSON_AddNumberToObject(payload,"gif_max_bytes",muse::kGifMaxBytes);
    cJSON_AddBoolToObject(payload,"gif_direct_transfer",true);
    cJSON_AddBoolToObject(payload,"gif_fast_display",true);
    cJSON_AddNumberToObject(payload,"gif_inline_max_bytes",muse::GifTransfer::ChunkBytes);
    cJSON_AddNumberToObject(payload,"gif_chunk_bytes",muse::GifTransfer::ChunkBytes);
    cJSON_AddNumberToObject(payload,"gif_max_fps",muse::kGifMaxFps);
    cJSON_AddNumberToObject(payload,"gif_max_frames",muse::kGifMaxFrames);
    cJSON_AddNumberToObject(payload,"gif_max_width",240);
    cJSON_AddNumberToObject(payload,"gif_max_height",320);
    cJSON_AddNumberToObject(payload,"text_max_bytes",4096);
    cJSON_AddStringToObject(payload,"text_font","ASCII bitmap; Unicode via rendered image");
    cJSON_AddBoolToObject(payload,"remote_content",muse::displayActive());
    cJSON_AddBoolToObject(payload,"image_fetching",muse::imageFetching());
    cJSON_AddNumberToObject(payload,"reset_reason",esp_reset_reason());
    cJSON_AddNumberToObject(payload,"internal_free_bytes",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(payload,"psram_free_bytes",heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    return r;
  }
  if(!strcmp(name,"display.show_text")) {
    cJSON *text=cJSON_GetObjectItem(params,"text"), *title=cJSON_GetObjectItem(params,"title");
    if(!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0] || strnlen(text->valuestring,4097)>4096)
      return result(false,"text must contain 1 to 4096 UTF-8 bytes");
    if(title && (!cJSON_IsString(title) || !title->valuestring || strnlen(title->valuestring,97)>96))
      return result(false,"title must be a string of at most 96 UTF-8 bytes");
    uint16_t fg=cfg::ui::kText,bg=cfg::ui::kBackground;
    cJSON *color=cJSON_GetObjectItem(params,"foreground"), *fill=cJSON_GetObjectItem(params,"background");
    if((color && (!cJSON_IsString(color) || !muse::screenColor(color->valuestring,fg))) ||
       (fill && (!cJSON_IsString(fill) || !muse::screenColor(fill->valuestring,bg))))
      return result(false,"colours must be #RRGGBB strings");
    if(!muse::showText(text->valuestring,title?title->valuestring:"",fg,bg))
      return result(false,"display busy or unavailable","busy");
    return result(true);
  }
  if(!strcmp(name,"bunty.gesture")) {
    cJSON *value=cJSON_GetObjectItem(params,"gesture");
    speechGesture::Kind kind;
    if(!cJSON_IsString(value) || !speechGesture::parse(value->valuestring,kind))
      return result(false,"gesture must be neutral, nod, shake, curious or happy");
    auto speed=speechGesture::Speed::Fast;
    cJSON *tempo=cJSON_GetObjectItem(params,"speed"),*when=cJSON_GetObjectItem(params,"when");
    if(tempo&&(!cJSON_IsString(tempo)||!speechGesture::parseSpeed(tempo->valuestring,speed)))
      return result(false,"speed must be fast, normal or slow");
    if(when&&!cJSON_IsString(when))return result(false,"when must be auto, speech, display or now");
    const char *trigger=when?when->valuestring:"auto";
    const uint32_t now=millis();
    if(!strcmp(trigger,"display"))speechGesture::requestDisplay(kind,now,speed);
    else if(!strcmp(trigger,"now")||(!strcmp(trigger,"auto")&&muse::displayActive()&&!muse::imageFetching()))speechGesture::requestVisual(kind,now,speed);
    else if(!strcmp(trigger,"speech")||!strcmp(trigger,"auto"))speechGesture::request(kind,now,speed,strcmp(trigger,"speech")!=0);
    else return result(false,"when must be auto, speech, display or now");
    return result(true);
  }
  if(strcmp(name,"bunty.look")) return result(false,"Unknown Bunty command");
  cJSON *yaw=cJSON_GetObjectItem(params,"yaw"), *pitch=cJSON_GetObjectItem(params,"pitch");
  if(!cJSON_IsNumber(yaw)||!cJSON_IsNumber(pitch)||!isfinite(yaw->valuedouble)||!isfinite(pitch->valuedouble)||fabs(yaw->valuedouble)>40||fabs(pitch->valuedouble)>40)
    return result(false,"yaw and pitch must be numbers between -40 and 40");
  Motion motion{static_cast<float>(yaw->valuedouble),static_cast<float>(pitch->valuedouble)};
  if(!motionQueue||xQueueOverwrite(motionQueue,&motion)!=pdTRUE) return result(false,"Motion unavailable");
  return result(true);
}
void bunty_muse_reply(const char *text) {
  if(!text || !text[0])return;
  logPrintf("[muse] reply received: bytes=%u\n",unsigned(strnlen(text,1024)));
  portENTER_CRITICAL(&captionLock); strlcpy(captionText,text,sizeof(captionText)); captionAt=millis(); portEXIT_CRITICAL(&captionLock);
  // Voice owns the SDK poll and receives this on the same task.
  extern void bunty_muse_queue_tts(const char *);
  bunty_muse_queue_tts(text);
}
}
#endif
