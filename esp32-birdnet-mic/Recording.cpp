#include "Recording.h"
#include "RecordingWav.h"
#include <Preferences.h>
#include <atomic>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#if defined(ARDUINO_XIAO_ESP32S3)
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#endif

namespace recording {
namespace {
Settings config;
Status view;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> accepting{false}, running{false}, starting{false}, stopRequested{false}, captureFailed{false};
std::atomic<uint32_t> inPush{0}, lost{0};
RingbufHandle_t queue = nullptr;
uint32_t startRate = 48000;
uint64_t acceptedFrames = 0, targetFrames = 0;
void error(const char* text) {
    portENTER_CRITICAL(&mux);
    if (!view.error[0]) strncpy(view.error, text, sizeof(view.error)-1);
    view.state = State::Error;
    portEXIT_CRITICAL(&mux);
}
void state(State value) {
    portENTER_CRITICAL(&mux); view.state = value; portEXIT_CRITICAL(&mux);
}
#if defined(ARDUINO_XIAO_ESP32S3)
TaskHandle_t storageTask = nullptr;
// Only this worker touches the card or its file descriptors. POSIX fsync is
// checked explicitly; Arduino File::flush() cannot report a failure.
struct Sink {
    int fd = -1;
    size_t write(const uint8_t* data, size_t count) {
        ssize_t n = ::write(fd, data, count);
        return n < 0 ? 0 : size_t(n);
    }
    bool seek(uint32_t at) { return lseek(fd, at, SEEK_SET) == off_t(at); }
    bool sync() { return fsync(fd) == 0; }
};
Sink sink;
WavWriter<Sink> wav(sink);
char partial[128], finalName[112];
uint64_t sessionBytes = 0;
uint32_t segmentBytes = 0, segmentIndex = 0;
uint32_t sessionId = 0;
uint64_t sessionStartUs = 0;
bool healthy = true;

void updateSpace() {
    uint64_t total = SD.totalBytes(), used = SD.usedBytes();
    portENTER_CRITICAL(&mux);
    view.freeMiB = total > used ? uint32_t((total-used)/1048576) : 0;
    portEXIT_CRITICAL(&mux);
}
bool openSegment() {
    // Random boot/session identity plus O_EXCL prevents overwriting an existing
    // recording, including when the wall clock is unavailable or goes backwards.
    snprintf(finalName, sizeof(finalName), "/sd/recordings/%08lx-%04lu.wav",
             (unsigned long)sessionId, (unsigned long)++segmentIndex);
    snprintf(partial, sizeof(partial), "%s.part", finalName);
    if (access(finalName, F_OK) == 0) { error("Recording filename collision; existing file preserved"); return false; }
    sink.fd = open(partial, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (sink.fd < 0) { error("SD open failed (existing files preserved)"); return false; }
    portENTER_CRITICAL(&mux);
    strncpy(view.file, partial+3, sizeof(view.file)-1);
    portEXIT_CRITICAL(&mux);
    if (!wav.begin(startRate)) { error("WAV header write failed"); return false; }
    return true;
}
bool closeSegment(bool finalize) {
    if (sink.fd < 0) return false;
    bool ok = finalize && wav.checkpoint();
    if (::close(sink.fd) != 0) ok = false;
    sink.fd = -1;
    if (ok && ::rename(partial, finalName) != 0) ok = false;
    if (ok) {
        portENTER_CRITICAL(&mux);
        strncpy(view.file, finalName+3, sizeof(view.file)-1);
        portEXIT_CRITICAL(&mux);
    } else error("SD save failed; partial file preserved");
    return ok;
}
void discardQueue() {
    size_t bytes = 0;
    while (void* item = xRingbufferReceive(queue, &bytes, 0)) vRingbufferReturnItem(queue, item);
}
void worker(void*) {
    for (;;) {
        if (!starting.load()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        // start() prepares the immutable session configuration before starting.
        sessionBytes = 0; segmentIndex = 0; sessionId = esp_random();
        segmentBytes = uint32_t(uint64_t(startRate)*2*config.segmentMinutes*60);
        healthy = true;
        discardQueue();
        SD.end();
        SPI.begin(D8, D9, D10, D6); // S3: GPIO7/8/9; CS=TX GPIO43, BFF default
        if (!SD.begin(D6, SPI, 4000000, "/sd", 5, false) || SD.cardType() == CARD_NONE) {
            error("SD unavailable; check card, power and wiring"); healthy = false;
        } else {
            portENTER_CRITICAL(&mux); view.cardReady = true; portEXIT_CRITICAL(&mux);
            updateSpace();
            if (!SD.exists("/recordings") && !SD.mkdir("/recordings")) {
                error("Cannot create recordings directory"); healthy = false;
            }
            if (healthy) healthy = openSegment();
        }
        sessionStartUs = esp_timer_get_time();
        // A stop during SD mount must cancel this start too.
        if (healthy && !stopRequested.load()) {
            state(State::Recording);
            accepting.store(true);
            if (stopRequested.load()) accepting.store(false);
        }
        starting.store(false);
        uint32_t checkpointAt = millis();
        while (healthy) {
            size_t bytes = 0;
            uint8_t* item = static_cast<uint8_t*>(xRingbufferReceiveUpTo(queue, &bytes, pdMS_TO_TICKS(20), 4096));
            if (item) {
                size_t offset = 0;
                while (offset < bytes && healthy) {
                    if (wav.bytes() == segmentBytes) {
                        healthy = closeSegment(true) && openSegment();
                        if (!healthy) break;
                    }
                    size_t n = bytes-offset;
                    if (n > segmentBytes-wav.bytes()) n = segmentBytes-wav.bytes();
                    uint64_t before = esp_timer_get_time();
                    healthy = wav.append(item+offset, n);
                    uint32_t writeMs = uint32_t((esp_timer_get_time()-before)/1000);
                    if (!healthy) { error(errno == ENOSPC ? "SD full; partial file preserved" : "SD write failed; partial file preserved"); break; }
                    sessionBytes += n; offset += n;
                    portENTER_CRITICAL(&mux);
                    view.writtenKiB = uint32_t(sessionBytes/1024);
                    if (writeMs > view.maxWriteMs) view.maxWriteMs = writeMs;
                    portEXIT_CRITICAL(&mux);
                }
                vRingbufferReturnItem(queue, item);
            }
            if (captureFailed.load()) { error("Microphone capture interrupted; partial file preserved"); healthy = false; }
            if (lost.load()) { error("Audio buffer overflow; recording stopped with a gap"); healthy = false; }
            if (!healthy) break;
            if (millis()-checkpointAt >= 5000) {
                healthy = wav.checkpoint();
                if (!healthy) { error("SD checkpoint failed; partial file preserved"); break; }
                updateSpace(); checkpointAt = millis();
            }
            portENTER_CRITICAL(&mux);
            view.elapsedSeconds = uint32_t((esp_timer_get_time()-sessionStartUs)/1000000);
            portEXIT_CRITICAL(&mux);
            if (!item && !accepting.load() && !inPush.load()) break;
        }
        accepting.store(false);
        while (inPush.load()) vTaskDelay(1);
        // No writer/capture can reference an item after this drain.
        discardQueue();
        if (sink.fd >= 0) {
            bool ok = closeSegment(healthy);
            healthy = healthy && ok;
        }
        SD.end();
        SPI.end();
        if (healthy) state(State::Saved);
        running.store(false);
    }
}
bool prepare() {
    if (queue) return true;
    // Keep room for Wi-Fi, HTTP and the separate Listen queue.
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 104*1024) return false;
    queue = xRingbufferCreate(32768, RINGBUF_TYPE_BYTEBUF);
    if (!queue) return false;
    if (xTaskCreate(worker, "sd_storage", 6144, nullptr, 2, &storageTask) != pdPASS) {
        vRingbufferDelete(queue); queue = nullptr; return false;
    }
    return true;
}
#endif
}
void begin() {
    Preferences prefs;
    if (prefs.begin("recording", true)) {
        Settings saved;
        if (prefs.getBytesLength("settings") == sizeof(saved) &&
            prefs.getBytes("settings", &saved, sizeof(saved)) == sizeof(saved) &&
            saved.magic == config.magic && saved.recordingMode <= 1 && saved.autoStart <= 1 &&
            saved.durationMinutes <= 10080 && saved.segmentMinutes >= 1 && saved.segmentMinutes <= 120) config = saved;
        prefs.end();
    }
}
Settings settings() { return config; }
bool saveSettings(const Settings& value) {
    if (busy() || value.magic != config.magic || value.recordingMode > 1 || value.autoStart > 1 ||
        value.durationMinutes > 10080 || value.segmentMinutes < 1 || value.segmentMinutes > 120) return false;
    if (memcmp(&config, &value, sizeof(config)) == 0) return true;
    Preferences prefs;
    if (!prefs.begin("recording", false)) return false;
    bool ok = prefs.putBytes("settings", &value, sizeof(value)) == sizeof(value);
    prefs.end();
    if (ok) config = value;
    return ok;
}
bool mode() { return config.recordingMode; }
bool busy() { return starting.load() || running.load(); }
bool start(uint32_t rate, bool supported) {
    if (busy()) return false;
    if (!mode() || !supported) { error("SD recording currently supports base XIAO S3 only"); return false; }
#if defined(ARDUINO_XIAO_ESP32S3)
    if (rate < 8000 || rate > 96000) { error("Unsupported WAV sample rate"); return false; }
    if (!prepare()) { error("Not enough RAM for SD recording buffer"); return false; }
    portENTER_CRITICAL(&mux); view = Status{}; view.state = State::Starting; portEXIT_CRITICAL(&mux);
    startRate = rate; acceptedFrames = 0;
    targetFrames = uint64_t(config.durationMinutes)*60*rate;
    lost.store(0); captureFailed.store(false); stopRequested.store(false); running.store(true); starting.store(true);
    return true;
#else
    error("SD recording currently supports base XIAO S3 only"); return false;
#endif
}
void push(const int16_t* pcm, size_t samples) {
    if (!accepting.load()) return;
    inPush.fetch_add(1);
    if (accepting.load()) {
        if (targetFrames && samples > targetFrames-acceptedFrames) samples = size_t(targetFrames-acceptedFrames);
        if (samples && xRingbufferSend(queue, pcm, samples*sizeof(int16_t), 0) != pdTRUE) {
            lost.fetch_add(samples); accepting.store(false);
        } else acceptedFrames += samples;
        if (targetFrames && acceptedFrames >= targetFrames) accepting.store(false);
    }
    inPush.fetch_sub(1);
}
void captureFault() { captureFailed.store(true); accepting.store(false); }
void requestStop() {
    stopRequested.store(true); accepting.store(false);
    if (busy()) state(State::Saving);
}
bool stop(uint32_t timeoutMs) {
    requestStop();
    uint32_t at = millis();
    while (busy() || inPush.load()) {
        if (millis()-at >= timeoutMs) { error("SD stop timed out; wait before retrying"); return false; }
        delay(1);
    }
    return true;
}
Status status() {
    portENTER_CRITICAL(&mux); Status copy = view; portEXIT_CRITICAL(&mux);
    copy.droppedSamples = lost.load();
    return copy;
}
String statusJson(bool supported) {
    Status s = status();
    const char* states[] = {"ready", "starting", "recording", "saving", "saved", "error"};
    // Names and errors are generated internally and contain no JSON metacharacters.
    String out = "{\"mode\":\""; out += mode() ? "recording" : "streaming";
    out += "\",\"state\":\""; out += states[uint8_t(s.state)];
    out += "\",\"supported\":"; out += supported ? "true" : "false";
    out += ",\"busy\":"; out += busy() ? "true" : "false";
    out += ",\"card_ready\":"; out += s.cardReady ? "true" : "false";
    out += ",\"elapsed_seconds\":" + String(s.elapsedSeconds);
    out += ",\"duration_minutes\":" + String(config.durationMinutes);
    out += ",\"segment_minutes\":" + String(config.segmentMinutes);
    out += ",\"auto_start\":"; out += config.autoStart ? "true" : "false";
    out += ",\"free_mib\":" + String(s.freeMiB);
    out += ",\"written_kib\":" + String(s.writtenKiB);
    out += ",\"dropped_samples\":" + String(s.droppedSamples);
    out += ",\"max_write_ms\":" + String(s.maxWriteMs);
    out += ",\"file\":\"" + String(s.file) + "\",\"error\":\"" + String(s.error) + "\"}";
    return out;
}
}
