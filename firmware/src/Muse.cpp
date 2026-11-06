#include "sdkconfig.h"
#if CONFIG_BUNTY_MUSE
#include "Muse.h"
#include "MuseDisplay.h"
#include "GifCanvas.h"
#include "GifTransfer.h"
#include "MuseText.h"
#include "Shell.h"
#include "SpeechGesture.h"
#include "WakeInfo.h"
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
// rate 0 keeps the servo's own calibrated glide speed.
struct Motion { float yaw, pitch, rate; };
QueueHandle_t motionQueue = nullptr;
portMUX_TYPE captionLock = portMUX_INITIALIZER_UNLOCKED;
char captionText[1024] = {};
uint32_t captionAt=0;
portMUX_TYPE progressLock = portMUX_INITIALIZER_UNLOCKED;
char progressStatus[32] = {};
int progressPercent = -1;
bool progressActive = false;
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
bool wifiConnected() { return wifi_mgr_is_connected(); }
const char *network() { static char name[33]; config_get_str("ssid",name,sizeof(name)); return name; }
void disconnect() { noise_ctrl_disconnect(); wifi_mgr_disconnect(); }
bool paired() { return config_setup_complete() && config_is_provisioned(); }
void resetPairing() {
  if (!config_clear_setup()) { ui::showError("Muse reset failed"); return; }
  ESP.restart();
}
void clearCaption() { portENTER_CRITICAL(&captionLock); captionText[0]=0; portEXIT_CRITICAL(&captionLock); }
void setCaption(const char *text) {
  if(!text || !text[0]) return;
  portENTER_CRITICAL(&captionLock); strlcpy(captionText,text,sizeof(captionText)); captionAt=millis(); portEXIT_CRITICAL(&captionLock);
}
void caption(char *out,size_t size) {
  if(!out || !size) return;
  char text[1024]; uint32_t since;
  portENTER_CRITICAL(&captionLock); strlcpy(text,captionText,sizeof(text)); since=captionAt; portEXIT_CRITICAL(&captionLock);
  out[0]=0;
  if(text[0]) {
    // For punchy short reactions (1-3 words), deliver immediately without waiting on typewriter ticks:
    if(strlen(text) <= 32) strlcpy(out, text, size);
    else muse_hatch_caption_at(text,std::min<size_t>(size_t(uint64_t(millis()-since)*14/1000),strlen(text)-1),out,size);
  }
}
void setProgress(const char *status, int percent) {
  portENTER_CRITICAL(&progressLock);
  strlcpy(progressStatus, status ? status : "", sizeof(progressStatus));
  progressPercent = percent;
  progressActive = true;
  portEXIT_CRITICAL(&progressLock);
}
void clearProgress() {
  portENTER_CRITICAL(&progressLock);
  progressStatus[0] = 0;
  progressPercent = -1;
  progressActive = false;
  portEXIT_CRITICAL(&progressLock);
}
bool getProgress(char *status, size_t size, int &percent) {
  portENTER_CRITICAL(&progressLock);
  const bool active = progressActive;
  if (active && status && size) strlcpy(status, progressStatus, size);
  percent = progressPercent;
  portEXIT_CRITICAL(&progressLock);
  return active;
}
void service(ServoMotion *yaw,ServoMotion *pitch,float yawHome,float pitchHome,bool conversationActive) {
  static Motion motion={};
  static float yawBase=0,pitchBase=0;  // calibrated speeds, restored after a speed override
  static int stage=0;
  // The conversation controller owns both axes until it has homed. Keep
  // manual requests queued and release any preempted single-axis reservation.
  if(conversationActive) {
    if(stage==1) {power::release(power::Load::kTurnServo);if(yawBase>0&&yaw)yaw->setSpeed(yawBase);}
    if(stage==3) {power::release(power::Load::kTiltServo);if(pitchBase>0&&pitch)pitch->setSpeed(pitchBase);}
    if(stage==1 || stage==3 || stage==2) stage=4;
    return;
  }
  if(stage==1 && yaw && !yaw->moving()) { power::release(power::Load::kTurnServo); if(yawBase>0)yaw->setSpeed(yawBase); stage=2; }
  if(stage==3 && pitch && !pitch->moving()) { power::release(power::Load::kTiltServo); if(pitchBase>0)pitch->setSpeed(pitchBase); stage=0; }
  if(stage==0 && motionQueue && xQueueReceive(motionQueue,&motion,0)==pdTRUE) stage=4;
  if(stage==4) {
    if(!yaw) stage=2;
    else if(power::claim(power::Load::kTurnServo)) {
      power::markBusy(); yawBase=motion.rate>0?yaw->speed():0; if(motion.rate>0)yaw->setSpeed(power::servoSpeed(motion.rate)); yaw->moveTo(yawHome+motion.yaw); stage=1;
    }
  }
  if(stage==2) {
    if(!pitch) stage=0;
    else if(power::claim(power::Load::kTiltServo)) {
      power::markBusy(); pitchBase=motion.rate>0?pitch->speed():0; if(motion.rate>0)pitch->setSpeed(power::servoSpeed(motion.rate)); pitch->moveTo(pitchHome+motion.pitch); stage=3;
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
bool lookAt(float yawDegrees,float pitchDegrees,float rate) {
  if(!motionQueue||!isfinite(yawDegrees)||!isfinite(pitchDegrees))return false;
  Motion motion{yawDegrees,pitchDegrees,rate};
  return xQueueOverwrite(motionQueue,&motion)==pdTRUE;
}
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
extern "C++" {
// Gesture angles stay inside the small-arc power reservation Dance enforces.
constexpr const char *kAngleDescription[]={
  "Optional peak yaw in degrees from home, -18 to 18 (positive is right, as in bunty.look). Replaces the gesture's own yaw swing; with gesture neutral it looks to this angle, holds, then returns.",
  "Optional peak pitch in degrees from home, -15 to 15 (positive is up, as in bunty.look). Replaces the gesture's own pitch swing; with gesture neutral it looks to this angle, holds, then returns."};
void addAngleParams(cJSON *options) {
  const char *names[]={"yaw","pitch"};
  for(int i=0;i<2;++i) {
    cJSON *param=cJSON_AddObjectToObject(options,names[i]);
    cJSON_AddStringToObject(param,"type","number");
    cJSON_AddStringToObject(param,"description",kAngleDescription[i]);
  }
}
// Reads optional yaw/pitch. Out-of-range values are refused, never clipped, so
// Muse learns the limit instead of getting a smaller motion than it asked for.
bool parseAngles(cJSON *params,speechGesture::Angles &out,const char *&error) {
  out={};
  const struct {const char *key;float limit;bool yaw;} axes[]={{"yaw",cfg::dance::kMaxYawDegrees,true},{"pitch",cfg::dance::kMaxTiltDegrees,false}};
  for(const auto &axis:axes) {
    cJSON *item=cJSON_GetObjectItem(params,axis.key);
    if(!item)continue;
    if(!cJSON_IsNumber(item)||!isfinite(item->valuedouble)||fabs(item->valuedouble)>axis.limit) {
      error=axis.yaw?"yaw must be a number from -18 to 18 degrees":"pitch must be a number from -15 to 15 degrees";
      return false;
    }
    const int8_t degrees=int8_t(lround(item->valuedouble));
    if(axis.yaw){out.yaw=degrees;out.hasYaw=true;}
    else {out.pitch=degrees;out.hasPitch=true;}
  }
  return true;
}
}
void bunty_muse_register(cJSON *commands) {
  if(cfg::ui::kTextContent)shell::registerCommands(commands);
  // Override the generic LED-board description with Bunty's actual geometry.
  cJSON *draw=cJSON_GetObjectItem(commands,"display.draw_url");
  if(draw) {
    cJSON_ReplaceItemInObject(draw,"description",cJSON_CreateString(
      "Download and display an image or animated GIF on Bunty's rectangular 240x320 colour LCD. "
      "Bunty downloads the URL directly; it does not require uploading the image "
      "to another service. Prefer an existing direct image/GIF URL when it meets "
      "the limits. If a user request is out of scope, unsupported, or impossible, "
      "fetch and display a funny cat GIF or funny meme animation here with an amused "
      "servo gesture (shake, puppytilt, or sassy shimmy). "
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
      "display.show_animation to return to Bunty's face."));
    cJSON *row=cJSON_GetObjectItem(cJSON_GetObjectItem(draw,"optional"),"row");
    if(row) cJSON_ReplaceItemInObject(row,"description",cJSON_CreateString(
      "Integer -1 to 319. Default -1 centres JPEGs, zooms GIFs edge-to-edge with centred cropping; RGB565 starts at 0. Explicit row places GIFs at native size."));
  }
  if(draw) {
    cJSON *options=cJSON_GetObjectItem(draw,"optional");
    addAngleParams(options);
    for(const char *name:{"gesture","speed"}) {
      cJSON *param=cJSON_AddObjectToObject(options,name);
      cJSON_AddStringToObject(param,"type","string");
      cJSON_AddStringToObject(param,"description",!strcmp(name,"gesture")?
        "Optional meme reaction: neutral, nod, shake, curious or happy. Starts when the image is displayed, including silent GIFs. yaw/pitch angles, if given, set its swing.":
        "Movement tempo: fast (default, 1300 ms), normal (1800 ms) or slow (2400 ms). Match the meme; use fast for energetic reactions.");
    }
  }
  cJSON *gifShow=cJSON_AddObjectToObject(commands,"display.show_gif");
  cJSON_AddStringToObject(gifShow,"description",
    "Fastest direct GIF display: send only data_base64, ONE call, no transfer ID, byte count, "
    "CRC32, begin/chunks/commit or preparatory display.info call. Max 6144 decoded bytes. "
    "Bunty immediately decodes and zooms it edge-to-edge on the full 240x320 screen with proportions preserved. "
    "Use for memes, funny cat GIFs when tasks are out of scope, or reaction animations. "
    "For memes/reactions include gesture and speed in THIS call (happy, curious, nod, shake, puppytilt, shimmy, bashful). "
    "Movement starts on display immediately.");
  cJSON *gifData=cJSON_AddObjectToObject(cJSON_AddObjectToObject(gifShow,"required"),"data_base64");
  cJSON_AddStringToObject(gifData,"type","string");
  cJSON_AddStringToObject(gifData,"description","Base64 of the GIF file, at most 8192 characters, no data URL prefix or whitespace.");
  cJSON *gifRow=cJSON_AddObjectToObject(cJSON_AddObjectToObject(gifShow,"optional"),"row");
  cJSON_AddStringToObject(gifRow,"type","number");
  cJSON_AddStringToObject(gifRow,"description","Omit or -1 for fullscreen zoom. Integer 0 to 319 uses native size at that row.");
  addAngleParams(cJSON_GetObjectItem(gifShow,"optional"));
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
  if(cfg::ui::kTextContent)cJSON_AddItemToObject(commands,"display.show_text",show);
  else cJSON_Delete(show);
  cJSON *info=cJSON_CreateObject();
  cJSON_AddStringToObject(info,"description","Read Bunty's screen dimensions, supported image/text formats, download state, previous reset reason and free memory. No reboot or hardware changes.");
  cJSON_AddItemToObject(commands,"display.info",info);
  cJSON *lastWake=cJSON_CreateObject();
  cJSON_AddStringToObject(lastWake,"description",
    "Where the most recent \"Hey Buddy\" came from, measured by Bunty's two microphones. "
    "Returns heard (false until the first wake), age_ms since it, direction (left, right, ahead or unknown, "
    "from Bunty's point of view), bearing_degrees (positive is Bunty's right, only when the direction is trusted; "
    "roughly +/-10 degrees, front and back cannot be told apart), confidence 0-1, whether Bunty turned toward the voice, "
    "look_yaw_degrees (Bunty's head offset from home in bunty.look's yaw units, so bunty.look can return to it), "
    "and whether a conversation was opened. Read-only; moves nothing. Call it when where the user is matters, "
    "for example to look back at them after glancing away.");
  cJSON_AddItemToObject(commands,"bunty.last_wake",lastWake);
  cJSON *gesture=cJSON_AddObjectToObject(commands,"bunty.gesture");
  cJSON_AddStringToObject(gesture,"description",
    "One head gesture: neutral, nod, shake, curious or happy. Not needed for spoken replies, whose "
    "expression tags already move the head with each sentence; use this for silence and images. "
    "Default auto starts with the next spoken reply or image, whichever comes first; "
    "if an image is already displayed, starts immediately. Set when speech, display or now to choose explicitly. "
    "For memes prefer gesture and speed in display.show_gif or display.draw_url: no extra tool call. "
    "Media uses clear held poses up to 18 degrees yaw and 15 pitch, fast 1300 ms, normal 1800 ms, slow 2400 ms. "
    "Speech remains gentle, fast 900 ms, normal 1400 ms, slow 2000 ms; device controls power/rate. "
    "Speech requests expire after 20 seconds; display requests after 90 seconds. "
    "Use neutral for stillness; keep movement markup out of spoken text. "
    "Optional yaw/pitch angles set how far the head swings, still timed by gesture and speed; neutral with an angle looks there, holds, then returns.");
  cJSON *gestureParam=cJSON_AddObjectToObject(cJSON_AddObjectToObject(gesture,"required"),"gesture");
  cJSON_AddStringToObject(gestureParam,"type","string");
  cJSON_AddStringToObject(gestureParam,"description","One of: neutral, nod, shake, curious, happy.");
  cJSON *gestureOptions=cJSON_AddObjectToObject(gesture,"optional");
  addAngleParams(gestureOptions);
  for(const char *name:{"speed","when"}) {
    cJSON *param=cJSON_AddObjectToObject(gestureOptions,name);
    cJSON_AddStringToObject(param,"type","string");
    cJSON_AddStringToObject(param,"description",!strcmp(name,"speed")?
      "fast (default), normal or slow. Media: 1300/1800/2400 ms; speech: 900/1400/2000 ms.":
      "auto (default): next speech/image, or immediately on current image; speech: next spoken reply; display: next displayed image; now: immediately, including silence.");
  }
  cJSON *look=cJSON_CreateObject();
  cJSON_AddStringToObject(look,"description","Turn Bunty's head while idle (queued until the conversation has finished). For voice replies use bunty.gesture. Relative yaw and pitch degrees are each -40 to 40; zero homes the head. Optional speed: slow (35 deg/s), normal (50) or fast (60); default is the servo's calibrated glide. Returns after queuing a smooth movement.");
  cJSON *required=cJSON_CreateObject();
  for(const char *name:{"yaw","pitch"}) {
    cJSON *param=cJSON_CreateObject(); cJSON_AddStringToObject(param,"type","number");
    cJSON_AddStringToObject(param,"description","Degrees relative to home, -40 to 40.");
    cJSON_AddItemToObject(required,name,param);
  }
  cJSON *lookOptional=cJSON_CreateObject();
  cJSON *lookSpeed=cJSON_AddObjectToObject(lookOptional,"speed");
  cJSON_AddStringToObject(lookSpeed,"type","string");
  cJSON_AddStringToObject(lookSpeed,"description","slow, normal or fast glide rate for this move.");
  cJSON_AddItemToObject(look,"required",required); cJSON_AddItemToObject(look,"optional",lookOptional);
  cJSON_AddItemToObject(commands,"bunty.look",look);

  cJSON *react=cJSON_AddObjectToObject(commands,"bunty.react");
  cJSON_AddStringToObject(react,"description",
    "Expressive silent pantomime reaction using Bunty's face, servos, and screen without cluttering the display. "
    "Provide emotion (face eyes: happy, curious, sassy, soft, sad, excited, angry, neutral), "
    "gesture (servos: nod, shake, puppytilt, shimmy, bashful, happy, curious, neutral), "
    "speed (fast, normal, slow), text (short 1-to-3 word punchy message, e.g. 'ALL DONE', 'ON IT', 'LOOK UP', 'YES!'), "
    "and optional emoji ('🎉', '❤️', '👍', '👀', '⚡', '🤖', '🔥', '✨'). "
    "Optional yaw/pitch angles set how far the head swings (replacing the gesture's own swing, timed by speed); "
    "angles alone look there, hold, then return.");
  cJSON *reactOptions=cJSON_AddObjectToObject(react,"optional");
  for(const char *paramName:{"emotion","gesture","speed","text","emoji"}) {
    cJSON *param=cJSON_AddObjectToObject(reactOptions,paramName);
    cJSON_AddStringToObject(param,"type","string");
  }
  addAngleParams(reactOptions);

  cJSON *progress=cJSON_AddObjectToObject(commands,"bunty.progress");
  cJSON_AddStringToObject(progress,"description",
    "Show live progress on Bunty's screen during long-running background tasks. "
    "Display a clean status message (e.g. 'SEARCHING...', 'RUNNING...', 'UPDATING') and optional percent 0-100.");
  cJSON *progRequired=cJSON_AddObjectToObject(progress,"required");
  cJSON *progStatus=cJSON_AddObjectToObject(progRequired,"status");
  cJSON_AddStringToObject(progStatus,"type","string");
  cJSON_AddStringToObject(progStatus,"description","Short status text, 1-24 UTF-8 bytes.");
  cJSON *progOptional=cJSON_AddObjectToObject(progress,"optional");
  cJSON *progPercent=cJSON_AddObjectToObject(progOptional,"percent");
  cJSON_AddStringToObject(progPercent,"type","number");
  cJSON_AddStringToObject(progPercent,"description","Optional percentage from 0 to 100, or -1 for indeterminate progress.");
  cJSON *progDone=cJSON_AddObjectToObject(progOptional,"done");
  cJSON_AddStringToObject(progDone,"type","boolean");
  cJSON_AddStringToObject(progDone,"description","True when task finishes to clear progress indicator.");
}
// The Muse SDK only carries these two ints from request to display, so the
// optional angles ride in the upper bits of `speed`: bits 0-1 tempo, 8-15 yaw,
// 16-23 pitch (signed degrees), 24-25 which of the two were given.
bool bunty_muse_parse_image_gesture(cJSON *params,int *kind,int *speed) {
  *kind=-1;*speed=int(speechGesture::Speed::Fast);
  cJSON *gesture=cJSON_GetObjectItem(params,"gesture"),*tempo=cJSON_GetObjectItem(params,"speed");
  speechGesture::Kind selected;speechGesture::Speed pace=speechGesture::Speed::Fast;
  if(gesture) {
    if(!cJSON_IsString(gesture)||!speechGesture::parse(gesture->valuestring,selected))return false;
    *kind=int(selected);
  }
  if(tempo&&(!cJSON_IsString(tempo)||!speechGesture::parseSpeed(tempo->valuestring,pace)))return false;
  speechGesture::Angles angles;const char *angleError="";
  if(!parseAngles(params,angles,angleError))return false;
  if(angles.any()&&*kind<0)*kind=int(speechGesture::Kind::Neutral);
  *speed=int(pace)|int(angles.pack()<<8);return true;
}
void bunty_muse_image_gesture(int kind,int packedSpeed) {
  if(kind<0) {logPrintf("[motion] no reaction attached to image\n");return;}
  const int speed=packedSpeed&3;
  const auto angles=speechGesture::Angles::unpack(uint32_t(packedSpeed)>>8);
  if(kind>int(speechGesture::Kind::Bashful)||speed>int(speechGesture::Speed::Fast)||!muse::displayActive()||muse::imageFetching())return;
  speechGesture::clearSpeech();
  speechGesture::requestVisual(static_cast<speechGesture::Kind>(kind),millis(),static_cast<speechGesture::Speed>(speed),angles);
}
cJSON *bunty_muse_command(const char *name,cJSON *params) {
  if(!cfg::ui::kTextContent&&(!strncmp(name,"ui.",3)||!strcmp(name,"display.show_text")))
    return result(false,"Bunty shows no text. Say it aloud and add a picture tag.","unsupported");
  if(!strncmp(name,"ui.",3))return shell::command(name,params);
  if(!strcmp(name,"display.show_gif")) {
    cJSON *data=cJSON_GetObjectItem(params,"data_base64"),*row=cJSON_GetObjectItem(params,"row");
    if(!cJSON_IsString(data))return result(false,"data_base64 is required");
    if(row&&(!cJSON_IsNumber(row)||!isfinite(row->valuedouble)||row->valuedouble< -1||
       row->valuedouble>319||floor(row->valuedouble)!=row->valuedouble))
      return result(false,"row must be an integer from -1 to 319");
    int gesture=-1,speed=2;
    if(!bunty_muse_parse_image_gesture(params,&gesture,&speed))return result(false,"Invalid gesture, speed, yaw (-18 to 18) or pitch (-15 to 15)");
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
      return result(false,"gesture must be neutral, nod, shake, curious, happy, puppytilt, shimmy or bashful");
    auto speed=speechGesture::Speed::Fast;
    cJSON *tempo=cJSON_GetObjectItem(params,"speed"),*when=cJSON_GetObjectItem(params,"when");
    if(tempo&&(!cJSON_IsString(tempo)||!speechGesture::parseSpeed(tempo->valuestring,speed)))
      return result(false,"speed must be fast, normal or slow");
    if(when&&!cJSON_IsString(when))return result(false,"when must be auto, speech, display or now");
    speechGesture::Angles angles;const char *angleError="";
    if(!parseAngles(params,angles,angleError))return result(false,angleError);
    const char *trigger=when?when->valuestring:"auto";
    const uint32_t now=millis();
    if(!strcmp(trigger,"display"))speechGesture::requestDisplay(kind,now,speed,angles);
    else if(!strcmp(trigger,"now")||(!strcmp(trigger,"auto")&&muse::displayActive()&&!muse::imageFetching()))speechGesture::requestVisual(kind,now,speed,angles);
    else if(!strcmp(trigger,"speech")||!strcmp(trigger,"auto"))speechGesture::request(kind,now,speed,strcmp(trigger,"speech")!=0,angles);
    else return result(false,"when must be auto, speech, display or now");
    return result(true);
  }
  if(!strcmp(name,"bunty.react")) {
    cJSON *em=cJSON_GetObjectItem(params,"emotion");
    cJSON *gest=cJSON_GetObjectItem(params,"gesture");
    cJSON *tempo=cJSON_GetObjectItem(params,"speed");
    cJSON *txt=cJSON_GetObjectItem(params,"text");
    cJSON *emo=cJSON_GetObjectItem(params,"emoji");

    Emotion emotion=Emotion::Neutral;
    bool hasEmotion=false;
    if(em&&cJSON_IsString(em)&&em->valuestring[0]) {
      emotion=parseEmotion(em->valuestring);
      hasEmotion=true;
    }

    speechGesture::Kind gesture=speechGesture::Kind::Neutral;
    bool hasGesture=false;
    if(gest&&cJSON_IsString(gest)&&gest->valuestring[0]) {
      if(!speechGesture::parse(gest->valuestring,gesture))
        return result(false,"Invalid gesture. Choose: neutral, nod, shake, curious, happy, puppytilt, shimmy, bashful");
      hasGesture=true;
    }

    speechGesture::Speed speed=speechGesture::Speed::Fast;
    if(tempo&&cJSON_IsString(tempo)&&tempo->valuestring[0]) {
      if(!speechGesture::parseSpeed(tempo->valuestring,speed))
        return result(false,"speed must be fast, normal or slow");
    }

    speechGesture::Angles angles;const char *angleError="";
    if(!parseAngles(params,angles,angleError))return result(false,angleError);
    if(angles.any()&&!hasGesture&&!hasEmotion) {hasGesture=true;gesture=speechGesture::Kind::Neutral;}

    char replyBuf[64]={};
    if(txt&&cJSON_IsString(txt)) {
      strlcpy(replyBuf,txt->valuestring,sizeof(replyBuf));
    }
    if(emo&&cJSON_IsString(emo)&&emo->valuestring[0]) {
      if(replyBuf[0]) strlcat(replyBuf," ",sizeof(replyBuf));
      strlcat(replyBuf,emo->valuestring,sizeof(replyBuf));
    }

    if(!hasGesture && hasEmotion) {
      switch(emotion) {
        case Emotion::Happy:
        case Emotion::Excited: gesture=speechGesture::Kind::Happy; break;
        case Emotion::Curious: gesture=speechGesture::Kind::PuppyTilt; break;
        case Emotion::Sassy: gesture=speechGesture::Kind::Shimmy; break;
        case Emotion::Soft: gesture=speechGesture::Kind::Bashful; break;
        case Emotion::Sad:
        case Emotion::Angry: gesture=speechGesture::Kind::Shake; break;
        default: gesture=speechGesture::Kind::Nod; break;
      }
      hasGesture=true;
    }

    if(hasGesture) {
      speechGesture::clearSpeech();
      speechGesture::requestVisual(gesture,millis(),speed,angles);
    }

    char turnText[128]={};
    if(hasEmotion) snprintf(turnText,sizeof(turnText),"[%s] %s",emotionName(emotion),replyBuf);
    else strlcpy(turnText,replyBuf,sizeof(turnText));
    extern void bunty_muse_queue_tts(const char *);
    bunty_muse_queue_tts(turnText[0]?turnText:"[happy]");
    return result(true);
  }
  if(!strcmp(name,"bunty.progress")) {
    cJSON *done=cJSON_GetObjectItem(params,"done");
    if(done && cJSON_IsTrue(done)) {
      muse::clearProgress();
      extern void bunty_muse_agent_busy(bool);
      bunty_muse_agent_busy(false);
      return result(true);
    }
    cJSON *status=cJSON_GetObjectItem(params,"status");
    if(!cJSON_IsString(status)||!status->valuestring[0])
      return result(false,"status is required and must be 1 to 24 characters");
    cJSON *pct=cJSON_GetObjectItem(params,"percent");
    int percent=-1;
    if(pct&&cJSON_IsNumber(pct)) {
      percent=int(pct->valuedouble);
      if(percent<0)percent=-1;
      if(percent>100)percent=100;
    }
    muse::setProgress(status->valuestring,percent);
    extern void bunty_muse_agent_busy(bool);
    bunty_muse_agent_busy(true);
    return result(true);
  }
  if(!strcmp(name,"bunty.last_wake")) {
    const auto wake=wakeInfo::last();
    cJSON *r=result(true),*payload=cJSON_GetObjectItem(r,"payload");
    cJSON_AddBoolToObject(payload,"heard",wake.sequence!=0);
    if(!wake.sequence)return r;
    cJSON_AddNumberToObject(payload,"sequence",wake.sequence);
    cJSON_AddNumberToObject(payload,"age_ms",double(millis()-wake.atMs));
    cJSON_AddStringToObject(payload,"direction",wakeInfo::direction(wake,cfg::wake::kTurnDeadbandDegrees));
    if(wake.bearingValid) cJSON_AddNumberToObject(payload,"bearing_degrees",std::round(wake.bearingRight));
    cJSON_AddNumberToObject(payload,"confidence",std::round(wake.confidence*100.0)/100.0);
    cJSON_AddBoolToObject(payload,"turned_toward_voice",wake.turned);
    cJSON_AddNumberToObject(payload,"look_yaw_degrees",std::round(wake.lookYaw));
    cJSON_AddBoolToObject(payload,"conversation_started",wake.conversationStarted);
    return r;
  }
  if(strcmp(name,"bunty.look")) return result(false,"Unknown Bunty command");
  cJSON *yaw=cJSON_GetObjectItem(params,"yaw"), *pitch=cJSON_GetObjectItem(params,"pitch");
  if(!cJSON_IsNumber(yaw)||!cJSON_IsNumber(pitch)||!isfinite(yaw->valuedouble)||!isfinite(pitch->valuedouble)||fabs(yaw->valuedouble)>40||fabs(pitch->valuedouble)>40)
    return result(false,"yaw and pitch must be numbers between -40 and 40");
  float rate=0;
  if(cJSON *tempo=cJSON_GetObjectItem(params,"speed")) {
    speechGesture::Speed pace;
    if(!cJSON_IsString(tempo)||!speechGesture::parseSpeed(tempo->valuestring,pace))return result(false,"speed must be fast, normal or slow");
    rate=speechGesture::rate(pace);
  }
  Motion motion{static_cast<float>(yaw->valuedouble),static_cast<float>(pitch->valuedouble),rate};
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
