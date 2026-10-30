#pragma once
#include <cstddef>
#include <cstring>
extern bool storedPresent,storedExternal,failOpen,failWrite;
extern int reads,writes;
class Preferences {
public:
 bool begin(const char* name,bool){return !failOpen && strcmp(name,"wifiant")==0;}
 bool getBool(const char* key,bool fallback){++reads;return strcmp(key,"external")==0&&storedPresent?storedExternal:fallback;}
 size_t putBool(const char* key,bool value){++writes;if(failWrite||strcmp(key,"external"))return 0;storedExternal=value;storedPresent=true;return 1;}
 void end(){}
};
