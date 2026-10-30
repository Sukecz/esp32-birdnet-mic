#pragma once
#include <Arduino.h>
#include "PowerPolicy.h"
#include "Battery.h"
namespace fieldpower {
void begin();
Settings settings();
bool saveSettings(const Settings& value);
bool observe(bool recordingMode, const battery::Reading& reading);
bool canStart(const battery::Reading& reading);
bool triggered();
bool acknowledge(const battery::Reading& reading);
void saved(bool success);
String statusJson(const battery::Reading& reading);
}
