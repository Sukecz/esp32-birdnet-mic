#include "AppRuntime.h"
#include "AudioCapture.h"
#include "FirmwareDefaults.h"
#include "MqttService.h"
#include "RtspService.h"
#include "AudioMemory.h"
#include "AudioReserve.h"
#include "RecordingMemory.h"
#include "RecordingPins.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <math.h>
#define FXAUDIO_HOT IRAM_ATTR __attribute__((noinline))
#include "FixedAudio.h"
#if SOC_I2S_SUPPORTS_PDM2PCM
#define MIC_PDM_HARDWARE 1
#else
#define MIC_PDM_HARDWARE 0
#endif

// PDM capability follows the installed ESP-IDF target, not a guessed board name.
#if SOC_I2S_SUPPORTS_PDM_RX && !SOC_I2S_SUPPORTS_PDM2PCM
#define MIC_PDM_SOFTWARE 1
#define PDMSW_HOT IRAM_ATTR __attribute__((noinline))
#include "PdmCapture.h"
struct PdmReadBuffers {
    uint8_t bytes[2048];
    uint16_t words[1025]; // +1 for a carried byte from a short DMA read
};
static pdmsw::Decimator* pdmDecimator = nullptr;
static PdmReadBuffers* pdmReadBuffers = nullptr;
static pdmsw::WordFramer pdmFramer;
static pdmsw::WordRateDetector pdmWordRate;
static uint32_t pdmReadChunks = 0;
static std::atomic<uint8_t> pdmDetectedStride{0};
static std::atomic<uint8_t> pdmStrideLogPending{0};
static std::atomic<uint8_t> pdmDetectedOrder{0};
#else
#define MIC_PDM_SOFTWARE 0
#endif

static constexpr uint32_t PDM_CLOCK_HZ = PDM_SAMPLE_RATE * 64;


#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32S3) || \
    defined(ARDUINO_XIAO_ESP32C5) || defined(ARDUINO_XIAO_ESP32C6)
static constexpr int I2S_MCLK_PIN = D7;
static constexpr int I2S_BCLK_PIN = D3;
static constexpr int I2S_LRCLK_PIN = D1;
static constexpr int I2S_DOUT_PIN = D2;
#else
static constexpr int I2S_MCLK_PIN = 7;
static constexpr int I2S_BCLK_PIN = 21;
static constexpr int I2S_LRCLK_PIN = 1;
static constexpr int I2S_DOUT_PIN = 2;
#endif
// -- Buffers
static int32_t* i2s_32bit_buffer = nullptr;
static int16_t* i2s_16bit_buffer = nullptr;
int16_t* i2s_16bit_network_buffer = nullptr;
uint8_t* rtpPacketScratch = nullptr;
static i2s_chan_handle_t i2s_rx_handle = nullptr;
RingbufHandle_t audioRingBuffer = nullptr;
bool audioRingInPsram = false;
static TaskHandle_t audioProducerTaskHandle = nullptr;
volatile bool audioProducerStopRequested = false;
volatile bool audioProducerRunning = false;
volatile uint32_t audioProducerLastReadMs = 0;
size_t audioRingBufferCapacityBytes = 0;

MicrophoneSignal microphoneSignal;

// -- Audio metering / clipping diagnostics
uint16_t lastPeakAbs16 = 0;       // last block peak absolute value (0..32767)
uint32_t audioClipCount = 0;      // total blocks where clipping occurred
bool audioClippedLastBlock = false; // clipping occurred in last processed block
uint16_t peakHoldAbs16 = 0;       // peak hold (recent window)
static unsigned long peakHoldUntilMs = 0; // when to clear hold

// The audio producer owns all DSP state. HTTP handlers only publish settings.
bool highpassEnabled = DEFAULT_HPF_ENABLED;
uint16_t highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
static fxaudio::BiquadQ28 hpf{};
static bool hpfConfigEnabled = false;
static uint32_t hpfConfigSampleRate = 0;
static uint16_t hpfConfigCutoff = 0;

