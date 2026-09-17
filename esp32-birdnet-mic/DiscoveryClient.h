#pragma once
#include <WiFi.h>
#include <errno.h>
#include <sys/socket.h>

// Bound a discovery write even when the broker stops reading. A partial MQTT
// packet must close the connection; publishing another packet would corrupt it.
class DiscoveryClient : public WiFiClient {
public:
    bool discoveryWrite = false;
    using WiFiClient::write;
    size_t write(const uint8_t* data, size_t size) override {
        if (!discoveryWrite) return WiFiClient::write(data, size);
        size_t sent = 0;
        uint32_t started = millis();
        while (sent < size && uint32_t(millis() - started) < 20) {
            int socketFd = fd();
            if (socketFd < 0) break;
            int n = send(socketFd, data + sent, size - sent, MSG_DONTWAIT);
            if (n > 0) { sent += n; continue; }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
            delay(1);
        }
        if (sent != size) stop();
        return sent;
    }
    size_t write(uint8_t data) override { return write(&data, 1); }
};
