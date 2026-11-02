#pragma once
#include <Arduino.h>
// DSP core contributed by Dan Logue. Engine belongs exclusively to capture;
// HTTP reads a bounded snapshot and never locks or delays audio processing.
namespace noiseshadow {
struct Chain { uint32_t sampleRate; uint8_t micFormat,shift; bool highpass; uint16_t highpassHz; int32_t gainQ; };
void begin();
void process(int16_t* pcm,int n,const Chain& chain);
void releaseForRestart(); // producer must already be stopped
bool save(uint8_t mode,uint8_t floor,uint8_t adapt); // validate/persist all first
void resetSettings();
String statusJson(bool spectra=false);
}
