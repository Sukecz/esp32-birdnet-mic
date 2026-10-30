#pragma once

// Only XIAO ESP32-C6 has this GPIO-controlled RF switch among our targets.
namespace antenna {
bool supported();
void begin();
bool external(); // Actual selection applied at this boot.
bool save(bool useExternal); // Persist for the next boot; never switches RF live.
}
