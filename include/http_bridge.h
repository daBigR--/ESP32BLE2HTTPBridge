#pragma once

#include <Arduino.h>

namespace HttpBridge {

using LogFn = void (*)(const String& line);
using BaseUrlFn = String (*)();
using MappedPathFn = String (*)(uint8_t keyCode);
using GetStartFn = void (*)();
using GetResultFn = void (*)(int statusCode);

void begin(LogFn logFn, BaseUrlFn baseUrlFn, MappedPathFn mappedPathFn);
void setGetCallbacks(GetStartFn getStartFn, GetResultFn getResultFn);
void onKeyPress(uint8_t keyCode);
void processPendingKeys();

} // namespace HttpBridge
