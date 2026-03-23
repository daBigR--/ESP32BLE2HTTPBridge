#pragma once
// =============================================================================
// key_log.h — Rolling in-memory event log
// =============================================================================
//
// Maintains a bounded deque of recent event messages in RAM for display in the
// web UI's live status panel.  The deque acts as a ring buffer: once the
// maximum capacity (40 entries) is reached, the oldest message is evicted to
// make room for each new one.
//
// All messages added via add() are also echoed to Serial at 115200 baud so
// they appear in the PlatformIO serial monitor during development.
//
// The web UI periodically polls /state, which includes a JSON representation
// of this log (newest-first) via toJson().
// =============================================================================

#include <Arduino.h>

namespace KeyLog {

// Append a message to the log and echo it to Serial.
// If the log is at capacity the oldest entry is silently discarded.
void add(const String& line);

// Return the current log contents as a JSON array of strings, newest-first.
// All string values are properly escaped with JsonUtil::escape().
String toJson();

} // namespace KeyLog