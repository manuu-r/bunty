#include <cassert>
#include <cstdio>
#include "DashboardCache.h"
int main() {
  JsonDocument previous, incoming;
  deserializeJson(previous, R"({"generated_at":1,"quota":{"providers":[{"name":"Claude","remaining_percent":75}],"errors":[]},"todos":[]})");
  deserializeJson(incoming, R"({"generated_at":2,"quota":{"providers":[],"errors":["Refreshing"]},"todos":[{"title":"New task"}]})");
  assert(dashboard_cache::preserveQuota(incoming, previous));
  assert(incoming["quota"]["providers"][0]["remaining_percent"] == 75);
  assert(incoming["todos"].size() == 1);
  assert(incoming["quota_cached"] == true);
  auto fingerprint = dashboard_cache::fingerprint(incoming);
  incoming["generated_at"] = 999;
  assert(dashboard_cache::fingerprint(incoming) == fingerprint);
  incoming["quota"]["providers"][0]["remaining_percent"] = 74;
  assert(dashboard_cache::fingerprint(incoming) != fingerprint);
  deserializeJson(incoming, R"({"quota":{"providers":[],"errors":[]}})");
  assert(!dashboard_cache::preserveQuota(incoming, previous)); // disabled providers stay disabled
  deserializeJson(incoming, R"({"quota":{"providers":[{"name":"Claude","remaining_percent":60}],"errors":[]}})");
  assert(!dashboard_cache::preserveQuota(incoming, previous));
  assert(incoming["quota"]["providers"][0]["remaining_percent"] == 60);
  puts("Dashboard cache: last-good retention, fresh updates, disabled providers, timestamp deduplication passed");
}
