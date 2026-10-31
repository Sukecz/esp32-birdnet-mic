#include "Recording.h"
#include "RecordingWav.h"
#include "RecordingPins.h"
#include "RecordingMemory.h"
#include "BoundedLog.h"
#include "LogStorage.h"
#include <time.h>
#include <Preferences.h>
#include <atomic>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#if RECORDING_SPI_SUPPORTED
#include <SPI.h>
#include "SdJournal.h"
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
LogSettings journalConfig;
Status view;
BoundedLog<3072>* systemLogs=nullptr;
uint32_t systemLogDropped = 0;
std::atomic<bool> journalReadPending{false}, journalReadReady{false};
bool journalReadArchive = false;
char* journalReadData=nullptr;
char journalReadFailure[80] = {};
uint32_t journalReadBytes = 0;
bool journalWarningPending = false;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> accepting{false}, running{false}, starting{false}, stopRequested{false}, captureFailed{false};
std::atomic<bool> checkOnly{false}, coolingPending{false}, diagnosticOnly{false};
uint32_t storageClockHz = kDefaultStorageClockHz, storageWriteBlockBytes = kDefaultWriteBlockBytes;
uint64_t completedUs = 0, completedBytes = 0, remainingFrames = 0;
std::atomic<uint32_t> inPush{0}, lost{0}, queueCapacityBytes{0};
RingbufHandle_t queue = nullptr;
bool queueInPsram = false;
recordingpins::Pins sessionPins = recordingpins::resolve();
uint32_t startRate = 48000;
uint64_t acceptedFrames = 0, targetFrames = 0;
std::atomic<StopReason> requestedReason{StopReason::User};
// The SD worker must never write the WebUI String ring from another task.
// Fixed-size snapshots keep event production bounded; the main loop formats them.
enum class LogKind : uint8_t { StartRequested, ResumeRequested, Started, Opened, Saved, Progress, StopRequested, Finished, Error, Cooling, CardReady, DiagnosticRequested, DiagnosticFinished };
struct LogEvent {
    LogKind kind;
    Status status;
    uint32_t atMs, rate, durationMinutes, segmentMinutes, fileBytes;
    uint64_t totalBytes;
    bool success;
    uint8_t consumed;
    char reason[32];
};
constexpr uint8_t kLogCapacity = 8;
LogEvent logEvents[kLogCapacity];
uint8_t logHead = 0, logCount = 0;
uint32_t logDropped = 0, logDroppedSD = 0;
void logLocked(LogKind kind, uint32_t fileBytes = 0, uint64_t totalBytes = 0, const char* reason = "", bool success = true) {
    if (!journalConfig.detailed && kind != LogKind::Started && kind != LogKind::Saved &&
        kind != LogKind::Finished && kind != LogKind::Error && kind != LogKind::Cooling &&
        kind != LogKind::ResumeRequested && !(kind == LogKind::CardReady && view.state == State::Checking)) return;
    if (logCount == kLogCapacity) {
        if (!(logEvents[logHead].consumed&1)) ++logDropped;
        if (!(logEvents[logHead].consumed&2)) ++logDroppedSD;
        logHead = (logHead+1)%kLogCapacity; --logCount;
    }
    auto& e = logEvents[(logHead+logCount)%kLogCapacity];
    e.kind = kind; e.status = view; e.consumed = 0;
    e.status.droppedSamples = lost.load(); e.status.queueBytes = queueCapacityBytes.load();
    e.atMs = millis(); e.rate = startRate; e.durationMinutes = config.durationMinutes;
    e.segmentMinutes = config.segmentMinutes; e.fileBytes = fileBytes;
    e.totalBytes = totalBytes ? totalBytes : uint64_t(view.writtenKiB)*1024;
    e.success = success;
    strncpy(e.reason, reason, sizeof(e.reason)-1); e.reason[sizeof(e.reason)-1] = 0;
    ++logCount;
}
void logEvent(LogKind kind, uint32_t fileBytes = 0, uint64_t totalBytes = 0, const char* reason = "", bool success = true) {
    portENTER_CRITICAL(&mux);
    logLocked(kind, fileBytes, totalBytes, reason, success);
    portEXIT_CRITICAL(&mux);
}
bool takeEvent(LogEvent& event, uint8_t consumer) {
    bool found = false;
    portENTER_CRITICAL(&mux);
    for(uint8_t i=0;i<logCount;++i) {
        auto& candidate = logEvents[(logHead+i)%kLogCapacity];
        if (!(candidate.consumed&consumer)) { event=candidate;candidate.consumed|=consumer;found=true;break; }
    }
    while(logCount && logEvents[logHead].consumed==3) { logHead=(logHead+1)%kLogCapacity;--logCount; }
    portEXIT_CRITICAL(&mux);
    return found;
}
String formatEvent(const LogEvent& e);
void error(const char* text) {
    portENTER_CRITICAL(&mux);
    const bool first = !view.error[0];
    if (first) strncpy(view.error, text, sizeof(view.error)-1);
    view.state = State::Error;
    if (first) logLocked(LogKind::Error, 0, 0, "", false);
    portEXIT_CRITICAL(&mux);
}
void state(State value) {
    portENTER_CRITICAL(&mux); view.state = value; portEXIT_CRITICAL(&mux);
}
#if RECORDING_SPI_SUPPORTED
TaskHandle_t storageTask = nullptr;
const char* preparationError = "";
// Only this worker touches the card or its file descriptors. POSIX fsync is
// checked explicitly; Arduino File::flush() cannot report a failure.
void noteStorageDelay(uint64_t before);
void ioFailure(const char* operation, int code, uint32_t offset, uint32_t requested, int32_t returned) {
    portENTER_CRITICAL(&mux);
    if (!view.ioOperation[0]) {
        strncpy(view.ioOperation, operation, sizeof(view.ioOperation)-1);
        view.ioErrno = code; view.ioOffset = offset;
        view.ioRequested = requested; view.ioReturned = returned;
    }
    portEXIT_CRITICAL(&mux);
}
struct Sink {
    int fd = -1;
    uint32_t position = 0;
    size_t write(const uint8_t* data, size_t count) {
        const uint64_t before = esp_timer_get_time();
        size_t done = 0;
        while (done < count) {
            const size_t chunk = count-done < storageWriteBlockBytes ? count-done : storageWriteBlockBytes;
            errno = 0;
            const ssize_t n = ::write(fd, data+done, chunk);
            const int code = errno;
            if (n != ssize_t(chunk)) ioFailure("write", code, position, chunk, n);
            if (n > 0) { position += uint32_t(n); done += size_t(n); }
            // A short write may already have changed the medium. Preserve the
            // partial file and never blindly retry or mask the first failure.
            if (n != ssize_t(chunk)) break;
        }
        noteStorageDelay(before);
        return done;
    }
    bool seek(uint32_t at) {
        errno = 0; const off_t result = lseek(fd, at, SEEK_SET);
        if (result != off_t(at)) { ioFailure("seek", errno, position, at, result); return false; }
        position = at; return true;
    }
    bool sync() {
        errno = 0; const int result = fsync(fd);
        if (result) ioFailure("sync", errno, position, 0, result);
        return result == 0;
    }
};
Sink sink;
WavWriter<Sink> wav(sink);
char partial[128], finalName[112];
uint64_t sessionBytes = 0;
uint32_t segmentBytes = 0, segmentIndex = 0;
uint32_t sessionId = 0;
bool healthy = true;

