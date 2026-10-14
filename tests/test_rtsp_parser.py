"""Exercise the actual request parser with pending RTP and pipelined requests."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "CooperativeTcpWriter.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using String = std::string;
struct FakeClient {
    std::string input;
    size_t offset = 0;
    bool connected() const { return true; }
    int available() const { return int(input.size() - offset); }
    int read(uint8_t* data, int size) {
        int n = std::min(size, available());
        memcpy(data, input.data() + offset, n); offset += n; return n;
    }
};
struct ClientSession {
    FakeClient client;
    CooperativeTcpWriter tcpWriter;
    uint8_t parseBuffer[1024] = {0};
    int parseBufferPos = 0;
    uint16_t interleavedBytesRemaining = 0;
};
std::vector<std::string> commands;
void simplePrintln(const String&) { assert(false); }
void handleRTSPCommand(ClientSession& session, String request, uint8_t) {
    commands.push_back(request);
    std::string reply = "RTSP/1.0 200 OK\r\n\r\n";
    assert(session.tcpWriter.queue(reinterpret_cast<const uint8_t*>(reply.data()), reply.size(), 0));
}
'''

MAIN = r'''
int main() {
    ClientSession session;
    session.client.input = std::string("$\1\0\4", 4) + std::string(4, '\0') +
                           "OPTIONS /audio1 RTSP/1.0\r\nCSeq: 1\r\n\r\n"
                           "GET_PARAMETER /audio1 RTSP/1.0\r\nCSeq: 2\r\n\r\n";
    uint8_t rtp[] = {'$', 0, 0, 4, 1, 2, 3, 4};
    assert(session.tcpWriter.queue(rtp, sizeof(rtp), 0));
    processRTSP(session, 0);
    assert(commands.empty() && session.client.offset == 0);
    std::vector<uint8_t> wire;
    auto send = [&](const uint8_t* bytes, size_t size) {
        wire.insert(wire.end(), bytes, bytes + size); return int(size);
    };
    assert(session.tcpWriter.poll(0, send) == CooperativeTcpWriter::COMPLETE);
    session.tcpWriter.clear();
    processRTSP(session, 0);
    assert(commands.size() == 1 && session.tcpWriter.pending());
    assert(session.client.available() == 0 && session.parseBufferPos > 0);
    processRTSP(session, 0);
    assert(commands.size() == 1); // Do not overwrite the queued reply.
    assert(session.tcpWriter.poll(0, send) == CooperativeTcpWriter::COMPLETE);
    session.tcpWriter.clear();
    processRTSP(session, 0); // Buffered request runs even without new socket bytes.
    assert(commands.size() == 2 && commands[1].find("CSeq: 2") != std::string::npos);
    assert(session.tcpWriter.poll(0, send) == CooperativeTcpWriter::COMPLETE);
    session.tcpWriter.clear();
    assert(std::equal(rtp, rtp + sizeof(rtp), wire.begin()));
    assert(std::string(wire.begin() + sizeof(rtp), wire.end()) ==
           "RTSP/1.0 200 OK\r\n\r\nRTSP/1.0 200 OK\r\n\r\n");
    assert(session.parseBufferPos == 0);
    // A large interleaved binary frame must not overflow the text buffer or
    // swallow the next RTSP request, even when input spans several passes.
    session.client.input += std::string("$\1\10\0", 4) + std::string(2048, '\0') +
                            "OPTIONS /audio1 RTSP/1.0\r\nCSeq: 3\r\n\r\n";
    for (int i = 0; i < 20 && commands.size() < 3; ++i) processRTSP(session, 0);
    assert(commands.size() == 3 && commands[2].find("CSeq: 3") != std::string::npos);
    assert(session.interleavedBytesRemaining == 0);
}
'''


class RtspParserTests(unittest.TestCase):
    def test_replies_follow_pending_frame_and_buffered_commands_resume(self):
        source = (ROOT / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').read_text()
        start = source.index('void processRTSP(')
        end = source.index('static int findFreeClientSlot', start)
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / 'parser.cpp'
            binary = pathlib.Path(temp) / 'parser'
            cpp.write_text(HARNESS + source[start:end] + MAIN)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'esp32-birdnet-mic'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
