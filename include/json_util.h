#pragma once
// =============================================================================
// json_util.h — Minimal JSON string escaping helper
// =============================================================================
//
// Rather than pulling in a full JSON library, the codebase builds JSON strings
// manually using String concatenation.  This header supplies the one function
// required to safely embed arbitrary C++ strings as JSON string values.
//
// Characters handled:
//   '"'  and '\'  →  prefixed with '\' (prevents broken JSON structure)
//   '\n'          →  "\n"  (newlines are illegal in JSON string literals)
//   '\r'          →  "\r"
//   all others    →  passed through unchanged
//
// This coverage is sufficient for all string types this device handles:
// BLE device names, WiFi SSIDs, URL fragments, and log messages.
// =============================================================================

#include <Arduino.h>

namespace JsonUtil {

// Return a copy of `in` with all JSON-structurally significant characters
// escaped.  inline so the compiler can eliminate the function-call overhead
// at each call site.
inline String escape(const String& in) {
  String out = "";
  out.reserve(in.length() + 8); // small headroom avoids most reallocations
  for (size_t i = 0; i < in.length(); i++) {
    const char c = in[i];
    if (c == '\\' || c == '"') {
      // Both backslash and double-quote must be escaped to avoid prematurely
      // closing the JSON string or creating ambiguous escape sequences.
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else {
      out += c;
    }
  }
  return out;
}

} // namespace JsonUtil