#pragma once
#include <Arduino.h>
#include <WebServer.h>
extern WebServer web;
bool hasMutationAuth();
bool requireMutationAuth(bool preparation);
void apiSendJSON(const String& json);
String jsonEscape(const String& value);
