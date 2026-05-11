#pragma once
// =============================================================================
// net_fetch.h — Simple synchronous outbound HTTP GET client
// =============================================================================
//
// Wraps Arduino's HTTPClient to provide a single blocking GET function.
// Caller is responsible for ensuring WiFi STA is connected before calling
// httpGet() — this module does NOT manage APSTA mode.
//
// Use Apsta::enterApsta() / Apsta::exitApsta() (or Apsta::withStaInterface)
// around calls to httpGet() to handle mode switching.
// =============================================================================

#include <Arduino.h>

namespace NetFetch {

// Maximum response body bytes read into memory.  Responses larger than this
// are read up to this limit and flagged as truncated.  200 KB is enough for
// a typical KOReader status page while keeping heap pressure bounded.
static const size_t MAX_BODY_BYTES = 200UL * 1024UL;

// Maximum redirect hops followed automatically.
static const int MAX_REDIRECTS = 3;

struct HttpResponse {
  int    status;      // HTTP status code (e.g., 200, 404).
                      // Negative on transport error (HTTPClient error codes).
  String body;        // Response body, possibly truncated to MAX_BODY_BYTES.
  String error;       // Human-readable error (empty when success == true).
  bool   success;     // true iff status is 2xx.
  bool   truncated;   // true iff body was cut at MAX_BODY_BYTES.
};

// Perform a synchronous HTTP GET.
//   url         — full URL including scheme (http:// or https://).
//   timeout_ms  — per-request timeout in milliseconds (default 5 s).
//
// Precondition: WiFi STA must already be connected (WL_CONNECTED).
// Follows up to MAX_REDIRECTS redirects.  Response body is capped at
// MAX_BODY_BYTES; if the body is larger it is truncated and HttpResponse
// .truncated is set to true.
HttpResponse httpGet(const String& url, uint32_t timeout_ms = 5000);

} // namespace NetFetch
