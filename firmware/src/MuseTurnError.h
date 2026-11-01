#pragma once
#include <cstring>
namespace muse {
// SDK-controlled reasons only; never display arbitrary remote error text.
inline const char *turnError(const char *reason) {
  if(!reason)return "Muse request failed";
  if(strstr(reason,"403") || strstr(reason,"401"))return "Muse reply access denied";
  if(strstr(reason,"LOST CONNECTION"))return "Muse connection interrupted";
  if(strstr(reason,"NO REPLY"))return "Muse reply timed out";
  if(strstr(reason,"OUT OF MEMORY"))return "Muse request memory unavailable";
  if(strstr(reason,"DIDN'T TAKE"))return "Muse rejected voice upload";
  if(strstr(reason,"TOO LONG") || strstr(reason,"BUFFER LIMIT"))return "Muse reply exceeded limits";
  if(strstr(reason,"CAN'T KEEP UP"))return "Muse upload queue stalled";
  if(strstr(reason,"CAN'T SUBSCRIBE"))return "Muse reply stream unavailable";
  if(strstr(reason,"CAN'T REACH"))return "Muse upload unavailable";
  if(strstr(reason,"HTTP 429"))return "Muse busy; please retry";
  if(strstr(reason,"HTTP 5"))return "Muse service temporarily unavailable";
  if(strstr(reason,"HTTP"))return "Muse reply protocol error";
  return "Muse request failed";
}
}
