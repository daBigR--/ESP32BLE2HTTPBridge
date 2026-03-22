#include "key_log.h"

#include <deque>

#include "json_util.h"

namespace {

static std::deque<String> gKeyLog;
static const size_t MAX_KEY_LOG = 40;

} // namespace

namespace KeyLog {

void add(const String& line) {
  if (line.length() == 0) {
    return;
  }
  gKeyLog.push_back(line);
  while (gKeyLog.size() > MAX_KEY_LOG) {
    gKeyLog.pop_front();
  }
  Serial.println(line);
}

String toJson() {
  String out = "[";
  size_t index = 0;
  for (const String& line : gKeyLog) {
    if (index++ > 0) {
      out += ",";
    }
    out += "\"" + JsonUtil::escape(line) + "\"";
  }
  out += "]";
  return out;
}

} // namespace KeyLog