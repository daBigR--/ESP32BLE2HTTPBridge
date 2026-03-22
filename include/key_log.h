#pragma once

#include <Arduino.h>

namespace KeyLog {

void add(const String& line);

String toJson();

} // namespace KeyLog