// =============================================================================
// net_fetch.cpp — Synchronous outbound HTTP GET implementation
// =============================================================================

#include "net_fetch.h"

#include <HTTPClient.h>
#include <WiFi.h>

namespace NetFetch {

HttpResponse httpGet(const String& url, uint32_t timeout_ms) {
  if (WiFi.status() != WL_CONNECTED) {
    return {-1, "", "STA not connected", false, false};
  }

  HTTPClient http;
  http.setTimeout(timeout_ms);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(url)) {
    http.end();
    return {-1, "", "HTTP begin failed", false, false};
  }

  int code = http.GET();

  if (code < 0) {
    // Negative codes are HTTPClient transport errors, not HTTP status codes.
    String err = String("Transport error (") + String(code) + String(")");
    http.end();
    return {code, "", err, false, false};
  }

  // Read response body, capped at MAX_BODY_BYTES.
  bool   truncated = false;
  String body;

  int clen = http.getSize(); // content-length; -1 if unknown (chunked/streaming)

  if (clen >= 0 && (size_t)clen > MAX_BODY_BYTES) {
    // Content-Length tells us the response is larger than our cap — stream
    // manually so we don't allocate a huge buffer via getString().
    truncated = true;
    WiFiClient* stream = http.getStreamPtr();
    if (stream) {
      body.reserve(MAX_BODY_BYTES);
      unsigned long deadline = millis() + timeout_ms;
      size_t read = 0;
      while (read < MAX_BODY_BYTES && millis() < deadline) {
        if (stream->available()) {
          body += (char)stream->read();
          read++;
        } else if (!http.connected()) {
          break;
        } else {
          delay(1);
        }
      }
    }
  } else {
    // Content-Length unknown or within limit — getString() is fine.
    body = http.getString();
    if (body.length() > MAX_BODY_BYTES) {
      body      = body.substring(0, MAX_BODY_BYTES);
      truncated = true;
    }
  }

  http.end();

  bool   success = (code >= 200 && code < 300);
  String error   = success ? "" : String("HTTP ") + String(code);
  return {code, body, error, success, truncated};
}

} // namespace NetFetch
