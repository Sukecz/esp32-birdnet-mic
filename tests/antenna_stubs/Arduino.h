#pragma once
#include <cstdint>
#include <vector>
#include <utility>
constexpr int OUTPUT=1,LOW=0,HIGH=1;
extern std::vector<std::pair<int,int>> pinWrites,pinModes;
inline void pinMode(int pin,int mode){pinModes.emplace_back(pin,mode);}
inline void digitalWrite(int pin,int level){pinWrites.emplace_back(pin,level);}
