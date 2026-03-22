#include "http_bridge.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include <deque>

namespace {

std::deque<uint8_t> gPendingKeyCodes;
uint8_t gLastDispatchedKeyCode = 0;
unsigned long gLastDispatchMs = 0;
const unsigned long KEY_REPEAT_FILTER_MS = 120;

HttpBridge::LogFn gLogFn = nullptr;
HttpBridge::BaseUrlFn gBaseUrlFn = nullptr;
HttpBridge::MappedPathFn gMappedPathFn = nullptr;
HttpBridge::GetStartFn gGetStartFn = nullptr;
HttpBridge::GetResultFn gGetResultFn = nullptr;

void addKeyLog(const String& line) {
  if (gLogFn) {
    gLogFn(line);
  }
}

void dispatchKeyHttp(uint8_t keyCode) {
  if (!gMappedPathFn || !gBaseUrlFn) {
    return;
  }

  String path = gMappedPathFn(keyCode);
  if (path.length() == 0) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    addKeyLog("HTTP skipped: WiFi not connected");
    return;
  }

  String url = gBaseUrlFn();
  bool baseHasSlash = url.endsWith("/");
  bool pathHasSlash = path.startsWith("/");
  if (baseHasSlash && pathHasSlash) {
    url.remove(url.length() - 1);
  } else if (!baseHasSlash && !pathHasSlash) {
    url += "/";
  }
  url += path;

  HTTPClient http;
  if (!http.begin(url)) {
    addKeyLog(String("HTTP begin failed: ") + url);
    return;
  }

  if (gGetStartFn) {
    gGetStartFn();
  }

  int rc = http.GET();
  if (gGetResultFn) {
    gGetResultFn(rc);
  }
  if (rc > 0) {
    addKeyLog(String("HTTP GET ") + url + String(" -> ") + String(rc));
  } else {
    addKeyLog(String("HTTP GET failed: ") + HTTPClient::errorToString(rc));
  }
  http.end();
}

} // namespace

namespace HttpBridge {

void begin(LogFn logFn, BaseUrlFn baseUrlFn, MappedPathFn mappedPathFn) {
  gLogFn = logFn;
  gBaseUrlFn = baseUrlFn;
  gMappedPathFn = mappedPathFn;
}

void setGetCallbacks(GetStartFn getStartFn, GetResultFn getResultFn) {
  gGetStartFn = getStartFn;
  gGetResultFn = getResultFn;
}

void onKeyPress(uint8_t keyCode) {
  if (keyCode == 0) {
    return;
  }
  if (gPendingKeyCodes.size() >= 24) {
    gPendingKeyCodes.pop_front();
  }
  gPendingKeyCodes.push_back(keyCode);
}

void processPendingKeys() {
  while (!gPendingKeyCodes.empty()) {
    uint8_t keyCode = gPendingKeyCodes.front();
    gPendingKeyCodes.pop_front();

    unsigned long now = millis();
    if (keyCode == gLastDispatchedKeyCode && (now - gLastDispatchMs) < KEY_REPEAT_FILTER_MS) {
      continue;
    }

    gLastDispatchedKeyCode = keyCode;
    gLastDispatchMs = now;
    dispatchKeyHttp(keyCode);
  }
}

} // namespace HttpBridge