std::atomic<uint32_t> audioProcessingLoadSnapshot{0};
static DRAM_ATTR portMUX_TYPE audioDmaMux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR uint32_t audioDmaOverrunTotal = 0; // ISR-only writer, protected by audioDmaMux
static DRAM_ATTR bool audioDmaCountingEnabled = false;
static bool audioDmaCallbackAvailable = false;
static RecentDmaOverruns audioDmaWindow;


bool otaInProgress = false;
bool pdmMicSupported() {
#if SOC_I2S_SUPPORTS_PDM_RX
    return true;
#else
    return false;
#endif
}

const char* pdmDecoderName() {
#if SOC_I2S_SUPPORTS_PDM2PCM
    return "hardware";
#elif MIC_PDM_SOFTWARE
    return "software";
#else
    return "unavailable";
#endif
}

uint8_t pdmRawWordStride() {
#if MIC_PDM_SOFTWARE
    return pdmDetectedStride.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

const char* pdmRawBitOrder() {
#if MIC_PDM_SOFTWARE
    uint8_t order = pdmDetectedOrder.load(std::memory_order_relaxed);
    return order == pdmsw::ORDER_MSB_FIRST ? "msb" : order == pdmsw::ORDER_LSB_FIRST ? "lsb" : "detecting";
#else
    return "hardware";
#endif
}

static void releasePdmCapture() {
#if MIC_PDM_SOFTWARE
    free(pdmDecimator); pdmDecimator = nullptr;
    free(pdmReadBuffers); pdmReadBuffers = nullptr;
    pdmFramer.reset();
    pdmWordRate.reset();
    pdmReadChunks = 0;
    pdmDetectedStride.store(0, std::memory_order_relaxed);
    pdmStrideLogPending.store(0, std::memory_order_relaxed);
    pdmDetectedOrder.store(0, std::memory_order_relaxed);
#endif
}

void updateHighpassCoeffs() {
    // A later HTTP edit must be noticed on the next block, even if it arrives
    // while coefficients are being computed on the other core.
    const bool enabled = highpassEnabled;
    const uint32_t rate = currentSampleRate;
    const uint16_t cutoff = normalizeHighpassCutoff(rate, highpassCutoffHz);
    if (enabled) {
        fxaudio::biquadSetHighpass(&hpf, rate, cutoff);
    } else {
        fxaudio::biquadReset(&hpf);
    }
    hpfConfigEnabled = enabled;
    hpfConfigSampleRate = rate;
    hpfConfigCutoff = cutoff;
}

static inline void* audioWorkAlloc(size_t bytes) {
#if defined(CONFIG_IDF_TARGET_ESP32C5) && CONFIG_SPIRAM
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

bool restartI2S() {
    if (recording::busy()) return false;
    lastI2SReset = millis();
    simplePrintln("Restarting I2S with new parameters...");
    stopAllRtspClients("I2S restart");
    stopAudioProducer();
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
    }

    if (i2s_32bit_buffer) { free(i2s_32bit_buffer); i2s_32bit_buffer = nullptr; }
    if (i2s_16bit_buffer) { free(i2s_16bit_buffer); i2s_16bit_buffer = nullptr; }
    if (i2s_16bit_network_buffer) { free(i2s_16bit_network_buffer); i2s_16bit_network_buffer = nullptr; }
    if (rtpPacketScratch) { free(rtpPacketScratch); rtpPacketScratch = nullptr; }

    i2s_32bit_buffer = (int32_t*)audioWorkAlloc(currentBufferSize * sizeof(int32_t));
    i2s_16bit_buffer = (int16_t*)audioWorkAlloc(currentBufferSize * sizeof(int16_t));
    i2s_16bit_network_buffer = (int16_t*)audioWorkAlloc(currentBufferSize * sizeof(int16_t));
    rtpPacketScratch = (uint8_t*)audioWorkAlloc(16 + (size_t)currentBufferSize * sizeof(int16_t));
    if (!i2s_32bit_buffer || !i2s_16bit_buffer || !i2s_16bit_network_buffer || !rtpPacketScratch) {
        simplePrintln("FATAL: Memory allocation failed after parameter change!");
        if (i2s_32bit_buffer) { free(i2s_32bit_buffer); i2s_32bit_buffer = nullptr; }
        if (i2s_16bit_buffer) { free(i2s_16bit_buffer); i2s_16bit_buffer = nullptr; }
        if (i2s_16bit_network_buffer) { free(i2s_16bit_network_buffer); i2s_16bit_network_buffer = nullptr; }
        if (rtpPacketScratch) { free(rtpPacketScratch); rtpPacketScratch = nullptr; }
        return false;
    }

    if (!setup_i2s_driver()) {
        simplePrintln("FATAL: I2S restart failed!");
        return false;
    }
    maxPacketRate = 0;
    minPacketRate = 0xFFFFFFFF;
    simplePrintln("I2S restarted successfully");
    return true;
}

static size_t computeAudioRingBufferCapacity() {
    size_t chunkBytes = (size_t)currentBufferSize * sizeof(int16_t);
    size_t chunkCount = 8;
    if (currentBufferSize > 4096) chunkCount = 3;
    else if (currentBufferSize > 2048) chunkCount = 4;
    size_t capacity = chunkBytes * chunkCount;
    // Keep RTP packets small for UDP compatibility while providing roughly
    // 64 chunks (~683 ms at 48 kHz/512 samples) of internal TCP jitter reserve.
    // Extra reserve is drained on recovery, rather than adding a fixed delay.
    if (currentBufferSize <= 2048 && capacity < 65536) capacity = 65536;
    if (capacity < 4096) capacity = 4096;
    return capacity;
}

void flushAudioRingBuffer() {
    if (!audioRingBuffer) return;
    size_t itemSize = 0;
    void* item = nullptr;
    uint32_t flushed = 0;
    while ((item = xRingbufferReceive(audioRingBuffer, &itemSize, 0)) != nullptr) {
        vRingbufferReturnItem(audioRingBuffer, item);
        flushed++;
    }
    if (flushed > 0) {
        audioRingBufferFlushCount += flushed;
    }
}

// Keep the overflow ISR bounded: no allocation, logging, clock calls or DSP.
static bool IRAM_ATTR onI2sDmaOverrun(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    portENTER_CRITICAL_ISR(&audioDmaMux);
    if (audioDmaCountingEnabled) ++audioDmaOverrunTotal;
    portEXIT_CRITICAL_ISR(&audioDmaMux);
    return false;
}

static void setI2sDmaCounting(bool enabled) {
    portENTER_CRITICAL(&audioDmaMux);
    audioDmaCountingEnabled = enabled && !audioProducerStopRequested;
    portEXIT_CRITICAL(&audioDmaMux);
}

uint32_t getI2sDmaOverrunTotal() {
    portENTER_CRITICAL(&audioDmaMux);
    uint32_t total = audioDmaOverrunTotal;
    portEXIT_CRITICAL(&audioDmaMux);
    return total;
}

static void registerI2sDiagnostics() {
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = onI2sDmaOverrun;
    esp_err_t result = i2s_channel_register_event_callback(i2s_rx_handle, &callbacks, nullptr);
    audioDmaCallbackAvailable = (result == ESP_OK);
    if (!audioDmaCallbackAvailable) {
        simplePrintln("I2S DMA diagnostics unavailable: " + String(esp_err_to_name(result)));
    }
}

static esp_err_t enableI2sReceiverWithDiagnostics() {
    registerI2sDiagnostics();
    esp_err_t result = i2s_channel_enable(i2s_rx_handle);
    if (result != ESP_OK) audioDmaCallbackAvailable = false;
    return result;
}

DmaOverrunStatus getI2sDmaStatus() {
    return {audioDmaCallbackAvailable, getI2sDmaOverrunTotal(), audioDmaWindow.count(),
            audioDmaWindow.hasEvent(), audioDmaWindow.lastEventMs()};
}

void checkMicrophoneSignal() {
    uint32_t now = millis();
    static MicrophoneSignal::State previousSignal = MicrophoneSignal::Unavailable;
    auto signal = microphoneSignal.state(now, audioProducerRunning && !audioProducerStopRequested);
    if (signal != previousSignal) {
        if (signal == MicrophoneSignal::Flatline)
            simplePrintln("Microphone signal unchanged for 20 s; check microphone wiring/power.");
        else if (signal == MicrophoneSignal::Healthy && previousSignal == MicrophoneSignal::Flatline)
            simplePrintln("Microphone signal restored.");
        previousSignal = signal;
        mqttPublishState(true);
    }
}

void updateAudioDiagnostics() {
#if MIC_PDM_SOFTWARE
    uint8_t stride = pdmStrideLogPending.exchange(0, std::memory_order_relaxed);
    if (stride) simplePrintln(stride == 2 ? "PDM raw rate: paired slots; using every second word."
                                        : "PDM raw rate: plain stream; using every word.");
#endif
    uint32_t now = millis();
    uint32_t added = audioDmaWindow.update(now, getI2sDmaOverrunTotal());
    static bool logged = false;
    static uint32_t lastLogMs = 0;
    if (added && (!logged || uint32_t(now - lastLogMs) >= 60000)) {
        logged = true;
        lastLogMs = now;
        simplePrintln("I2S DMA overrun: " + String(audioDmaWindow.count()) +
                      " observed in the last 60 s; capture could not keep up.");
    }
}

#if MIC_PDM_SOFTWARE
static int readPdmSoftwareBlock(uint64_t& filterUs) {
    int produced = 0;
    while (produced < (int)currentBufferSize && !audioProducerStopRequested) {
        size_t bytesRead = 0;
        size_t want = size_t(currentBufferSize - produced) * 8 * (pdmFramer.stride == 1 ? 1 : 2);
        if (want > sizeof(pdmReadBuffers->bytes)) want = sizeof(pdmReadBuffers->bytes);
        esp_err_t err = i2s_channel_read(i2s_rx_handle, pdmReadBuffers->bytes, want, &bytesRead, 100);
        if (audioProducerStopRequested) break;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) { audioI2SErrorCount++; recording::captureFault(); break; }
        if (!bytesRead) { audioI2SErrorCount++; recording::captureFault(); break; }
        audioProducerLastReadMs = millis();
        uint64_t started = esp_timer_get_time();
        size_t words = pdmFramer.unpack(pdmReadBuffers->bytes, bytesRead, pdmReadBuffers->words);
        uint8_t detected = pdmWordRate.observe(words, started,
            PDM_SAMPLE_RATE * pdmsw::kWordsPerSample, getI2sDmaOverrunTotal());
        if (detected) {
            if (pdmDetectedStride.exchange(detected, std::memory_order_relaxed) == 0)
                pdmStrideLogPending.store(detected, std::memory_order_relaxed);
            if (detected != pdmFramer.stride) {
                pdmFramer.stride = detected;
                pdmFramer.secondOfPair = false;
                pdmReadChunks = 0;
                pdmDetectedOrder.store(0, std::memory_order_relaxed);
                pdmsw::init(pdmDecimator, false);
                produced = 0; // discard samples decoded with the provisional stride
            }
        }
        size_t compacted = pdmFramer.compact(pdmReadBuffers->words, words);
        ++pdmReadChunks;
        if (detected && pdmDetectedOrder.load(std::memory_order_relaxed) == 0 &&
            pdmReadChunks >= 4 && (pdmReadChunks - 4) % 32 == 0 && compacted >= 256) {
            auto order = pdmsw::detectBitOrder(pdmDecimator, pdmReadBuffers->words, compacted);
            pdmDetectedOrder.store(uint8_t(order), std::memory_order_relaxed);
        }
        produced += int(pdmsw::process(pdmDecimator, pdmReadBuffers->words, compacted,
                                       i2s_32bit_buffer + produced, currentBufferSize - produced));
        filterUs += esp_timer_get_time() - started;
        if (err == ESP_ERR_TIMEOUT) break;
    }
    return produced;
}
#endif

