// =============================================================================
// key_log.cpp — rolling in-memory event / debug log
// =============================================================================
//
// Maintains a bounded deque of String log lines.  Lines are added from
// anywhere in the firmware and automatically echoed to Serial so they appear
// in the PlatformIO monitor.  The web UI polls /state periodically and
// receives the log as a JSON array, giving real-time visibility into what the
// firmware is doing without needing a serial connection.
//
// The maximum size (MAX_KEY_LOG = 40 lines) is chosen to give enough context
// for diagnosing connection issues while keeping RAM usage bounded.
// =============================================================================

#include "key_log.h"

#include <deque>

#include "json_util.h"

namespace {

// The log is a simple deque: new entries are appended at the back and the
// oldest entry is removed from the front when the cap is reached.
static std::deque<String> gKeyLog;
static const size_t MAX_KEY_LOG = 40;

} // namespace

namespace KeyLog {

// ---------------------------------------------------------------------------
// add — append a line to the log
// ---------------------------------------------------------------------------
// Empty lines are silently dropped to avoid wasting log capacity.
// Once MAX_KEY_LOG entries are present the oldest line is evicted, preserving
// the most recent context (the part most useful for diagnosing current issues).
void add(const String& line) {
  if (line.length() == 0) {
    return;
  }
  gKeyLog.push_back(line);
  while (gKeyLog.size() > MAX_KEY_LOG) {
    gKeyLog.pop_front(); // evict oldest
  }
  Serial.println(line); // mirror to Serial for wired debugging
}

// ---------------------------------------------------------------------------
// toJson — serialise the log to a JSON array
// ---------------------------------------------------------------------------
// Called by the web UI status endpoint.  Each line is JSON-escaped so that
// embedded quotes, backslashes, or newlines do not break the JSON structure.
String toJson() {
  String out = "[";
  size_t index = 0;
  for (const String& line : gKeyLog) {
    if (index++ > 0) { out += ","; }
    out += "\"" + JsonUtil::escape(line) + "\"";
  }
  out += "]";
  return out;
}

} // namespace KeyLog