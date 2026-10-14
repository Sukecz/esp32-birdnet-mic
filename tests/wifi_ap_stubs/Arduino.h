#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
using String = std::string;
extern unsigned long testNow;
inline unsigned long millis() { return testNow; }