void audioProducerTask(void* /*arg*/) {
    audioProducerRunning = true;
    setI2sDmaCounting(true);
    AudioProcessingWindow processingLoad;
    processingLoad.reset(esp_timer_get_time());

    const bool pdm = micFormat == MIC_FORMAT_PDM;
    const size_t sampleBytes = pdm && MIC_PDM_HARDWARE ? sizeof(int16_t) : sizeof(int32_t);
    while (!audioProducerStopRequested) {
        uint64_t filterUs = 0;
        int samplesRead = 0;
#if MIC_PDM_SOFTWARE
        if (pdm) samplesRead = readPdmSoftwareBlock(filterUs);
        else
#endif
        {
            size_t bytesRead = 0;
            esp_err_t result = i2s_channel_read(i2s_rx_handle, i2s_32bit_buffer,
                                                currentBufferSize * sampleBytes, &bytesRead, 100);
            if (audioProducerStopRequested) break;
            if (result != ESP_OK && result != ESP_ERR_TIMEOUT) { audioI2SErrorCount++; recording::captureFault(); continue; }
            samplesRead = bytesRead / sampleBytes;
            if (samplesRead == 0) { audioI2SErrorCount++; recording::captureFault(); vTaskDelay(pdMS_TO_TICKS(1)); continue; }
            audioProducerLastReadMs = millis();
        }
        if (audioProducerStopRequested) break;
        if (samplesRead <= 0) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        uint64_t processingStartedUs = esp_timer_get_time();

        // Hardware PDM is packed int16. Widen backwards without overwriting
        // unread samples; both PDM paths then share the same fixed PCM scale.
        if (pdm && MIC_PDM_HARDWARE) {
            const uint8_t* packed = (const uint8_t*)i2s_32bit_buffer;
            for (int i = samplesRead - 1; i >= 0; --i) {
                int16_t value;
                memcpy(&value, packed + size_t(i) * sizeof(value), sizeof(value));
                i2s_32bit_buffer[i] = int32_t(value) * 65536;
            }
        }
        if (hpfConfigEnabled != highpassEnabled || hpfConfigSampleRate != currentSampleRate ||
            hpfConfigCutoff != highpassCutoffHz) updateHighpassCoeffs();

        int32_t rawMinimum = INT32_MAX, rawMaximum = INT32_MIN;
        for (int i = 0; i < samplesRead; ++i) {
            int32_t raw = i2s_32bit_buffer[i];
            if (raw < rawMinimum) rawMinimum = raw;
            if (raw > rawMaximum) rawMaximum = raw;
        }
        bool clipped = false;
        // PDM retains its defined int16 PCM scale; I2S Shift stays I2S-only.
        lastPeakAbs16 = fxaudio::processBlock(i2s_32bit_buffer, i2s_16bit_buffer, samplesRead,
            pdm ? 16 : i2sShiftBits, hpfConfigEnabled ? &hpf : nullptr,
            fxaudio::gainToQ(currentGainFactor), &clipped);
        microphoneSignal.observe(rawMinimum, rawMaximum, millis());
        recording::push(i2s_16bit_buffer, samplesRead);
        browserAudioPush(i2s_16bit_buffer, samplesRead);
        audioClippedLastBlock = clipped;
        if (clipped) audioClipCount++;

        if (lastPeakAbs16 > peakHoldAbs16) {
            peakHoldAbs16 = lastPeakAbs16;
            peakHoldUntilMs = millis() + 3000UL;
        } else if (peakHoldAbs16 > 0 && millis() > peakHoldUntilMs) {
            peakHoldAbs16 = 0;
        }

        if (audioRingBuffer && hasActiveRtspStream()) {
            size_t payloadBytes = (size_t)samplesRead * sizeof(int16_t);
            if (xRingbufferSend(audioRingBuffer, i2s_16bit_buffer, payloadBytes, 0) == pdTRUE) {
                audioRingBufferChunkCount++;
            } else {
                // Capture remains the only writer; keep the existing atomic counter.
                audioRingBufferDropCount.store(
                    audioRingBufferDropCount.load(std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
            }
        }
        uint64_t processingFinishedUs = esp_timer_get_time();
        if (processingLoad.add(processingFinishedUs, processingFinishedUs - processingStartedUs + filterUs)) {
            audioProcessingLoadSnapshot.store(processingLoad.snapshot(), std::memory_order_relaxed);
        }
    }

    setI2sDmaCounting(false);
    audioProcessingLoadSnapshot.store(0, std::memory_order_relaxed);
    audioProducerRunning = false;
    audioProducerTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

static void deleteAudioRingBuffer() {
    if (!audioRingBuffer) return;
#if AUDIO_RING_IN_PSRAM
    vRingbufferDeleteWithCaps(audioRingBuffer);
#else
    vRingbufferDelete(audioRingBuffer);
#endif
    audioRingBuffer = nullptr;
    audioRingInPsram = false;
}

#if AUDIO_RING_IN_PSRAM
// Leave 64 KB of PSRAM for other large allocations; otherwise use internal RAM.
static bool createPsramAudioRing(size_t requested) {
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    // The ring itself goes to PSRAM, so only Wi-Fi headroom and the producer
    // stack are needed internally. Use the C5 restart allowance (64 KB) here
    // too: a client disconnected for the restart may still hold TCP buffers.
    if (!audioreserve::externalHeadroom(heap_caps_get_free_size(internal),
                                       heap_caps_get_largest_free_block(internal),
                                       audioreserve::kC5MinimumHeapMargin)) return false;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < requested + 64 * 1024) return false;
    audioRingBuffer = xRingbufferCreateWithCaps(requested, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audioRingBuffer) return false;
    audioRingInPsram = true;
    audioRingBufferCapacityBytes = requested;
    simplePrintln("Audio reserve: " + String((uint32_t)requested) + " bytes in PSRAM; internal free " +
                  String((uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) +
                  " bytes; PSRAM free " + String((uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024)) + " KB");
    return true;
}
#endif

void stopAudioProducer() {
    browserAudioStop();
    audioProducerStopRequested = true;
    setI2sDmaCounting(false);
    unsigned long startMs = millis();
    while (audioProducerTaskHandle != nullptr && (millis() - startMs) < 600UL) {
        delay(5);
    }
    if (audioProducerTaskHandle != nullptr) {
        vTaskDelete(audioProducerTaskHandle);
        audioProducerTaskHandle = nullptr;
        audioProducerRunning = false;
    }
    deleteAudioRingBuffer();
    audioRingBufferCapacityBytes = 0;
    audioRingBufferFlushPending = false;
    audioProcessingLoadSnapshot.store(0, std::memory_order_relaxed);
    microphoneSignal.reset();
    audioProducerStopRequested = false;
}

// Stop capture and its clocks as well as network listeners. Leaving the PDM
// converter running prevents cooling and competes with flash writes during OTA.
void pauseAudioCapture() {
    recording::requestStop();
    stopAudioProducer();
    recording::stop();
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
    }
    audioDmaCallbackAvailable = false;
    releasePdmCapture();
}

static bool otaRestoreRtsp = false;

void beginOtaUpdate() {
    if (otaInProgress) return;
    otaRestoreRtsp = rtspServerEnabled;
    otaInProgress = true;
    stopAllRtspClients("OTA update starting");
    rtspServerEnabled = false;
    rtspServer.stop();
    pauseAudioCapture();
    overheatConsecutiveSamples = 0;
    simplePrintln("OTA: audio capture paused until reboot or update failure.");
}

void endOtaUpdate(bool installed) {
    if (!otaInProgress || installed) return; // Successful OTA stays paused until reboot.
    otaInProgress = false;
    if (overheatLatched) return;
    if (!setup_i2s_driver()) {
        simplePrintln("OTA failed: audio restart failed; reboot required.");
        return;
    }
    if (otaRestoreRtsp && !recording::mode() && isStreamScheduleAllowedNow()) {
        rtspServer.begin();
        rtspServer.setNoDelay(true);
        rtspServerEnabled = true;
    }
    simplePrintln("OTA failed: audio capture restored.");
}

static RingbufHandle_t createInternalAudioRing(size_t bytes) {
#if AUDIO_RING_IN_PSRAM
    return xRingbufferCreateWithCaps(bytes, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xRingbufferCreate(bytes, RINGBUF_TYPE_BYTEBUF);
#endif
}

bool startAudioProducer() {
    stopAudioProducer();

    if (!recording::mode()) {
    const size_t requested = computeAudioRingBufferCapacity();
#if AUDIO_RING_IN_PSRAM
    // PSRAM first; the unchanged internal-RAM reserve path below is the fallback.
    if (!createPsramAudioRing(requested)) {
        simplePrintln("Audio reserve: PSRAM unavailable, using internal RAM.");
#endif
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t heapFree = heap_caps_get_free_size(caps);
    const size_t heapLargest = heap_caps_get_largest_free_block(caps);
    const size_t chunkBytes = size_t(currentBufferSize) * sizeof(int16_t);
    audioRingBufferCapacityBytes = audioreserve::fit(requested,
        chunkBytes, heapFree, heapLargest);
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (!audioRingBufferCapacityBytes) {
        audioRingBufferCapacityBytes = audioreserve::fit(
            audioreserve::minimumCapacity(requested, chunkBytes), chunkBytes,
            heapFree, heapLargest, audioreserve::kC5MinimumHeapMargin);
        if (audioRingBufferCapacityBytes) {
            simplePrintln("Audio reserve: low RAM, using minimum reserve with 64 KiB Wi-Fi headroom.");
        }
    }
#endif
    simplePrintln("Audio reserve check: free " + String((uint32_t)heapFree) +
                  " bytes; largest block " + String((uint32_t)heapLargest));
    if (!audioRingBufferCapacityBytes) {
        simplePrintln("Audio reserve: insufficient RAM for Wi-Fi and capture.");
        return false;
    }
    audioRingBuffer = createInternalAudioRing(audioRingBufferCapacityBytes);
    if (!audioRingBuffer) {
        simplePrintln("Audio RB alloc failed");
        audioRingBufferCapacityBytes = 0;
        return false;
    }

    simplePrintln("Audio reserve: " + String((uint32_t)audioRingBufferCapacityBytes) +
                  " bytes; requested " + String((uint32_t)requested));
#if AUDIO_RING_IN_PSRAM
    }
#endif
    } else {
        // Recording sends PCM directly to the SD queue and independent Listen
        // queue. An unused RTSP ring would consume up to 64 KiB on C3/C6.
        const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        if (!audioreserve::externalHeadroom(heap_caps_get_free_size(caps),
                                           heap_caps_get_largest_free_block(caps),
                                           recordingmemory::kWorkingReserveBytes)) {
            simplePrintln("Recording capture: insufficient internal RAM for Wi-Fi and producer.");
            return false;
        }
    }
    updateHighpassCoeffs(); // producer is stopped here
    audioProducerStopRequested = false;
    audioProducerLastReadMs = millis();
    BaseType_t rc = xTaskCreate(
        audioProducerTask,
        "audio_prod",
        audioreserve::kProducerStackBytes,
        nullptr,
        5,
        &audioProducerTaskHandle
    );
    if (rc != pdPASS) {
        simplePrintln("Audio producer failed");
        deleteAudioRingBuffer();
        audioRingBufferCapacityBytes = 0;
        return false;
    }

    simplePrintln("Audio producer started");
    return true;
}

// I2S setup
bool setup_i2s_driver() {
    stopAudioProducer();
    audioDmaCallbackAvailable = false;
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
    }

    releasePdmCapture();
    // A persisted thermal shutdown must boot with capture stopped so it can
    // actually cool. Settings changes must not bypass either pause condition.
    if (overheatLatched || otaInProgress) return true;
    if (micFormat == MIC_FORMAT_PDM && (!pdmMicSupported() || currentSampleRate != PDM_SAMPLE_RATE)) return false;
    uint16_t dma_buf_len = (currentBufferSize > 512) ? 512 : currentBufferSize;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = dma_buf_len;
#if MIC_PDM_SOFTWARE
    if (micFormat == MIC_FORMAT_PDM) chan_cfg.dma_frame_num = 1024;
#endif

    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &i2s_rx_handle);
    if (err != ESP_OK) {
        simplePrintln("I2S channel create failed: " + String(esp_err_to_name(err)));
        return false;
    }

    if (micFormat == MIC_FORMAT_PDM) {
#if SOC_I2S_SUPPORTS_PDM_RX
        i2s_pdm_rx_config_t config = {};
#if SOC_I2S_SUPPORTS_PDM2PCM
        config.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_SAMPLE_RATE);
        config.clk_cfg.dn_sample_mode = I2S_PDM_DSR_8S;
        config.slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#else
        // Both are just over 4 KB; with PSRAM enabled, malloc() prefers PSRAM above
        // 4 KB. The filter reads them for every PDM word, so keep them internal.
        pdmDecimator = (pdmsw::Decimator*)audioWorkAlloc(sizeof(pdmsw::Decimator));
        pdmReadBuffers = (PdmReadBuffers*)audioWorkAlloc(sizeof(PdmReadBuffers));
        if (!pdmDecimator || !pdmReadBuffers) {
            releasePdmCapture();
            i2s_del_channel(i2s_rx_handle); i2s_rx_handle = nullptr;
            return false;
        }
        pdmsw::init(pdmDecimator, false);
        config.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_CLOCK_HZ);
        config.slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#endif
        config.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
        config.gpio_cfg.clk = (gpio_num_t)s3boards::clockPin(s3BoardVariant, I2S_BCLK_PIN);
        config.gpio_cfg.din = (gpio_num_t)s3boards::dataPin(s3BoardVariant, I2S_DOUT_PIN);
        err = i2s_channel_init_pdm_rx_mode(i2s_rx_handle, &config);
        if (err == ESP_OK) err = enableI2sReceiverWithDiagnostics();
        if (err == ESP_OK && startAudioProducer()) {
#if MIC_PDM_SOFTWARE
            simplePrintln(String("PDM filter buffers: ") +
                          (esp_ptr_external_ram(pdmDecimator) || esp_ptr_external_ram(pdmReadBuffers) ? "PSRAM" : "internal RAM"));
#endif
            simplePrintln(String("PDM ready: 48000 Hz, ") + pdmDecoderName() +
                          ", CLK=GPIO" + String(s3boards::clockPin(s3BoardVariant, I2S_BCLK_PIN)) +
                          ", DATA=GPIO" + String(s3boards::dataPin(s3BoardVariant, I2S_DOUT_PIN)) +
                          "; WS/MCLK unused.");
            return true;
        }
        audioDmaCallbackAvailable = false;
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle); i2s_rx_handle = nullptr;
        releasePdmCapture();
