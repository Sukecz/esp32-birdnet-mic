"""Execute production detection policy with bounded probe/driver adapters."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class AutoAudioTests(unittest.TestCase):
    def test_policy_migration_independence_and_complete_rollback(self):
        source=(ROOT/'esp32-birdnet-mic/AudioCapture.cpp').read_text()
        policy=source[source.index('namespace {\nstruct DetectionConfig'):]
        stub=r'''
#include <string>
#include <cassert>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include "MicAutoDetect.h"
#include "S3Boards.h"
using String=std::string;
#define MIC_PDM_SOFTWARE 1
constexpr uint8_t MIC_FORMAT_PDM=2,MIC_FORMAT_PHILIPS=0,MIC_FORMAT_MSB=1;
constexpr uint32_t PDM_SAMPLE_RATE=48000;
constexpr int I2S_BCLK_PIN=4,I2S_DOUT_PIN=3;
constexpr size_t MIC_PROBE_WORDS=2048;
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t n,int){return malloc(n);}
uint8_t s3BoardVariant=0,micFormat=0,cpuFrequencyMhz=80;
uint32_t currentSampleRate=32000,minAcceptableRate=20;
uint16_t highpassCutoffHz=300;
float currentGainFactor=4;
bool micAutoDetect=false,s3BoardAutoDetect=false,microphoneGainConfigured=true;
bool overheatLatched=false,otaInProgress=false,autoThresholdEnabled=true;
String microphoneDetectionNote;
int8_t microphoneDetectedFormat=-1,detectedS3Board=-1;
bool detectionBootChanged=false,s3=false,psram=true,sense=false,atom=false,externalPdm=false;
int probes=0,saves=0,restarts=0;
micdetect::Result i2s=micdetect::None;
namespace recordingpins{bool senseSdPresent=false;}
namespace recording {bool working=false,recordingMode=false;bool busy(){return working;}bool mode(){return recordingMode;} }
bool s3BoardSelectionSupported(){return s3;}
bool micBoardHasPsram(){return psram;}
bool pdmMicSupported(){return true;}
bool micBoardFormatAllowed(uint8_t f){return !s3||s3BoardVariant!=2||f==2;}
bool micProbePdm(int c,int,int32_t*){++probes;return c==42?sense:c==39?atom:externalPdm;}
micdetect::Result micProbeI2s(int32_t*){++probes;return i2s;}
const char* micFormatName(uint8_t){return "format";}
const char* FW_BOARD_NAME_STR="board";
uint16_t normalizeHighpassCutoff(uint32_t,uint16_t c){return c;}
uint32_t computeRecommendedMinRate(){return currentSampleRate/1024;}
void updateS3BoardIdentity(){}
bool setCpuFrequencyMhz(uint8_t){return true;}
void stopAudioProducer(){}
void* i2s_rx_handle=nullptr;
void i2s_channel_disable(void*){}void i2s_del_channel(void*){}void releasePdmCapture(){}
void saveAudioSettings(){++saves;}
bool restartOk=true;
bool restartI2S(){++restarts;return restartOk;}
bool applyMicProfileConfig(uint8_t f,bool){micFormat=f;return restartOk;}
bool applyS3BoardConfig(uint8_t b){s3BoardVariant=b;return restartOk;}
'''
        checks=r'''
int main(){
 // Upgraded manual installation: no probes or mutation at boot.
 detectMicrophoneAtBoot(); assert(probes==0&&micFormat==0&&saves==0);
 // A fresh Auto device with no mic remains usable and does not persist a guess.
 micAutoDetect=true;detectMicrophoneAtBoot();confirmMicrophoneBoot();
 assert(microphoneDetectedFormat==-1&&saves==0&&currentGainFactor==4);
 // Auto PDM changes the necessary rate/CPU but never overwrites tuned gain.
 externalPdm=true;detectMicrophoneAtBoot();assert(micFormat==2&&currentGainFactor==4&&cpuFrequencyMhz==160);
 assert(saves==0);confirmMicrophoneBoot();assert(saves==1);
 // Starting gain is used only on a fresh, unconfigured device.
 microphoneGainConfigured=false;currentGainFactor=1.5;detectMicrophoneAtBoot();assert(currentGainFactor==24);confirmMicrophoneBoot();
 currentGainFactor=4;detectMicrophoneAtBoot();assert(currentGainFactor==4);
 // Missing PSRAM and missing Atom mic cannot reclassify an existing XIAO.
 s3=true;s3BoardAutoDetect=true;psram=false;externalPdm=false;micAutoDetect=false;micFormat=0;
 detectMicrophoneAtBoot();assert(s3BoardVariant==0&&micFormat==0&&microphoneDetectedFormat==-1&&detectedS3Board==-1);
 // Positive Atom probe may identify board, but must not force a manual I2S selection.
 atom=true;detectMicrophoneAtBoot();assert(s3BoardVariant==0&&micFormat==0);
 micAutoDetect=true;detectMicrophoneAtBoot();assert(s3BoardVariant==2&&micFormat==2&&detectedS3Board==2);
 // A saved Sense remains a Sense, including its SD route, when microphone fails.
 atom=false;psram=true;sense=false;s3BoardVariant=1;recordingpins::senseSdPresent=true;
 detectMicrophoneAtBoot();assert(s3BoardVariant==1&&recordingpins::senseSdPresent&&detectedS3Board==-1);
 // Manual board plus Auto mic is an independent combination.
 s3BoardAutoDetect=false;sense=true;detectMicrophoneAtBoot();assert(s3BoardVariant==1&&micFormat==2&&micAutoDetect);
 // Runtime failure restores format, board, CPU, gain, rate and both Auto flags.
 s3=false;s3BoardVariant=0;micAutoDetect=false;s3BoardAutoDetect=false;micFormat=0;
 currentSampleRate=32000;cpuFrequencyMhz=80;currentGainFactor=3;externalPdm=true;restartOk=false;
 detectedS3Board=1;microphoneDetectedFormat=-1;microphoneDetectionNote="previous check";
 assert(!applyMicrophoneChoice(3,false));assert(detectedS3Board==1&&microphoneDetectedFormat==-1&&microphoneDetectionNote=="previous check");assert(!micAutoDetect&&!s3BoardAutoDetect&&micFormat==0&&currentSampleRate==32000&&currentGainFactor==3&&cpuFrequencyMhz==80);
 recording::working=true;int old=probes;assert(!applyMicrophoneChoice(3,false)&&old==probes);
}
'''
        with tempfile.TemporaryDirectory() as t:
            cpp=Path(t)/'test.cpp';binary=Path(t)/'test';cpp.write_text(stub+policy+checks)
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'esp32-birdnet-mic'),str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_native_dsp_and_classifiers(self):
        for name in ['noise_signature','mic_auto_detect']:
            with tempfile.TemporaryDirectory() as t:
                binary=Path(t)/'test'
                subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'esp32-birdnet-mic'),str(ROOT/'tests'/f'{name}_test.cpp'),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True,timeout=60)