SdJournal journal;
void journalFailed() {
    if (!journalConfig.sdEnabled || !journal.errorCode()) return;
    portENTER_CRITICAL(&mux);
    if (!view.logError[0]) {
        snprintf(view.logError,sizeof(view.logError),"SD log unavailable (errno %d)",journal.errorCode());
        journalWarningPending=true;
    }
    portEXIT_CRITICAL(&mux);
}
void openJournal() {
    journal.close();
    portENTER_CRITICAL(&mux);view.logError[0]=0;view.logBytes=0;portEXIT_CRITICAL(&mux);
    if (journalConfig.sdEnabled) { journal.begin("/sd/recording.log",journalConfig.maxKiB*1024);journalFailed(); }
}
void flushJournal() {
    char line[512];
    uint32_t droppedSystem, droppedEvents;
    portENTER_CRITICAL(&mux);droppedSystem=systemLogDropped;systemLogDropped=0;droppedEvents=logDroppedSD;logDroppedSD=0;portEXIT_CRITICAL(&mux);
    if ((droppedSystem || droppedEvents) && journal.active()) {
        snprintf(line,sizeof(line),"Log buffer overrun: %lu system lines / %lu recording events lost",(unsigned long)droppedSystem,(unsigned long)droppedEvents);
        journal.append(line);
    }
    for(;;) {
        portENTER_CRITICAL(&mux);const bool have=systemLogs && systemLogs->pop(line,sizeof(line));portEXIT_CRITICAL(&mux);
        if(!have)break;
        if(journal.active())journal.append(line);
    }
    LogEvent event;
    while(takeEvent(event,2)) {
        if (!journal.active()) continue;
        const time_t now=time(nullptr);
        struct tm local;char stamp[40];
        if(now>1672531200 && localtime_r(&now,&local))strftime(stamp,sizeof(stamp),"[%Y-%m-%d %H:%M:%S] ",&local);
        else snprintf(stamp,sizeof(stamp),"[uptime %lus] ",(unsigned long)(event.atMs/1000));
        const String text=String(stamp)+formatEvent(event);
        journal.append(text.c_str());
    }
    if(journal.active())journal.flush();
    journalFailed();
    portENTER_CRITICAL(&mux);view.logBytes=journal.size();portEXIT_CRITICAL(&mux);
}
void closeJournal() { flushJournal();journal.close();journalFailed(); }
void readJournalTail() {
    journalReadData[0]=journalReadFailure[0]=0;journalReadBytes=0;
    SPI.begin(sessionPins.sck,sessionPins.miso,sessionPins.mosi,sessionPins.cs);
    if(!SD.begin(sessionPins.cs,SPI,storageClockHz,"/sd",5,false)) {
        strcpy(journalReadFailure,"SD card is unavailable");
    } else {
        const char* file=journalReadArchive?"/sd/recording.log.1":"/sd/recording.log";
        const int fd=::open(file,O_RDONLY);
        if(fd<0) {
            if(errno==ENOENT)strcpy(journalReadFailure,"This SD log does not exist yet");
            else snprintf(journalReadFailure,sizeof(journalReadFailure),"SD log cannot be opened (errno %d)",errno);
        }
        else {
            const off_t end=lseek(fd,0,SEEK_END);
            const off_t at=end>2048?end-2048:0;
            if(end<0 || lseek(fd,at,SEEK_SET)!=at)strcpy(journalReadFailure,"SD log seek failed");
            else {
                const ssize_t got=::read(fd,journalReadData,2048);
                if(got<0)strcpy(journalReadFailure,"SD log read failed");
                else {journalReadData[got]=0;journalReadBytes=uint32_t(end);}
            }
            if(::close(fd) && !journalReadFailure[0])strcpy(journalReadFailure,"SD log close failed");
        }
    }
    SD.end();SPI.end();journalReadReady.store(true);
}
void noteStorageDelay(uint64_t before) {
    const uint32_t elapsed = uint32_t((esp_timer_get_time()-before)/1000);
    portENTER_CRITICAL(&mux);
    if (elapsed > view.maxWriteMs) view.maxWriteMs = elapsed;
    portEXIT_CRITICAL(&mux);
}
void updateSpace() {
    uint64_t total = SD.totalBytes(), used = SD.usedBytes();
    portENTER_CRITICAL(&mux);
    view.freeBytes = total > used ? total-used : 0;
    view.freeMiB = uint32_t(view.freeBytes/1048576);
    portEXIT_CRITICAL(&mux);
}
bool openSegment() {
    // Random boot/session identity plus O_EXCL prevents overwriting an existing
    // recording, including when the wall clock is unavailable or goes backwards.
    char stamp[32] = "unsynced";
    const time_t now = time(nullptr);
    struct tm local;
    if (now > 1672531200 && localtime_r(&now, &local)) {
        if (!strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local))
            strcpy(stamp, "unsynced");
    }
    snprintf(finalName, sizeof(finalName), "/sd/recordings/%s_%08lx-%04lu.wav",
             stamp, (unsigned long)sessionId, (unsigned long)++segmentIndex);
    snprintf(partial, sizeof(partial), "%s.part", finalName);
    if (access(finalName, F_OK) == 0) { error("Recording filename collision; existing file preserved"); return false; }
    sink.position = 0;
    sink.fd = open(partial, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (sink.fd < 0) { ioFailure("open", errno, 0, 0, -1); error("SD open failed (existing files preserved)"); return false; }
    portENTER_CRITICAL(&mux);
    strncpy(view.file, partial+3, sizeof(view.file)-1);
    view.verified = false; view.verifiedBytes = 0;
    portEXIT_CRITICAL(&mux);
    if (!wav.begin(startRate)) { error("WAV header write failed"); return false; }
    logEvent(LogKind::Opened, 44, completedBytes+sessionBytes);
    return true;
}
bool verifyWav() {
    uint8_t expected[44], actual[44]; wavHeader(expected, startRate, wav.bytes());
    const int fd = open(partial, O_RDONLY);
    if (fd < 0) { ioFailure("open-read", errno, 0, 44, -1); return false; }
    errno = 0;
    const ssize_t n = ::read(fd, actual, sizeof(actual));
    bool ok = n == 44;
    if (!ok) ioFailure("read", errno, 0, 44, n);
    if (ok && memcmp(expected, actual, 44)) { ioFailure("wav-header", 0, 0, 44, 44); ok = false; }
    if (ok) {
        const off_t end = lseek(fd, 0, SEEK_END);
        if (end != off_t(wav.bytes()+44)) { ioFailure("wav-size", errno, 0, wav.bytes()+44, end); ok = false; }
    }
    if (::close(fd)) { ioFailure("close-read", errno, 0, 0, -1); ok = false; }
    return ok;
}
bool closeSegment(bool finalize) {
    if (sink.fd < 0) return false;
    bool ok = finalize && wav.checkpoint();
    if (::close(sink.fd) != 0) { ioFailure("close", errno, sink.position, 0, -1); ok = false; }
    sink.fd = -1;
    if (ok && !verifyWav()) { error("WAV verification failed; partial file preserved"); ok = false; }
    if (ok && ::rename(partial, finalName) != 0) ok = false;
    if (ok) {
        portENTER_CRITICAL(&mux);
        strncpy(view.file, finalName+3, sizeof(view.file)-1);
        view.verified = true; view.verifiedBytes = wav.bytes()+44;
        portEXIT_CRITICAL(&mux);
        logEvent(LogKind::Saved, wav.bytes()+44, completedBytes+sessionBytes);
    } else error("SD save failed; partial file preserved");
    return ok;
}
void inspectCard() {
    // Read the BPB through the public read-only API; handle an MBR or superfloppy.
    uint8_t b[512];
    uint32_t volumeMiB = 0, cluster = 0; uint8_t bits = 0;
    auto le16 = [&b](size_t i) { return uint32_t(b[i]) | uint32_t(b[i+1])<<8; };
    auto le32 = [&b](size_t i) { return uint32_t(b[i]) | uint32_t(b[i+1])<<8 | uint32_t(b[i+2])<<16 | uint32_t(b[i+3])<<24; };
    if (SD.readRAW(b, 0)) {
        bool boot = b[510] == 0x55 && b[511] == 0xaa && le16(11) == 512 && b[13];
        if (!boot && b[510] == 0x55 && b[511] == 0xaa) {
            const uint32_t lba = le32(454);
            boot = lba && SD.readRAW(b, lba) && b[510] == 0x55 && b[511] == 0xaa && le16(11) == 512 && b[13];
        }
        if (boot) {
            const uint32_t sectors = le16(19) ? le16(19) : le32(32);
            const uint32_t fat = le16(22) ? le16(22) : le32(36);
            const uint64_t overhead = le16(14) + uint64_t(b[16])*fat + (le16(17)*32+511)/512;
            if (sectors > overhead && !(b[13] & (b[13]-1))) {
                const uint32_t clusters = (sectors-overhead)/b[13];
                bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
                cluster = 512U*b[13]; volumeMiB = sectors/2048;
            }
        }
    }
    const uint8_t type = SD.cardType();
    const uint32_t capacity = uint32_t(SD.cardSize()/1048576);
    portENTER_CRITICAL(&mux);
    view.spiHz = storageClockHz; view.writeBlockBytes = storageWriteBlockBytes; view.cardType = type;
    view.cardMiB = capacity;
    view.volumeMiB = volumeMiB; view.clusterBytes = cluster; view.fatBits = bits;
    portEXIT_CRITICAL(&mux);
}
uint8_t diagnosticByte(uint32_t at) { return uint8_t((at*33) ^ (at>>8) ^ 0xa5); }
bool runDiagnostic() {
    // Explicit action only. New exclusive file, 1 MiB plus a WAV-like 44-byte
    // prefix, close/reopen/read-back. Never remove or overwrite card contents.
    constexpr uint32_t total = 1048576 + 44;
    uint8_t* block = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!block) { error("Not enough RAM for SD diagnostic"); return false; }
    snprintf(partial, sizeof(partial), "/sd/sd-test-%08lx.bin", (unsigned long)sessionId);
    portENTER_CRITICAL(&mux); strncpy(view.file, partial+3, sizeof(view.file)-1); portEXIT_CRITICAL(&mux);
    sink.position = 0; sink.fd = open(partial, O_WRONLY | O_CREAT | O_EXCL, 0666);
    bool ok = sink.fd >= 0;
    if (!ok) { ioFailure("open", errno, 0, 0, -1); error("SD diagnostic open failed"); }
    const uint64_t writeStarted = esp_timer_get_time();
    uint32_t at = 0;
    while (ok && at < total && !stopRequested.load()) {
        const size_t n = at == 0 ? 44 : (total-at < 4096 ? total-at : 4096);
        for (size_t i = 0; i < n; ++i) block[i] = diagnosticByte(at+i);
        ok = sink.write(block, n) == n;
        if (!ok) { error("SD diagnostic write failed; test file preserved"); break; }
        at += n;
        portENTER_CRITICAL(&mux); view.writtenKiB = at/1024; portEXIT_CRITICAL(&mux);
        vTaskDelay(1);
    }
    if (ok && stopRequested.load()) { error("SD diagnostic cancelled; test file preserved"); ok = false; }
    if (ok && !sink.sync()) { error("SD diagnostic sync failed"); ok = false; }
    if (sink.fd >= 0 && ::close(sink.fd)) { ioFailure("close", errno, at, 0, -1); ok = false; error("SD diagnostic close failed"); }
    sink.fd = -1;
    portENTER_CRITICAL(&mux); view.diagnosticWriteMs = uint32_t((esp_timer_get_time()-writeStarted)/1000); portEXIT_CRITICAL(&mux);
    const uint64_t readStarted = esp_timer_get_time();
    if (ok) {
        sink.fd = open(partial, O_RDONLY);
        if (sink.fd < 0) { ioFailure("open-read", errno, 0, 0, -1); error("SD diagnostic reopen failed"); ok = false; }
    }
    at = 0;
    while (ok && at < total && !stopRequested.load()) {
        const size_t n = total-at < 4096 ? total-at : 4096;
        errno = 0; const ssize_t got = ::read(sink.fd, block, n);
        if (got != ssize_t(n)) { ioFailure("read", errno, at, n, got); error("SD diagnostic read failed"); ok = false; break; }
        for (size_t i = 0; i < n; ++i) if (block[i] != diagnosticByte(at+i)) {
            ioFailure("compare", 0, at+i, diagnosticByte(at+i), block[i]);
            error("SD diagnostic data mismatch"); ok = false; break;
        }
        if (!ok) break;
        at += n;
        portENTER_CRITICAL(&mux); view.verifiedBytes = at; portEXIT_CRITICAL(&mux);
        vTaskDelay(1);
    }
    if (ok && stopRequested.load()) { error("SD diagnostic cancelled; test file preserved"); ok = false; }
    if (sink.fd >= 0 && ::close(sink.fd)) { ioFailure("close-read", errno, at, 0, -1); ok = false; error("SD diagnostic read close failed"); }
    sink.fd = -1; heap_caps_free(block);
    portENTER_CRITICAL(&mux); view.verified = ok; view.diagnosticReadMs = uint32_t((esp_timer_get_time()-readStarted)/1000); portEXIT_CRITICAL(&mux);
    return ok;
}
void discardQueue() {
    if (!queue) return;
    size_t bytes = 0;
    while (void* item = xRingbufferReceive(queue, &bytes, 0)) vRingbufferReturnItem(queue, item);
}
void worker(void*) {
    for (;;) {
        if (!starting.load()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if(journalReadPending.exchange(false)) {
            readJournalTail();starting.store(false);running.store(false);continue;
        }
        const bool checking = checkOnly.exchange(false);
        const bool diagnostic = diagnosticOnly.exchange(false);
        // start() prepares the immutable session configuration before starting.
        sessionBytes = 0; segmentIndex = 0; sessionId = esp_random();
        segmentBytes = uint32_t(uint64_t(startRate)*2*config.segmentMinutes*60);
        healthy = true;
        discardQueue();
        SD.end();
        const auto pins = sessionPins; // Frozen before this worker is notified.
        SPI.begin(pins.sck, pins.miso, pins.mosi, pins.cs);
        if (!SD.begin(pins.cs, SPI, storageClockHz, "/sd", 5, false) || SD.cardType() == CARD_NONE) {
            error("SD unavailable; check card, power and wiring"); healthy = false;
        } else {
            portENTER_CRITICAL(&mux); view.cardReady = true; portEXIT_CRITICAL(&mux);
            inspectCard();
            updateSpace();
            openJournal();
            logEvent(LogKind::CardReady);
            if (!checking && !diagnostic && !SD.exists("/recordings") && !SD.mkdir("/recordings")) {
                error("Cannot create recordings directory"); healthy = false;
            }
            if (healthy && !checking && !diagnostic) healthy = openSegment();
        }
        if (diagnostic) {
            if (healthy) healthy = runDiagnostic();
            if (healthy) state(State::Ready);
            logEvent(LogKind::DiagnosticFinished, view.verifiedBytes, view.verifiedBytes, "", healthy);
            closeJournal();SD.end(); SPI.end();
            starting.store(false); running.store(false);
            continue;
        }
        if (checking) {
            closeJournal();SD.end(); SPI.end();
            if (healthy) state(State::Ready);
            starting.store(false); running.store(false);
            continue;
        }
        // A stop during SD mount must cancel this start too.
        if (healthy && !stopRequested.load()) {
            state(State::Recording);
            logEvent(LogKind::Started);
            flushJournal();
            accepting.store(true);
            if (stopRequested.load()) accepting.store(false);
        }
        starting.store(false);
        uint32_t checkpointAt = millis(), progressAt = checkpointAt;
        while (healthy) {
            size_t bytes = 0;
            uint8_t* item = static_cast<uint8_t*>(xRingbufferReceiveUpTo(queue, &bytes, pdMS_TO_TICKS(20), 4096));
            if (item) {
                size_t offset = 0;
                while (offset < bytes && healthy) {
                    if (wav.bytes() == segmentBytes) {
                        const uint64_t rotationAt = esp_timer_get_time();
                        healthy = closeSegment(true) && openSegment();
                        flushJournal();
                        noteStorageDelay(rotationAt);
                        if (!healthy) break;
                    }
                    size_t n = bytes-offset;
                    if (n > segmentBytes-wav.bytes()) n = segmentBytes-wav.bytes();
                    uint64_t before = esp_timer_get_time();
                    healthy = wav.append(item+offset, n);
                    uint32_t writeMs = uint32_t((esp_timer_get_time()-before)/1000);
                    noteStorageDelay(before);
                    if (!healthy) { error(errno == ENOSPC ? "SD full; partial file preserved" : "SD write failed; partial file preserved"); break; }
                    sessionBytes += n; offset += n;
                    portENTER_CRITICAL(&mux);
                    view.writtenKiB = uint32_t((completedBytes+sessionBytes)/1024);
                    if (writeMs > view.maxWriteMs) view.maxWriteMs = writeMs;
                    portEXIT_CRITICAL(&mux);
                }
                vRingbufferReturnItem(queue, item);
            }
            if (captureFailed.load()) { error("Microphone capture interrupted; partial file preserved"); healthy = false; }
            if (lost.load()) { error("Audio buffer overflow; recording stopped with a gap"); healthy = false; }
            if (!healthy) break;
            if (millis()-checkpointAt >= 5000) {
                const uint64_t syncAt = esp_timer_get_time();
                healthy = wav.checkpoint();
                noteStorageDelay(syncAt);
                if (!healthy) { error("SD checkpoint failed; partial file preserved"); break; }
                updateSpace(); flushJournal(); checkpointAt = millis();
            }
            portENTER_CRITICAL(&mux);
            view.elapsedSeconds = uint32_t((completedUs+sessionBytes*500000ULL/startRate)/1000000);
            portEXIT_CRITICAL(&mux);
            if (uint32_t(millis()-progressAt) >= 300000) {
                logEvent(LogKind::Progress, wav.bytes()+44, completedBytes+sessionBytes);
                progressAt = millis();
            }
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
        if (healthy) {
            portENTER_CRITICAL(&mux); view.error[0] = 0; view.state = State::Saved; portEXIT_CRITICAL(&mux);
        }
        const char* reason = !healthy ? "error" : stopRequested.load() ?
            requestedReason.load()==StopReason::LowBattery ? "low battery" :
            requestedReason.load()==StopReason::Thermal ? "thermal pause" :
            requestedReason.load()==StopReason::ModeChange ? "mode changed" :
            requestedReason.load()==StopReason::Update ? "firmware update" : "stop requested" :
            targetFrames ? "duration limit" : "capture stopped";
        logEvent(LogKind::Finished, 0, completedBytes+sessionBytes, reason, healthy);
        closeJournal();SD.end();SPI.end();
        running.store(false);
    }
}
bool prepare(bool needsQueue = true) {
    preparationError = "Not enough internal RAM for SD; stop Listen and close extra device tabs";
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t internalHeadroom = 72 * 1024;
    const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    // Capture, DMA and the Wi-Fi driver are already allocated. External PCM
    // needs the same 48 KiB SD/filesystem/network working reserve as a card
    // check, not the Streaming ring's allowance for starting another producer.
    // The C5 PDM + AP + mDNS field case has only 54-57 KiB internal free while
    // 8 MiB PSRAM is available. Keep the larger margin for an internal queue.
    const bool psramQueue = needsQueue && (queueInPsram ||
        (!queue && heap_caps_get_free_size(external) >= 128 * 1024 + 8192));
    const size_t headroom = !needsQueue || psramQueue ? recordingmemory::kWorkingReserveBytes : internalHeadroom;
    // Account for the worker stack and task metadata before allocating a queue.
    const size_t taskBytes = storageTask ? 0 : 6144 + 2048;
    const size_t freeInternal = heap_caps_get_free_size(internal);
    if (freeInternal < headroom + taskBytes) return false;
    if (!storageTask && heap_caps_get_largest_free_block(internal) < taskBytes) return false;
    if (needsQueue && !queue) {
        // SD writes use CPU-accessible PCM, not an SPI DMA buffer. Keep this
        // queue in PSRAM where available, leaving Wi-Fi/Listen RAM internal.
        // Five seconds of PCM where PSRAM is available, with a smaller PSRAM
        // option before falling back to bounded internal RAM on compact boards.
        const size_t psramOptions[] = {512 * 1024, 128 * 1024};
        for (const size_t bytes : psramOptions) {
            if (heap_caps_get_free_size(external) < bytes + 8192) continue;
            queue = xRingbufferCreateWithCaps(bytes, RINGBUF_TYPE_BYTEBUF, external);
            if (queue) { queueInPsram = true; queueCapacityBytes.store(bytes); break; }
        }
        if (!queue) {
            // If PSRAM allocation failed/fragmented, reapply the full internal
            // margin before subtraction; never underflow into a huge request.
            if (freeInternal < internalHeadroom + taskBytes) return false;
            size_t reserve = freeInternal - internalHeadroom - taskBytes;
            if (reserve > 128 * 1024) reserve = 128 * 1024;
            // Include ring-control/allocation overhead in the retained margin.
            for (; reserve >= 32768 + 1024 && !queue; reserve -= 4096) {
                queue = xRingbufferCreate(reserve - 1024, RINGBUF_TYPE_BYTEBUF);
                if (queue) queueCapacityBytes.store(reserve - 1024);
            }
        }
        if (!queue) { preparationError = "Could not allocate the SD recording buffer"; return false; }
    }
    // Re-read actual heap after allocation: a capability allocator or another
    // task may have consumed internal RAM since the initial budget snapshot.
    const auto releaseQueue = []() {
        if (queue) {
            if (queueInPsram) vRingbufferDeleteWithCaps(queue);
            else vRingbufferDelete(queue);
        }
        queue = nullptr; queueInPsram = false; queueCapacityBytes.store(0);
        return false;
    };
    const size_t retained = !needsQueue || queueInPsram ? recordingmemory::kWorkingReserveBytes : internalHeadroom;
    if (heap_caps_get_free_size(internal) < retained + taskBytes) return releaseQueue();
    if (!storageTask && xTaskCreate(worker, "sd_storage", 6144, nullptr, 2, &storageTask) != pdPASS) {
        preparationError = "Could not start the SD storage task; retry recording";
        return releaseQueue();
    }
    if (heap_caps_get_free_size(internal) < retained) return releaseQueue();
    return true;
}
#endif
}
// Optional weak hook used only by the project-local SD diagnostic library.
extern "C" void birdnetmic_sd_trace(uint32_t phase, uint32_t command, uint32_t response, uint32_t) {
    portENTER_CRITICAL(&mux);
    if (phase == 1 && !view.sdDataToken) view.sdDataToken = response;
    if (phase == 2) { view.sdCommand = command; view.sdCommandReply = response; }
    if (phase == 3) ++view.sdWaitTimeouts;
    if (phase == 4) view.sdStatus = response;
    portEXIT_CRITICAL(&mux);
}
LogSettings logSettings() { return journalConfig; }
bool saveLogSettings(const LogSettings& value) {
    if (busy() || resumePending() || value.magic!=0x4c4f4731 || value.sdEnabled>1 || value.detailed>1 || value.maxKiB<64 || value.maxKiB>16384) return false;
    if (!memcmp(&journalConfig,&value,sizeof(value))) return true;
    Preferences p;if(!p.begin("reclog",false))return false;
    const bool ok=p.putBytes("settings",&value,sizeof(value))==sizeof(value);p.end();
    if(ok)journalConfig=value;
    return ok;
}
void queueSystemLog(const char* line) {
    if(!mode() || !journalConfig.sdEnabled || !line)return;
    const size_t n=strlen(line)<511?strlen(line):511;
    portENTER_CRITICAL(&mux);if(systemLogs)systemLogDropped+=systemLogs->append(line,n);else ++systemLogDropped;portEXIT_CRITICAL(&mux);
}
bool requestLogTail(bool archive, bool supported, uint8_t variant) {
    if(busy() || resumePending() || !mode() || !supported)return false;
    const auto pins=recordingpins::resolve(variant);if(!pins.supported)return false;
#if RECORDING_SPI_SUPPORTED
    if(!journalReadData)journalReadData=static_cast<char*>(allocateLogStorage(2049));
    if(!journalReadData) {strcpy(journalReadFailure,"Not enough memory to read the SD log");journalReadReady.store(true);return false;}
    if(!prepare(false)) {strcpy(journalReadFailure,"Not enough free memory to prepare SD log reading");journalReadReady.store(true);return false;}
    sessionPins=pins;journalReadArchive=archive;journalReadReady.store(false);journalReadPending.store(true);
    stopRequested.store(false);checkOnly.store(false);diagnosticOnly.store(false);running.store(true);starting.store(true);return true;
#else
    (void)archive;return false;
#endif
}
bool logTailBusy() {return busy() && !journalReadReady.load();}
bool logTailReady() {return journalReadReady.load();}
String logTail() {return logTailReady()?String(journalReadData):String();}
String logTailError() {return logTailReady()?String(journalReadFailure):String();}
uint32_t logTailFileBytes() {return logTailReady()?journalReadBytes:0;}
void begin() {
    if(!systemLogs)systemLogs=allocateLogObject<BoundedLog<3072>>();
    Preferences p;
    if(p.begin("reclog",true)) {
        LogSettings saved;
        if(p.getBytesLength("settings")==sizeof(saved) && p.getBytes("settings",&saved,sizeof(saved))==sizeof(saved) &&
           saved.magic==journalConfig.magic && saved.sdEnabled<=1 && saved.detailed<=1 && saved.maxKiB>=64 && saved.maxKiB<=16384)journalConfig=saved;
        p.end();
    }
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
    if (busy() || resumePending() || value.magic != config.magic || value.recordingMode > 1 || value.autoStart > 1 ||
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
void setRuntimeMode(bool enabled) { config.recordingMode = enabled; }
bool busy() { return starting.load() || running.load(); }
bool resumePending() { return coolingPending.load(); }
void cancelResume() {
    coolingPending.store(false);
    if (status().state == State::Cooling) state(State::Saved);
}
bool pauseForCooling() {
    const auto before = status().state;
    if (!busy() || (before != State::Recording && before != State::Starting)) return false;
    requestStop(StopReason::Thermal);
    if (!stop() || status().state == State::Error) return false;
#if RECORDING_SPI_SUPPORTED
    remainingFrames = targetFrames > sessionBytes/2 ? targetFrames-sessionBytes/2 : 0;
    if (targetFrames && !remainingFrames) return false;
    completedUs += sessionBytes*500000ULL/startRate;
    completedBytes += sessionBytes;
    coolingPending.store(true);
    state(State::Cooling);
    logEvent(LogKind::Cooling, 0, completedBytes);
    return true;
#else
    return false;
#endif
}
bool checkCard(bool supported, uint8_t boardVariant) {
    if (busy() || resumePending() || !mode()) return false;
    portENTER_CRITICAL(&mux); view = Status{}; portEXIT_CRITICAL(&mux);
    lost.store(0);
    const auto pins = recordingpins::resolve(boardVariant);
    if (!supported || !pins.supported) { error("SD recording is unavailable for this board"); return false; }
#if RECORDING_SPI_SUPPORTED
    if (!prepare(false)) { error(preparationError); return false; }
    sessionPins = pins;
    state(State::Checking);
    stopRequested.store(false); diagnosticOnly.store(false); checkOnly.store(true);
    running.store(true); starting.store(true);
    return true;
#else
    error("SD recording is unavailable for this board"); return false;
#endif
}
static bool startSession(uint32_t rate, bool supported, uint8_t boardVariant, bool continuing) {
    if (busy()) return false;
    if (!mode()) { error("Select Recording mode before starting"); return false; }
    const auto pins = recordingpins::resolve(boardVariant);
    if (!supported || !pins.supported) { error("SD recording is unavailable for this board"); return false; }
#if RECORDING_SPI_SUPPORTED
    if (rate < 8000 || rate > 96000) { error("Unsupported WAV sample rate"); return false; }
    // A new attempt must not inherit a previous card-check error and mask a
    // current allocation failure. Stop/cleanup still preserve their first error.
    portENTER_CRITICAL(&mux); view = Status{}; view.state = State::Starting; portEXIT_CRITICAL(&mux);
    if (!prepare()) { error(preparationError); return false; }
    sessionPins = pins;
    checkOnly.store(false); diagnosticOnly.store(false);
    if (continuing) targetFrames = config.durationMinutes ? remainingFrames*rate/startRate : 0;
    else {
        completedUs = completedBytes = 0;
        targetFrames = uint64_t(config.durationMinutes)*60*rate;
    }
    if (continuing && config.durationMinutes && !targetFrames) targetFrames = 1;
    startRate = rate; acceptedFrames = 0;
    portENTER_CRITICAL(&mux);
    view.elapsedSeconds = uint32_t(completedUs/1000000);
    view.writtenKiB = uint32_t(completedBytes/1024);
    portEXIT_CRITICAL(&mux);
    coolingPending.store(false);
    lost.store(0); captureFailed.store(false); stopRequested.store(false);
    logEvent(continuing ? LogKind::ResumeRequested : LogKind::StartRequested, 0, completedBytes);
    running.store(true); starting.store(true);
    return true;
#else
    error("SD recording is unavailable for this board"); return false;
#endif
}
bool setStorageClock(uint32_t hz, uint32_t writeBlockBytes) {
    if (busy() || resumePending() || (hz != 20000000 && hz != 16000000 && hz != 8000000 && hz != 4000000 && hz != 2000000 && hz != 1000000) ||
        (writeBlockBytes != 4096 && writeBlockBytes != 512)) return false;
    storageClockHz = hz; storageWriteBlockBytes = writeBlockBytes;
    portENTER_CRITICAL(&mux); view.spiHz = hz; view.writeBlockBytes = writeBlockBytes; portEXIT_CRITICAL(&mux);
    return true;
}
bool diagnose(bool supported, uint8_t boardVariant) {
    if (busy() || resumePending() || !mode()) return false;
    const auto pins = recordingpins::resolve(boardVariant);
    if (!supported || !pins.supported) return false;
#if RECORDING_SPI_SUPPORTED
    if (!prepare(false)) { rejectStart(preparationError); return false; }
    portENTER_CRITICAL(&mux); view = Status{}; view.diagnostic = true; view.state = State::Checking; portEXIT_CRITICAL(&mux);
    lost.store(0); sessionPins = pins;
    stopRequested.store(false); checkOnly.store(false); diagnosticOnly.store(true);
    logEvent(LogKind::DiagnosticRequested);
    running.store(true); starting.store(true); return true;
#else
    return false;
#endif
}
bool start(uint32_t rate, bool supported, uint8_t boardVariant) {
    return startSession(rate, supported, boardVariant, false);
}
bool rejectStart(const char* message) {
    if (busy()) return false; // Never change an active session's status.
    portENTER_CRITICAL(&mux); view.error[0] = 0; portEXIT_CRITICAL(&mux);
    error(message);
    return false;
}
bool resume(uint32_t rate, bool supported, uint8_t boardVariant) {
    if (!resumePending()) return false;
    const bool ok = startSession(rate, supported, boardVariant, true);
    if (!ok) cancelResume();
    return ok;
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
void requestStop(StopReason reason) {
    if(!stopRequested.load())requestedReason.store(reason);
    const bool already = stopRequested.exchange(true);
    accepting.store(false);
    if (busy()) {
        portENTER_CRITICAL(&mux);
        // Do not overwrite Saved when worker cleanup races a repeated stop.
        if (view.state == State::Starting || view.state == State::Recording || view.state == State::Checking) {
            view.state = State::Saving;
            if (!already) logLocked(LogKind::StopRequested);
        }
        portEXIT_CRITICAL(&mux);
    }
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
bool releaseBuffer() {
    if (busy() || resumePending() || inPush.load()) return false;
#if RECORDING_SPI_SUPPORTED
    if (queue) {
        if (queueInPsram) vRingbufferDeleteWithCaps(queue);
        else vRingbufferDelete(queue);
        queue = nullptr;
    }
    queueInPsram = false;
    queueCapacityBytes.store(0);
#endif
    return true;
}
Status status() {
    portENTER_CRITICAL(&mux); Status copy = view; portEXIT_CRITICAL(&mux);
    copy.droppedSamples = lost.load(); copy.queueBytes = queueCapacityBytes.load();
    return copy;
}
namespace {
String formatEvent(const LogEvent& e) {
    char text[640];
    String line;
    const auto& s = e.status;
    if (!journalConfig.detailed) {
        switch(e.kind) {
            case LogKind::Started:snprintf(text,sizeof(text),"Recording: started; file=%s; rate=%luHz",s.file,(unsigned long)e.rate);break;
            case LogKind::Saved:snprintf(text,sizeof(text),"Recording: saved; file=%s; bytes=%lu",s.file,(unsigned long)e.fileBytes);break;
            case LogKind::Finished:snprintf(text,sizeof(text),"Recording: %s; duration=%lus; audio_bytes=%llu; reason=%s",e.success?"stopped":"failed",(unsigned long)s.elapsedSeconds,(unsigned long long)e.totalBytes,e.reason);break;
            case LogKind::Error:snprintf(text,sizeof(text),"Recording: ERROR; %s; file=%s",s.error,s.file[0]?s.file:"-");break;
            case LogKind::Cooling:snprintf(text,sizeof(text),"Recording: paused for cooling");break;
            case LogKind::ResumeRequested:snprintf(text,sizeof(text),"Recording: resuming after cooling");break;
            case LogKind::CardReady:snprintf(text,sizeof(text),"Recording: SD card ready; free=%luMiB",(unsigned long)s.freeMiB);break;
            default:snprintf(text,sizeof(text),"Recording: event");break;
        }
        line=text;
        if(e.kind==LogKind::Error && s.ioOperation[0]) {
            snprintf(text,sizeof(text),"; io=%s; errno=%d",s.ioOperation,s.ioErrno);line+=text;
        }
        return line;
    }
    const char* name = "event";
    switch(e.kind) {
        case LogKind::StartRequested: name="start requested"; break;
        case LogKind::ResumeRequested: name="resume requested"; break;
        case LogKind::Started: name="started"; break;
        case LogKind::Opened: name="segment opened"; break;
        case LogKind::Saved: name="segment saved and verified"; break;
        case LogKind::Progress: name="progress"; break;
        case LogKind::StopRequested: name="stop requested"; break;
        case LogKind::Finished: name=e.success ? "finished" : "finished with error; partial file preserved"; break;
        case LogKind::Error: name="ERROR"; break;
        case LogKind::Cooling: name="paused for cooling"; break;
        case LogKind::CardReady:
            snprintf(text,sizeof(text),"Recording: SD ready; card=%luMiB; free=%luMiB; FAT%u; SPI=%luHz; write_block=%luB; event_uptime_ms=%lu",
                (unsigned long)s.cardMiB,(unsigned long)s.freeMiB,unsigned(s.fatBits),
                (unsigned long)s.spiHz,(unsigned long)s.writeBlockBytes,(unsigned long)e.atMs);
            line=text; return line;
        case LogKind::DiagnosticRequested: name="SD diagnostic requested"; break;
        case LogKind::DiagnosticFinished: name=e.success ? "SD diagnostic verified" : "SD diagnostic failed"; break;
    }
    if (e.kind == LogKind::StartRequested || e.kind == LogKind::ResumeRequested) {
        snprintf(text,sizeof(text),"Recording: %s; rate=%luHz; duration=%lumin (0=continuous); segment=%lumin; queue=%luB",
            name,(unsigned long)e.rate,(unsigned long)e.durationMinutes,(unsigned long)e.segmentMinutes,(unsigned long)s.queueBytes);
    } else if (e.kind == LogKind::Opened || e.kind == LogKind::Saved) {
        snprintf(text,sizeof(text),"Recording: %s; file=%s; segment_size=%luB; written=%lluB",
            name,s.file[0]?s.file:"-",(unsigned long)e.fileBytes,(unsigned long long)e.totalBytes);
    } else if (e.kind == LogKind::Started) {
        snprintf(text,sizeof(text),"Recording: started; file=%s; rate=%luHz",s.file[0]?s.file:"-",(unsigned long)e.rate);
    } else {
        snprintf(text,sizeof(text),"Recording: %s; elapsed=%lus; written=%lluB; dropped=%lu; max_write=%lums; file=%s",
            name,(unsigned long)s.elapsedSeconds,(unsigned long long)e.totalBytes,(unsigned long)s.droppedSamples,
            (unsigned long)s.maxWriteMs,s.file[0]?s.file:"-");
    }
    line = text;
    if (uint32_t(millis()-e.atMs) > 1000) { line += "; event_uptime_ms="; line += String(e.atMs); }
    if (e.reason[0]) { line += "; reason="; line += e.reason; }
    if (s.error[0]) { line += "; error="; line += s.error; }
    if (s.ioOperation[0]) {
        snprintf(text,sizeof(text),"; io=%s; errno=%d; offset=%lu; requested=%lu; returned=%ld",
            s.ioOperation,s.ioErrno,(unsigned long)s.ioOffset,(unsigned long)s.ioRequested,(long)s.ioReturned);
        line += text;
    }
    return line;
}
}

bool nextLog(String& line) {
    uint32_t dropped;
    portENTER_CRITICAL(&mux);
    if(journalWarningPending) {journalWarningPending=false;char warning[80];strcpy(warning,view.logError);portEXIT_CRITICAL(&mux);line=warning;return true;}
    dropped=logDropped;logDropped=0;
    portEXIT_CRITICAL(&mux);
    if(dropped) {line="Recording: "+String(dropped)+" older log events dropped (event queue full)";return true;}
    LogEvent event;if(!takeEvent(event,1))return false;
    line=formatEvent(event);return true;
}

String statusJson(bool supported, uint8_t boardVariant, uint32_t sampleRate) {
    Status s = status();
    const char* states[] = {"ready", "starting", "recording", "saving", "saved", "error", "checking", "cooling"};
    // Names and errors are generated internally and contain no JSON metacharacters.
    String out = "{\"mode\":\""; out += mode() ? "recording" : "streaming";
    out += "\",\"state\":\""; out += states[uint8_t(s.state)];
    out += "\",\"supported\":"; out += supported ? "true" : "false";
    out += ",\"storage\":\""; out += recordingpins::storage(boardVariant); out += "\"";
    out += ",\"busy\":"; out += busy() ? "true" : "false";
    out += ",\"cooling_pending\":"; out += resumePending() ? "true" : "false";
    out += ",\"space_seconds\":";
    out += s.cardReady && sampleRate >= 8000 && sampleRate <= 96000
        ? String(uint32_t(s.freeBytes/(uint64_t(sampleRate)*2))) : String("null");
    out += ",\"card_ready\":"; out += s.cardReady ? "true" : "false";
    out += ",\"elapsed_seconds\":" + String(s.elapsedSeconds);
    out += ",\"duration_minutes\":" + String(config.durationMinutes);
    out += ",\"segment_minutes\":" + String(config.segmentMinutes);
    out += ",\"auto_start\":"; out += config.autoStart ? "true" : "false";
    out += ",\"logging\":{\"sd_enabled\":" + String(journalConfig.sdEnabled?"true":"false") + ",\"detailed\":" + String(journalConfig.detailed?"true":"false") + ",\"max_kib\":" + String(journalConfig.maxKiB) + ",\"file\":\"/recording.log\",\"bytes\":" + String(s.logBytes) + ",\"error\":\"" + String(s.logError) + "\"}";
    out += ",\"free_mib\":" + String(s.freeMiB);
    out += ",\"written_kib\":" + String(s.writtenKiB);
    out += ",\"dropped_samples\":" + String(s.droppedSamples);
    out += ",\"max_write_ms\":" + String(s.maxWriteMs);
    out += ",\"spi_hz\":" + String(s.spiHz);
    out += ",\"write_block_bytes\":" + String(s.writeBlockBytes);
    out += ",\"card_mib\":" + String(s.cardMiB) + ",\"volume_mib\":" + String(s.volumeMiB);
    out += ",\"card_type\":" + String(s.cardType) + ",\"fat_bits\":" + String(s.fatBits);
    out += ",\"cluster_bytes\":" + String(s.clusterBytes);
    out += ",\"sd_data_token\":" + String(s.sdDataToken) + ",\"sd_command\":" + String(s.sdCommand);
    out += ",\"sd_command_reply\":" + String(s.sdCommandReply) + ",\"sd_status\":" + String(s.sdStatus);
    out += ",\"sd_wait_timeouts\":" + String(s.sdWaitTimeouts);
    out += ",\"io_errno\":" + String(s.ioErrno) + ",\"io_offset\":" + String(s.ioOffset);
    out += ",\"io_requested\":" + String(s.ioRequested) + ",\"io_returned\":" + String(s.ioReturned);
    out += ",\"io_operation\":\"" + String(s.ioOperation) + "\"";
    out += ",\"diagnostic\":"; out += s.diagnostic ? "true" : "false";
    out += ",\"verified\":"; out += s.verified ? "true" : "false";
    out += ",\"verified_bytes\":" + String(s.verifiedBytes);
    out += ",\"diagnostic_write_ms\":" + String(s.diagnosticWriteMs) + ",\"diagnostic_read_ms\":" + String(s.diagnosticReadMs);
    out += ",\"queue_bytes\":" + String(s.queueBytes);
    out += ",\"file\":\"" + String(s.file) + "\",\"error\":\"" + String(s.error) + "\"}";
    return out;
}
}
