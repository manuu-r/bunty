#include "Log.h"
#include "Dashboard.h"
#include "DashboardCache.h"

#include <ArduinoJson.h>
#include <mdns.h>
#include <esp_http_client.h>
#include <Preferences.h>
#include <WiFi.h>

#include "Config.h"
#include "Memory.h"
#include <freertos/queue.h>
#include "Fonts.h"
#include "Ui.h"
#include "flow32/graphics/Display.h"

namespace dashboard {
void notifyGaugeOnWorker();
namespace {

using namespace cfg::dashboard;

Preferences *storage_ = nullptr;
String hostname_;
// Small protocol strings are loaded once when runtime starts. The networking
// libraries still allocate their own HTTP/mDNS objects, exclusively on core 0.
String serverId_, mdnsHost_, requestPath_, authorization_, addressHost_;
uint16_t configuredPort_ = 0;
IPAddress configuredAddress_;
bool directAddressFailed_ = false;
bool attemptedFetch_ = false;
size_t tokenLength_ = 0;
char wifiSsid_[33] = {}, wifiPassword_[65] = {};

struct AddressText {
  explicit AddressText(const IPAddress &ip) {
    snprintf(value, sizeof(value), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  }
  const char *c_str() const { return value; }
  char value[16];
};

// The single parsed snapshot every page renders from.
memory::Arena arenas_[2];
void *arenaStorage_[2] = {};
size_t arenaCapacity_ = 0;
JsonDocument snapshot_(&arenas_[0]);
uint8_t liveArena_ = 0;
char *body_ = nullptr;
size_t bodyLength_ = 0;
size_t bodyCapacity_ = 0;
bool bodyOverflow_ = false;
std::atomic<bool> resetDiscovery_{false};
QueueHandle_t fetchCommands_ = nullptr;
QueueHandle_t fetchResults_ = nullptr;
StaticQueue_t commandControl_, resultControl_;
uint8_t commandStorage_[1], resultStorage_[sizeof(bool)];
bool fetchBusy_ = false;

// IDF owns HTTP framing/chunk decoding; retain the existing bounded body.
esp_err_t receiveBody(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  const size_t length = event->data_len;
  if (length > bodyCapacity_ - bodyLength_) { bodyOverflow_ = true; return ESP_ERR_NO_MEM; }
  memcpy(body_ + bodyLength_, event->data, length);
  bodyLength_ += length;
  body_[bodyLength_] = 0;
  return ESP_OK;
}

// mDNS query results belong to the network worker and are always released.
class NativeMdns {
 public:
  ~NativeMdns() { clear(); }
  bool begin(const char *hostname) {
    if (mdns_init() != ESP_OK) return false;
    if (mdns_hostname_set(hostname) == ESP_OK) return true;
    mdns_free();
    return false;
  }
  void end() { clear(); mdns_free(); }
  IPAddress queryHost(const String &hostname, uint32_t timeout) {
    esp_ip4_addr_t ip = {};
    if (mdns_query_a(hostname.c_str(), timeout, &ip) != ESP_OK) return IPAddress();
    return IPAddress(ip.addr);
  }
  int queryService(const char *service, const char *protocol) {
    clear();
    const String type = String("_") + service;
    const String proto = String("_") + protocol;
    if (mdns_query_ptr(type.c_str(), proto.c_str(), cfg::net::kMdnsQueryTimeoutMs, 16, &results_) != ESP_OK) return 0;
    int count = 0;
    for (auto *r = results_; r; r = r->next) ++count;
    return count;
  }
  String txt(int index, const char *key) const {
    auto *r = at(index);
    if (r) for (size_t i = 0; i < r->txt_count; ++i)
      if (strcmp(r->txt[i].key, key) == 0) return r->txt[i].value ? r->txt[i].value : "";
    return "";
  }
  IPAddress IP(int index) const {
    auto *r = at(index);
    if (r) for (auto *a = r->addr; a; a = a->next)
      if (a->addr.type == ESP_IPADDR_TYPE_V4) return IPAddress(a->addr.u_addr.ip4.addr);
    return IPAddress();
  }
  uint16_t port(int index) const { auto *r = at(index); return r ? r->port : 0; }
 private:
  void clear() { if (results_) mdns_query_results_free(results_); results_ = nullptr; }
  mdns_result_t *at(int index) const {
    auto *r = results_;
    while (r && index-- > 0) r = r->next;
    return r;
  }
  mdns_result_t *results_ = nullptr;
};
NativeMdns MDNS;
bool available_ = false;
bool online_ = false;
uint8_t pageCount_ = 0;
// Identifies the stored body without keeping a 12 KB copy of it, so an
// unchanged refresh does not rewrite NVS.
uint32_t snapshotHash_ = 0;

uint32_t refreshMs_ = kDefaultRefreshMs;
uint32_t nextFetchAt_ = 0;
uint32_t nextPageAt_ = 0;
uint8_t page_ = 0;
uint16_t round_ = 0;
uint8_t connectFailures_ = 0;
bool mdnsStarted_ = false;

// Replaces the live snapshot if `body` is a well-formed document for the
// protocol and schema this firmware renders.
bool adopt(const char *body, size_t length) {
  if (length == 0 || length > kMaximumBytes) return false;

  const uint8_t spare = liveArena_ ^ 1;
  arenas_[spare].reset();
  JsonDocument candidate(&arenas_[spare]);
  if (deserializeJson(candidate, body, length)) return false;
  const char *protocol = candidate["protocol"] | "";
  if (strcmp(protocol, kProtocol) != 0 ||
      static_cast<uint16_t>(candidate["schema_version"] | 0) != 1) {
    return false;
  }

  dashboard_cache::preserveQuota(candidate, snapshot_);
  if (candidate.overflowed() || measureJson(candidate) > bodyCapacity_) return false;
  snapshot_ = std::move(candidate);
  liveArena_ = spare;
  available_ = true;
  pageCount_ = static_cast<uint8_t>(
      3 + snapshot_["quota"]["providers"].as<JsonArrayConst>().size());
  if (page_ >= pageCount_) page_ = 0;
  return true;
}

void loadCached() {
  available_ = false;
  pageCount_ = 0;
  if (!storage_ || !storage_->isKey(cfg::nvs::kDashboard)) {
    Serial.println("[dashboard] no cached snapshot stored yet");
    return;
  }
  const size_t length = storage_->getBytesLength(cfg::nvs::kDashboard);
  if (length == 0 || length > kMaximumBytes) return;

  char *body = body_;
  if (!body || length > bodyCapacity_) return;
  if (storage_->getBytes(cfg::nvs::kDashboard, body, length) == length) {
    // The parsed document copies every string it keeps, so the raw bytes are
    // released immediately rather than held for the life of the snapshot.
    if (adopt(body, length)) snapshotHash_ = dashboard_cache::fingerprint(snapshot_);
  }
  logPrintf("[dashboard] cached snapshot %u bytes, %s\n",
                static_cast<unsigned>(length),
                available_ ? "valid" : "INVALID");
}

// ------------------------------------------------------------- discovery ---

bool startMdns() {
  if (mdnsStarted_) return true;
  mdnsStarted_ = MDNS.begin(hostname_.c_str());
  if (!mdnsStarted_) Serial.println("[dashboard] mDNS start failed");
  return mdnsStarted_;
}

bool sameSubnet(const IPAddress &left, const IPAddress &right,
                const IPAddress &mask) {
  for (uint8_t i = 0; i < 4; ++i) {
    if ((left[i] & mask[i]) != (right[i] & mask[i])) return false;
  }
  return true;
}

const String &storedPath() {
  return requestPath_;
}

// mDNS PTR browsing is unreliable on the ESP32 stack and on networks that
// rate-limit or isolate multicast between clients. Pairing already stored the
// port, and the Mac derives its Bonjour hostname from the same server id
// (gauge-<first 12 of server_id>.local), so a direct A-record lookup can find
// it even when the service browse comes back empty.
bool resolveByHostname(const String &serverId, IPAddress &address,
                       uint16_t &port, String &path) {
  const uint16_t stored = configuredPort_;
  if (stored == 0 || serverId.length() < 12) return false;

  const String &host = mdnsHost_;
  const IPAddress resolved = MDNS.queryHost(host, cfg::net::kMdnsQueryTimeoutMs);
  if (resolved == IPAddress()) {
    logPrintf("[dashboard] %s.local did not resolve either\n", host.c_str());
    return false;
  }
  address = resolved;
  port = stored;
  path = storedPath();
  logPrintf("[dashboard] resolved %s.local -> %s:%u%s\n", host.c_str(),
                AddressText(address).c_str(), port, path.c_str());
  return true;
}

bool locateGauge(IPAddress &address, uint16_t &port, String &path) {
  if (!directAddressFailed_ && configuredAddress_ != IPAddress() &&
      sameSubnet(configuredAddress_, WiFi.localIP(), WiFi.subnetMask())) {
    address = configuredAddress_;
    port = configuredPort_;
    path = storedPath();
    return port != 0;
  }
  if (!startMdns()) return false;
  const String &expected = serverId_;
  if (expected.empty()) {
    Serial.println("[dashboard] no paired server_id stored; nothing to discover");
    return false;
  }

  const int services = MDNS.queryService("gauge", "tcp");
  logPrintf("[dashboard] mDNS _gauge._tcp -> %d service(s), want id=%s\n",
                services, expected.c_str());
  if (services <= 0) {
    logPrintf("[dashboard] nothing advertised; we are %s/%s on \"%s\"\n",
                  AddressText(WiFi.localIP()).c_str(),
                  AddressText(WiFi.subnetMask()).c_str(), wifiSsid_);
  }

  IPAddress fallback;
  uint16_t fallbackPort = 0;
  uint8_t idMatches = 0;
  for (int i = 0; i < services; ++i) {
    const String advertised = MDNS.txt(i, "id");
    const IPAddress candidate = MDNS.IP(i);
    const uint16_t candidatePort = MDNS.port(i);
    if (advertised != expected) {
      logPrintf("[dashboard]   [%d] %s:%u id=%s -- not our server\n", i,
                    AddressText(candidate).c_str(), candidatePort,
                    advertised.empty() ? "(none)" : advertised.c_str());
      continue;
    }
    ++idMatches;
    if (candidate == IPAddress() || candidatePort == 0 || candidate[0] == 127) {
      logPrintf("[dashboard]   [%d] %s:%u -- unusable address\n", i,
                    AddressText(candidate).c_str(), candidatePort);
      continue;
    }
    if (sameSubnet(candidate, WiFi.localIP(), WiFi.subnetMask())) {
      address = candidate;
      port = candidatePort;
      path = storedPath();
      logPrintf("[dashboard] discovered Gauge at %s:%u%s\n",
                    AddressText(address).c_str(), port, path.c_str());
      return true;
    }
    logPrintf("[dashboard]   [%d] %s:%u -- off-subnet (we are %s/%s)\n", i,
                  AddressText(candidate).c_str(), candidatePort,
                  AddressText(WiFi.localIP()).c_str(),
                  AddressText(WiFi.subnetMask()).c_str());
    if (fallback == IPAddress()) {
      fallback = candidate;
      fallbackPort = candidatePort;
    }
  }

  if (fallback == IPAddress()) {
    if (services > 0) {
      logPrintf(
          "[dashboard] %u service(s) matched id but none was reachable\n",
          static_cast<unsigned>(idMatches));
    }
    return resolveByHostname(expected, address, port, path);
  }
  address = fallback;
  port = fallbackPort;
  path = storedPath();
  logPrintf("[dashboard] discovered off-subnet Gauge at %s:%u\n",
                AddressText(address).c_str(), port);
  return true;
}

void noteConnectFailure(const IPAddress &address, uint16_t port) {
  directAddressFailed_ = true;
  if (connectFailures_ < UINT8_MAX) ++connectFailures_;
  logPrintf("[dashboard] TCP connect failed to %s:%u (%u/%u, RSSI %d dBm)\n",
                AddressText(address).c_str(), port, connectFailures_,
                kReconnectAfterFailures, int(WiFi.RSSI()));
  forgetDiscovery();
  if (connectFailures_ < kReconnectAfterFailures) return;

  connectFailures_ = 0;
  Serial.println("[dashboard] refreshing Wi-Fi connection");
  if (!WiFi.reconnect()) {
    WiFi.begin(wifiSsid_, wifiPassword_);
  }
}

bool fetch() {
  if (resetDiscovery_.exchange(false) && mdnsStarted_) {
    MDNS.end();
    mdnsStarted_ = false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[dashboard] skipped: Wi-Fi not connected");
    return false;
  }
  IPAddress address;
  uint16_t port = 0;
  String &path = requestPath_;
  if (!locateGauge(address, port, path)) return false;

  addressHost_ = AddressText(address).c_str();
  esp_http_client_config_t config = {};
  config.host = addressHost_.c_str();
  config.port = port;
  config.path = path.c_str();
  config.transport_type = HTTP_TRANSPORT_OVER_TCP;
  config.timeout_ms = kReadTimeoutMs;
  config.disable_auto_redirect = true;
  config.event_handler = receiveBody;
  auto request = esp_http_client_init(&config);
  if (!request) return false;
  esp_http_client_set_header(request, "Accept", "application/json");
  esp_http_client_set_header(request, "Authorization", authorization_.c_str());
  logPrintf("[dashboard] GET %s:%u%s (token %u chars)\n",
            addressHost_.c_str(), port, path.c_str(), unsigned(tokenLength_));
  bodyLength_ = 0;
  bodyOverflow_ = false;
  const uint32_t startedAt = millis();
  const esp_err_t result = esp_http_client_perform(request);
  const int status = esp_http_client_get_status_code(request);
  const bool complete = esp_http_client_is_complete_data_received(request);
  esp_http_client_cleanup(request);
  if (result != ESP_OK || status != 200 || bodyLength_ == 0 || bodyOverflow_ || !complete) {
    logPrintf("[dashboard] HTTP %d (%s) via %s:%u\n", status,
              esp_err_to_name(result), addressHost_.c_str(), port);
    if (result != ESP_OK) noteConnectFailure(address, port);
    return false;
  }
  connectFailures_ = 0;
  configuredAddress_ = address;
  directAddressFailed_ = false;
  logPrintf("[dashboard] HTTP 200, %u bytes in %u ms\n",
            unsigned(bodyLength_), unsigned(millis() - startedAt));
  return true;
}

void fetchTask(void *) {
  for (;;) {
    uint8_t command;
    xQueueReceive(fetchCommands_, &command, portMAX_DELAY);
    bool result = false;
    if (command == 2) notifyGaugeOnWorker();
    else result = fetch();
    memory::sampleTask("dashboard-net");
    xQueueSend(fetchResults_, &result, portMAX_DELAY);
  }
}

bool acceptFetched() {
  if (!adopt(body_, bodyLength_)) {
    Serial.println("[dashboard] invalid or over-budget snapshot");
    return false;
  }

  // Gauge serves an in-memory snapshot; polling does not query providers.
  refreshMs_ = kMinimumRefreshSeconds * 1000U;
  const uint32_t bodyHash = dashboard_cache::fingerprint(snapshot_);
  if (bodyHash != snapshotHash_) {
    // Persist the merged snapshot, including preserved last-good quotas.
    bodyLength_ = serializeJson(snapshot_, body_, bodyCapacity_ + 1);
    if (storage_->putBytes(cfg::nvs::kDashboard, body_,
                           bodyLength_) != bodyLength_) {
      Serial.println("[dashboard] could not persist snapshot");
    } else {
      snapshotHash_ = bodyHash;
      logPrintf("[dashboard] cached %u bytes\n",
                    static_cast<unsigned>(bodyLength_));
    }
  }
  return true;
}

// --------------------------------------------------------------- drawing ---

// Long lists rotate a window each time the carousel completes a full round, so
// entries beyond the first page are eventually shown.
size_t windowOffset(size_t itemCount, size_t itemsPerPage) {
  if (itemCount <= itemsPerPage) return 0;
  return (static_cast<size_t>(round_) * itemsPerPage) % itemCount;
}

void drawHeader(const char *title) {
  Display &panel = ui::display();
  panel.fillScreen(cfg::ui::kBackground);
  ui::drawTopBar(false);
  ui::drawText(BuntyFonts::kHeading,
               ui::fitText(BuntyFonts::kHeading, title, 216), 12, 74,
               cfg::ui::kText);
  panel.drawFastHLine(12, 89, 216, cfg::ui::kBorder);
}

void drawFooter() {
  ui::drawControls();
  ui::drawText(BuntyFonts::kBody, (online_ && !(snapshot_["quota_cached"] | false)) ? "LIVE" : "CACHED", 85, 301,
               online_ ? cfg::ui::kSuccess : cfg::ui::kMuted);
}

void drawOverview(JsonArrayConst providers) {
  drawHeader("AI USAGE");
  int16_t y = 119;
  const size_t offset = windowOffset(providers.size(), kProviderRowsPerPage);
  for (size_t i = offset;
       i < providers.size() && i < offset + kProviderRowsPerPage; ++i) {
    JsonObjectConst provider = providers[i].as<JsonObjectConst>();
    const char *name = provider["name"] | "Agent";
    const int remaining = provider["remaining_percent"] | -1;
    if (remaining == 0) continue;
    ui::drawText(BuntyFonts::kBody, ui::fitText(BuntyFonts::kBody, name, 120),
                 14, y, cfg::ui::kText);
    char percent[24];
    snprintf(percent, sizeof(percent), "%d%%", remaining);
    ui::drawTextRight(BuntyFonts::kHeading,
                      remaining >= 0 ? percent : "--", 228,
                      y + 3,
                      remaining >= 20 ? cfg::ui::kSuccess : cfg::ui::kDanger);
    y += 42;
  }
}

void drawProvider(JsonObjectConst provider) {
  drawHeader(provider["name"] | "AI LIMITS");
  JsonArrayConst limits = provider["limits"].as<JsonArrayConst>();
  int16_t y = 116;
  const size_t offset = windowOffset(limits.size(), kLimitRowsPerPage);
  for (size_t i = offset; i < limits.size() && i < offset + kLimitRowsPerPage;
       ++i) {
    JsonObjectConst limit = limits[i].as<JsonObjectConst>();
    const char *label = limit["label"] | "Limit";
    const int remaining = limit["remaining_percent"] | -1;
    if (remaining == 0) continue;
    ui::drawText(BuntyFonts::kBody, ui::fitText(BuntyFonts::kBody, label, 130),
                 14, y, cfg::ui::kText);
    char percent[24];
    snprintf(percent, sizeof(percent), "%d%% left", remaining);
    ui::drawTextRight(BuntyFonts::kBody,
                      remaining >= 0 ? percent : "--", 228,
                      y, cfg::ui::kMuted);
    y += 29;
  }
}

void drawCalendar() {
  drawHeader("CALENDAR");
  JsonArrayConst events = snapshot_["calendar"]["events"].as<JsonArrayConst>();
  int16_t y = 116;
  if (events.size() == 0) {
    ui::drawText(BuntyFonts::kBody, "No upcoming events", 14, y,
                 cfg::ui::kMuted);
    return;
  }
  const size_t offset = windowOffset(events.size(), kEventRowsPerPage);
  for (size_t i = offset; i < events.size() && i < offset + kEventRowsPerPage;
       ++i) {
    const char *title = events[i].as<JsonObjectConst>()["title"] | "Untitled";
    ui::drawText(BuntyFonts::kBody, ui::fitText(BuntyFonts::kBody, title, 212),
                 14, y, cfg::ui::kText);
    y += 34;
  }
}

void drawTodos() {
  drawHeader("TO-DO");
  JsonArrayConst todos = snapshot_["todos"].as<JsonArrayConst>();
  int16_t y = 116;
  if (todos.size() == 0) {
    ui::drawText(BuntyFonts::kBody, "Nothing left to do", 14, y,
                 cfg::ui::kMuted);
    return;
  }
  const size_t offset = windowOffset(todos.size(), kTodoRowsPerPage);
  for (size_t i = offset; i < todos.size() && i < offset + kTodoRowsPerPage;
       ++i) {
    JsonObjectConst todo = todos[i].as<JsonObjectConst>();
    const bool complete = todo["completed"] | false;
    char line[256];
    snprintf(line, sizeof(line), "%s%s", complete ? "[x] " : "[ ] ",
             static_cast<const char *>(todo["title"] | "Untitled"));
    ui::drawText(BuntyFonts::kBody, ui::fitText(BuntyFonts::kBody, line, 212),
                 14, y, complete ? cfg::ui::kMuted : cfg::ui::kText);
    y += 29;
  }
}

}  // namespace

void begin(Preferences &storage, const String &hardwareId) {
  storage_ = &storage;
  hostname_ = hardwareId;
  const bool psram = ESP.getPsramSize() > 0;
  bodyCapacity_ = psram ? kMaximumBytes : 4096;
  const size_t arenaBytes = psram ? 65536 : 16384;
  body_ = static_cast<char *>(psram ? memory::external(bodyCapacity_ + 1) : memory::internal(bodyCapacity_ + 1));
  for (int i = 0; i < 2; ++i) {
    void *buffer = psram ? memory::external(arenaBytes) : memory::internal(arenaBytes);
    arenaStorage_[i] = buffer;
    if (!buffer || !body_) {
      Serial.println("[dashboard] JSON arena unavailable");
      for (auto &allocation : arenaStorage_) { heap_caps_free(allocation); allocation = nullptr; }
      heap_caps_free(body_);
      body_ = nullptr;
      return;
    }
    arenas_[i].bind(buffer, arenaBytes);
  }
  arenaCapacity_ = arenaBytes;
  loadCached();
  fetchCommands_ = xQueueCreateStatic(1, 1, commandStorage_, &commandControl_);
  fetchResults_ = xQueueCreateStatic(1, sizeof(bool), resultStorage_, &resultControl_);
  if (xTaskCreatePinnedToCore(fetchTask, "dashboard-net", 6144, nullptr, 1, nullptr, 0) != pdPASS)
    fetchCommands_ = nullptr;
}

void restart() {
  serverId_ = storage_->getString(cfg::nvs::kServerId, "");
  mdnsHost_ = "gauge-" + serverId_.substr(0, 12);
  requestPath_ = storage_->getString(cfg::nvs::kPath, kDefaultPath);
  const String token = storage_->getString(cfg::nvs::kToken, "");
  tokenLength_ = token.length();
  authorization_ = "Bearer " + token;
  configuredPort_ = storage_->getUShort(cfg::nvs::kPort, 0);
  configuredAddress_.fromString(storage_->getString(cfg::nvs::kServerIp, ""));
  directAddressFailed_ = false;
  attemptedFetch_ = false;
  storage_->getString(cfg::nvs::kWifiSsid, wifiSsid_, sizeof(wifiSsid_));
  storage_->getString(cfg::nvs::kWifiPassword, wifiPassword_, sizeof(wifiPassword_));
  addressHost_.reserve(16);
  online_ = false;
  nextFetchAt_ = 0;
  page_ = 0;
  round_ = 0;
}

bool available() { return available_; }
bool online() { return online_; }

void reportMemory() {
  logPrintf("[memory] dashboard body_capacity=%u arenas=2x%u live_used=%u spare_used=%u placement=%s\n",
      unsigned(bodyCapacity_), unsigned(arenaCapacity_), unsigned(arenas_[liveArena_].used()),
      unsigned(arenas_[liveArena_ ^ 1].used()), ESP.getPsramSize() ? "psram" : "internal");
}

void refreshSoon() { nextFetchAt_ = 0; }

void forgetDiscovery() {
  resetDiscovery_ = true;
}

bool serviceFetch() {
  if (!fetchCommands_) return false;
  bool result;
  if (fetchBusy_ && xQueueReceive(fetchResults_, &result, 0) == pdTRUE) {
    fetchBusy_ = false;
    attemptedFetch_ = true;
    const bool wasOnline = online_;
    online_ = result && acceptFetched();
    nextFetchAt_ = millis() + (online_ ? refreshMs_ : kRetryMs);
    return !available_ || online_ || wasOnline != online_;
  }
  if (!fetchBusy_ && static_cast<int32_t>(millis() - nextFetchAt_) >= 0) {
    uint8_t command = 1;
    fetchBusy_ = xQueueSend(fetchCommands_, &command, 0) == pdTRUE;
  }
  return false;
}

bool pageDue() {
  return available_ && static_cast<int32_t>(millis() - nextPageAt_) >= 0;
}

void nextPage() {
  if (pageCount_ == 0) return;
  page_ = static_cast<uint8_t>((page_ + 1) % pageCount_);
  if (page_ == 0) ++round_;
}

void render() {
  ui::Guard guard;
  if (!available_) {
    ui::showSyncing(attemptedFetch_);
    return;
  }

  JsonArrayConst providers =
      snapshot_["quota"]["providers"].as<JsonArrayConst>();
  if (page_ >= pageCount_) page_ = 0;

  if (page_ == 0) {
    drawOverview(providers);
  } else if (page_ <= providers.size()) {
    drawProvider(providers[page_ - 1].as<JsonObjectConst>());
  } else if (page_ == providers.size() + 1) {
    drawCalendar();
  } else {
    drawTodos();
  }
  drawFooter();
  ui::display().present();
  nextPageAt_ = millis() + kPageTimeMs;
}

void notifyGaugeBeforeUnpair() {
  if (!fetchCommands_) return;
  if (fetchBusy_) {
    bool ignoredResult;
    xQueueReceive(fetchResults_, &ignoredResult, portMAX_DELAY);
    fetchBusy_ = false;
  }
  const uint8_t command = 2;
  xQueueSend(fetchCommands_, &command, portMAX_DELAY);
  bool result;
  xQueueReceive(fetchResults_, &result, portMAX_DELAY);
}

void notifyGaugeOnWorker() {
  if (WiFi.status() != WL_CONNECTED) return;
  IPAddress address;
  uint16_t port = 0;
  String &ignored = requestPath_;
  if (!locateGauge(address, port, ignored)) return;

  addressHost_ = AddressText(address).c_str();
  esp_http_client_config_t config = {};
  config.host = addressHost_.c_str();
  config.port = port;
  config.path = "/v1/accessory";
  config.method = HTTP_METHOD_DELETE;
  config.transport_type = HTTP_TRANSPORT_OVER_TCP;
  config.timeout_ms = kUnpairNotifyReadMs;
  config.disable_auto_redirect = true;
  auto request = esp_http_client_init(&config);
  if (!request) return;
  esp_http_client_set_header(request, "Authorization", authorization_.c_str());
  esp_http_client_perform(request);
  const int status = esp_http_client_get_status_code(request);
  esp_http_client_cleanup(request);
  logPrintf("[unpair] Gauge returned HTTP %d\n", status);
}

void clear() {
  snapshot_.clear();
  snapshot_.shrinkToFit();
  snapshotHash_ = 0;
  available_ = false;
  online_ = false;
  pageCount_ = 0;
  connectFailures_ = 0;
  forgetDiscovery();
}

}  // namespace dashboard
