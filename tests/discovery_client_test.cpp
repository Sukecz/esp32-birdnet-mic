#include "../esp32-birdnet-mic/DiscoveryClient.h"
#include <cassert>
#include <vector>

int main() {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    DiscoveryClient client;
    client.socket = pair[0];
    client.discoveryWrite = true;
    uint8_t data[] = {1, 2, 3};
    assert(client.write(data, sizeof(data)) == sizeof(data));
    uint8_t received[3];
    assert(recv(pair[1], received, sizeof(received), 0) == 3);
    assert(received[0] == 1 && received[2] == 3);
    // A receiver that never drains its socket cannot block discovery indefinitely.
    int capacity = 1024;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
    std::vector<uint8_t> large(1024 * 1024, 42);
    uint32_t before = millis();
    size_t written = client.write(large.data(), large.size());
    assert(written > 0 && written < large.size());
    assert(uint32_t(millis() - before) < 500);
    assert(client.fd() == -1); // Never reuse a stream containing a partial packet.
    assert(client.write(data, sizeof(data)) == 0);
    close(pair[1]);
}
