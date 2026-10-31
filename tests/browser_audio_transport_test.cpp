#include "Arduino.h"
#include "WiFi.h"
#include "BrowserAudio.h"
#include <cassert>
#include <cerrno>
#include <vector>
#include <string>
#include <signal.h>
#include <sys/syscall.h>

uint32_t testNow = 100;
static int latestServer = -1, partialFd = -1, partialMode = 0;
// Force an odd partial PCM send followed by EAGAIN, using real socket bytes.
extern "C" ssize_t send(int fd, const void* bytes, size_t count, int flags) {
    if (fd == partialFd && partialMode == 2) { errno = EAGAIN; return -1; }
    if (fd == partialFd && partialMode == 1) { count = std::min(count, size_t(23)); partialMode = 2; }
    return syscall(SYS_sendto, fd, bytes, count, flags, nullptr, 0);
}
static int connect(const std::string& request) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int size = 4096; setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    testConnections.emplace_back(sockets[0]);
    latestServer = sockets[0];
    if (!request.empty()) assert(send(sockets[1], request.data(), request.size(), 0) == int(request.size()));
    return sockets[1];
}
static std::string read(int fd) {
    std::string result;
    char data[65536];
    int size;
    while ((size = recv(fd, data, sizeof(data), MSG_DONTWAIT)) > 0) result.append(data, size);
    return result;
}
static uint32_t le32(const std::string& text, size_t offset) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(uint8_t(text[offset + i])) << (8 * i);
    return value;
}
static const char* request = "GET /listen.wav HTTP/1.1\r\nHost: mic.local:81\r\nOrigin: http://mic.local\r\n\r\n";
int main() {
    signal(SIGPIPE, SIG_IGN);
    browserAudioBegin();
    for (uint32_t rate : {8000, 48000, 192000}) {
        int peer = connect(request);
        browserAudioLoop(true, rate);
        std::string header = read(peer);
        size_t wave = header.find("\r\n\r\n") + 4;
        assert(header.find("200 OK") != std::string::npos);
        assert(header.find("Access-Control-Allow-Origin: http://mic.local\r\n") != std::string::npos);
        assert(header.substr(wave, 4) == "RIFF" && header.substr(wave + 8, 8) == "WAVEfmt ");
        assert(le32(header, wave + 24) == rate && le32(header, wave + 28) == rate * 2);
        const int16_t pcm[] = {-32768, -1, 0, 1, 32767};
        browserAudioPush(pcm, 5); browserAudioLoop(true, rate);
        assert(read(peer) == std::string(reinterpret_cast<const char*>(pcm), sizeof(pcm)));
        assert(browserAudioActive());
        int other = connect(request); browserAudioLoop(true, rate);
        const auto busy=read(other);
        assert(busy.find("503 Service Unavailable") != std::string::npos);
        assert(busy.find("Access-Control-Allow-Origin: *\r\n") != std::string::npos);
        assert(browserAudioActive()); close(other); // Does not displace existing listener.
        browserAudioLoop(false, rate);
        assert(!browserAudioActive()); close(peer);
    }
    for (const char* bad : {
        "GET /listen.wav HTTP/1.1\r\nHost: mic.local:81\r\nOrigin: http://evil.example\r\n\r\n",
        "POST /listen.wav HTTP/1.1\r\nHost: mic.local:81\r\n\r\n",
        "GET /wrong HTTP/1.1\r\nHost: mic.local:81\r\n\r\n"}) {
        int peer = connect(bad); browserAudioLoop(true, 48000);
        assert(read(peer).find("400 Bad Request") != std::string::npos);
        assert(!browserAudioActive()); close(peer);
    }
    int peer = connect(""); browserAudioLoop(true, 48000);
    assert(!browserAudioActive()); // Never reuse a previous request's headers.
    testNow += 2000; browserAudioLoop(true, 48000);
    assert(!browserAudioActive()); close(peer);
    peer = connect(request); browserAudioLoop(false, 48000);
    assert(read(peer).find("503 Service Unavailable") != std::string::npos); close(peer);

    // A peer that stops reading must be dropped with bounded, nonblocking work.
    peer = connect(request); browserAudioLoop(true, 48000); read(peer);
    std::vector<int16_t> pcm(1024, 12);
    for (int i = 0; i < 30 && browserAudioActive(); ++i) {
        browserAudioPush(pcm.data(), pcm.size()); browserAudioLoop(true, 48000); ++testNow;
    }
    testNow += 5001; browserAudioLoop(true, 48000);
    assert(!browserAudioActive()); close(peer);
    const auto slow = browserAudioDiagnostics();
    assert(slow.overflows || slow.sendTimeouts);
    assert(slow.stops && slow.bytesSent);

    // Distinguish capture starvation from a blocked socket or a full PCM queue.
    peer = connect(request); browserAudioLoop(true, 48000); read(peer);
    for (int i = 0; i < 21 && browserAudioActive(); ++i) {
        testNow += 100; browserAudioLoop(true, 48000);
    }
    assert(!browserAudioActive());
    assert(std::string(browserAudioDiagnostics().lastStop) == "no_audio");
    close(peer);

    peer = connect(request); browserAudioLoop(true, 48000); read(peer);
    std::vector<int16_t> overflow(browserAudioQueueBytes()/2 + 1, 0);
    browserAudioPush(overflow.data(), overflow.size());
    testNow += 400; browserAudioLoop(true, 48000);
    assert(browserAudioActive());
    assert(std::string(browserAudioDiagnostics().lastStop) == "none");
    assert(browserAudioDiagnostics().maxLoopGapMs == 400);
    // Recovery never corrupts alignment or affects the next fresh PCM block.
    const int16_t fresh[] = {1234,-1234,0,32767};
    browserAudioPush(fresh,4);browserAudioLoop(true,48000);
    assert(read(peer)==std::string(reinterpret_cast<const char*>(fresh),sizeof(fresh)));
    browserAudioStop();
    close(peer);

    // Real 48 kHz PCM produced while HTTP/UI work delays the consumer 40 ms.
    // A single 2 KiB send per pass falls behind 3840 bytes/pass and overflows
    // even with an eager reader. Bounded catch-up must preserve every byte.
    peer = connect(request); browserAudioLoop(true, 48000); read(peer);
    std::vector<int16_t> burst(1920);
    for (size_t i=0;i<burst.size();++i) burst[i]=int16_t(i*17);
    const std::string expected(reinterpret_cast<const char*>(burst.data()),burst.size()*2);
    for (int i=0;i<200;++i) {
        testNow += 40;
        browserAudioPush(burst.data(),burst.size());browserAudioLoop(true,48000);
        assert(browserAudioActive());
        assert(read(peer)==expected);
    }
    browserAudioStop();close(peer);

    // An overrun during a partially sent block must retain the remaining bytes:
    // dropping them after an odd send would shift every later PCM sample.
    peer = connect(request);browserAudioLoop(true,48000);read(peer);
    std::vector<int16_t> first(1024);for(size_t i=0;i<first.size();++i)first[i]=int16_t(i*29);
    partialFd=latestServer;partialMode=1;
    browserAudioPush(first.data(),first.size());browserAudioLoop(true,48000);
    std::string received=read(peer);assert(received.size()==23);
    browserAudioPush(first.data(),first.size()); // Queued stale PCM, separate from pending.
    browserAudioPush(overflow.data(),overflow.size());browserAudioLoop(true,48000);
    assert(browserAudioActive()&&browserAudioDiagnostics().previewDiscardedBytes>=first.size()*2);
    partialMode=0;browserAudioPush(fresh,4);browserAudioLoop(true,48000);received+=read(peer);
    const std::string firstBytes(reinterpret_cast<const char*>(first.data()),first.size()*2);
    assert(received==firstBytes+std::string(reinterpret_cast<const char*>(fresh),sizeof(fresh)));
    partialFd=-1;browserAudioStop();close(peer);
}
