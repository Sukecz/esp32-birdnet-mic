#include "Arduino.h"
#include "../esp32-birdnet-mic/Antenna.h"
#include <cassert>
bool storedPresent=false,storedExternal=true,failOpen=false,failWrite=false;
int reads=0,writes=0;
std::vector<std::pair<int,int>> pinWrites,pinModes;
int main(){
 antenna::begin();
#if defined(ARDUINO_XIAO_ESP32C6)
 assert(antenna::supported()&&antenna::external());
 assert(pinWrites.size()==2&&pinWrites[0]==std::make_pair(3,LOW)&&pinWrites[1]==std::make_pair(14,HIGH));
 assert(pinModes.size()==2&&pinModes[0]==std::make_pair(3,OUTPUT)&&pinModes[1]==std::make_pair(14,OUTPUT));
 assert(writes==0);assert(antenna::save(true)&&writes==0); // No unnecessary flash write or RF switch.
 const auto pins=pinWrites;
 assert(antenna::save(false)&&writes==1&&antenna::external()&&pinWrites==pins); // Only save; RF stays external until restart.
 antenna::begin();assert(!antenna::external()&&pinWrites.back()==std::make_pair(14,LOW));
 assert(antenna::save(true));antenna::begin();assert(antenna::external()&&pinWrites.back()==std::make_pair(14,HIGH));
 failWrite=true;assert(!antenna::save(false));failWrite=false;antenna::begin();assert(antenna::external());
 failOpen=true;assert(!antenna::save(false));antenna::begin();assert(antenna::external());
#else
 assert(!antenna::supported()&&reads==0&&writes==0&&pinWrites.empty()&&pinModes.empty());
 assert(!antenna::save(false)&&!antenna::save(true)&&writes==0);
#endif
}
