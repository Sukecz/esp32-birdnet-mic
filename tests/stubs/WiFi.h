#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

inline uint32_t millis() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
inline void delay(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

class WiFiClient {
public:
    int socket = -1;
    virtual ~WiFiClient() { stop(); }
    virtual size_t write(const uint8_t* data, size_t size) {
        int n = send(socket, data, size, MSG_DONTWAIT);
        return n > 0 ? n : 0;
    }
    virtual size_t write(uint8_t data) { return write(&data, 1); }
    int fd() const { return socket; }
    void stop() { if (socket >= 0) close(socket); socket = -1; }
};