#endif
        simplePrintln("PDM setup failed; check configuration/memory.");
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(currentSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_PIN,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws = (gpio_num_t)I2S_LRCLK_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)I2S_DOUT_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // PCM1808 slave mode accepts 256/384/512 fs. Mics without an MCLK input
    // leave D7 unconnected and continue to use the same BCLK/WS/SD wiring.
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    if (micFormat == MIC_FORMAT_MSB) {
        std_cfg.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    }

    err = i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg);
    if (err != ESP_OK) {
        simplePrintln("I2S pin/mode setup failed: " + String(esp_err_to_name(err)));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }

    err = enableI2sReceiverWithDiagnostics();
    if (err != ESP_OK) {
        audioDmaCallbackAvailable = false;
        simplePrintln("I2S channel enable failed: " + String(esp_err_to_name(err)));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }

    simplePrintln("I2S ready: " + String(currentSampleRate) + "Hz, gain " +
                  String(currentGainFactor, 1) + ", buffer " + String(currentBufferSize) +
                  ", shiftBits " + String(i2sShiftBits) +
                  ", micFormat " + String(micFormatName(micFormat)) +
                  ", pins MCLK/BCLK/WS/SD " +
                  String(I2S_MCLK_PIN) + "/" + String(I2S_BCLK_PIN) + "/" +
                  String(I2S_LRCLK_PIN) + "/" + String(I2S_DOUT_PIN));
    if (!startAudioProducer()) {
        audioDmaCallbackAvailable = false;
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }
    return true;
}


bool allocateAudioBuffers() {
    i2s_32bit_buffer = (int32_t*)audioWorkAlloc(currentBufferSize * sizeof(int32_t));
    i2s_16bit_buffer = (int16_t*)audioWorkAlloc(currentBufferSize * sizeof(int16_t));
    i2s_16bit_network_buffer = (int16_t*)audioWorkAlloc(currentBufferSize * sizeof(int16_t));
    rtpPacketScratch = (uint8_t*)audioWorkAlloc(16 + (size_t)currentBufferSize * sizeof(int16_t));
    return i2s_32bit_buffer && i2s_16bit_buffer && i2s_16bit_network_buffer && rtpPacketScratch;
}
void resetAudioDmaWindow() { audioDmaWindow.reset(millis(), getI2sDmaOverrunTotal()); }
