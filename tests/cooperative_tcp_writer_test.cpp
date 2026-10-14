#include "CooperativeTcpWriter.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using Writer = CooperativeTcpWriter;

int main() {
    std::vector<uint8_t> frame(8208);
    for (size_t i = 0; i < frame.size(); ++i) frame[i] = uint8_t(i);
    const auto expected = frame;
    Writer slow, healthy;
    assert(slow.queue(frame.data(), frame.size(), 0));
    // Source scratch can be reused immediately: pending data is an owned copy.
    std::fill(frame.begin(), frame.end(), 99);
    assert(!slow.queue(frame.data(), 10, 1));
    std::vector<uint8_t> received;
    int calls = 0;
    auto partial = [&](const uint8_t* data, size_t length) {
        ++calls;
        size_t n = std::min(length, size_t(13));
        received.insert(received.end(), data, data + n);
        return int(n);
    };
    assert(slow.poll(1, partial) == Writer::PENDING && calls == 4);
    assert(slow.poll(2, [](const uint8_t*, size_t) { errno = EAGAIN; return -1; }) == Writer::PENDING);
    assert(slow.stalled());
    assert(slow.poll(2, [&](const uint8_t*, size_t) { assert(false); return 0; }) == Writer::PENDING);
    assert(healthy.queue(expected.data(), 1040, 2));
    assert(healthy.poll(3, [](const uint8_t*, size_t size) { return int(size); }) == Writer::COMPLETE);
    healthy.clear();
    // An interrupted send returns control, then resumes at the exact offset.
    assert(slow.poll(3, [](const uint8_t*, size_t) { errno = EINTR; return -1; }) == Writer::PENDING);
    while (slow.poll(4, partial) != Writer::COMPLETE) {}
    assert(received == expected);
    slow.clear();
    // A control reply follows the complete RTP frame, never its partial bytes.
    const uint8_t reply[] = {'R', 'T', 'S', 'P'};
    assert(slow.queue(reply, sizeof(reply), 5));
    assert(slow.poll(6, partial) == Writer::COMPLETE);
    assert(std::equal(reply, reply + sizeof(reply), received.end() - sizeof(reply)));
    slow.clear();
    assert(slow.queue(reply, sizeof(reply), UINT32_MAX - 20));
    assert(slow.poll(uint32_t(UINT32_MAX - 20 + 9999UL), [](const uint8_t*, size_t) { errno = EWOULDBLOCK; return -1; }) == Writer::PENDING);
    assert(slow.poll(uint32_t(UINT32_MAX - 20 + 10000UL), partial) == Writer::TIMED_OUT);
    slow.release(); // A reconnect discards only this client's unsent frame.
    assert(!slow.pending());
    assert(slow.queue(reply, sizeof(reply), 0));
    assert(slow.poll(1, [](const uint8_t*, size_t) { errno = ECONNRESET; return -1; }) == Writer::FAILED);
    slow.clear();
    assert(slow.queue(reply, sizeof(reply), 0));
    assert(slow.poll(1, [](const uint8_t*, size_t) { return 0; }) == Writer::FAILED);
    slow.release();

    // Exercise actual kernel backpressure with a full non-blocking socket.
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    int sndbuf = 1024;
    assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);
    std::vector<uint8_t> filler(4096, 42), incoming;
    while (send(fds[0], filler.data(), filler.size(), MSG_DONTWAIT) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(slow.queue(expected.data(), expected.size(), 0));
    auto sendReal = [&](const uint8_t* data, size_t length) { return int(send(fds[0], data, length, MSG_DONTWAIT)); };
    assert(slow.poll(1, sendReal) == Writer::PENDING && slow.stalled());
    uint8_t scratch[4096];
    while (recv(fds[1], scratch, sizeof(scratch), MSG_DONTWAIT) > 0) {}
    Writer::Result result;
    uint32_t tick = 2;
    do {
        result = slow.poll(tick++, sendReal);
        int n;
        while ((n = int(recv(fds[1], scratch, sizeof(scratch), MSG_DONTWAIT))) > 0)
            incoming.insert(incoming.end(), scratch, scratch + n);
    } while (result == Writer::PENDING);
    assert(result == Writer::COMPLETE && incoming == expected);
    close(fds[0]); close(fds[1]);
    std::cout << "Partial writes, bounded work, EAGAIN, serialization, timeout/wrap, reconnect and real socket backpressure passed\n";
}
