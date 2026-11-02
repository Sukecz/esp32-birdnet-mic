#include "AppRuntime.h"
#include "RtspService.h"
#include "MqttService.h"
#include "CooperativeTcpWriter.h"
#include "StreamHealth.h"
#include <WiFiUdp.h>
#include <sys/socket.h>
#include "freertos/ringbuf.h"
enum TransportMode : uint8_t {
    TRANSPORT_TCP = 0,
    TRANSPORT_UDP = 1
};







struct ClientSession {
    WiFiClient client;
    WiFiUDP udpSocket;
    WiFiUDP rtcpSocket;
    TransportMode transport = TRANSPORT_TCP;
    IPAddress clientRtpAddress = IPAddress();
    uint16_t clientRtpPort = 0;
    uint16_t serverRtpPort = 0;
    String sessionId = "";
    bool streaming = false;
    uint8_t profileIndex = 0;  // 0 => /audio1 (alias /audio), 1 => /audio2
    uint16_t rtpSequence = 0;
    uint32_t rtpTimestamp = 0;
    unsigned long lastActivity = 0;
    unsigned long connectedAtMs = 0;
    unsigned long playStartedAtMs = 0;
    unsigned long lastRtpPacketMs = 0;
    char userAgent[64] = {0};
    uint8_t parseBuffer[1024] = {0};
    int parseBufferPos = 0;
    uint16_t interleavedBytesRemaining = 0;
    unsigned long packetsSent = 0;
    CooperativeTcpWriter tcpWriter;
    bool tcpFrameIsRtp = false;
    uint16_t tcpFrameSamples = 0;
    uint32_t tcpFrameChunk = 0;
    uint32_t tcpFrameDropsAtQueue = 0;

    void reset() {
        tcpWriter.release();
        tcpFrameIsRtp = false;
        tcpFrameSamples = 0;
        tcpFrameChunk = 0;
        tcpFrameDropsAtQueue = 0;
        client.stop();
        if (transport == TRANSPORT_UDP) {
            udpSocket.stop();
            rtcpSocket.stop();
        }
        transport = TRANSPORT_TCP;
        clientRtpPort = 0;
        serverRtpPort = 0;
        sessionId = "";
        streaming = false;
        profileIndex = 0;
        rtpSequence = 0;
        rtpTimestamp = 0;
        lastActivity = 0;
        connectedAtMs = 0;
        playStartedAtMs = 0;
        lastRtpPacketMs = 0;
        userAgent[0] = '\0';
        parseBufferPos = 0;
        interleavedBytesRemaining = 0;
        parseBuffer[0] = '\0';
        packetsSent = 0;
    }
};

static ClientSession clients[MAX_CLIENTS];
static uint32_t audioChunkSequence = 0;
static uint32_t countedAudioChunk = 0;
static uint32_t countedProfileChunk[2] = {0, 0};
WiFiServer rtspServer(8554);
WiFiClient rtspClient;

bool hasActiveRtspStream() {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) return true;
    }
    return false;
}

static bool anotherSessionStreaming(const ClientSession &session) {
    for (uint8_t i = 0; i < MAX_CLIENTS; ++i) {
        if (&clients[i] != &session && clients[i].streaming) return true;
    }
    return false;
}

static void recordRtpSent(ClientSession &session, uint16_t samples, uint32_t chunk) {
    session.rtpSequence++;
    session.rtpTimestamp += samples;
    session.packetsSent++;
    session.lastRtpPacketMs = millis();
    lastRtpPacketMs = session.lastRtpPacketMs;
    // A chunk delivered to multiple clients is counted once per profile.
    // Delayed copies from a slower client must not count the same chunk again.
    uint8_t pi = session.profileIndex;
    if ((int32_t)(chunk - countedProfileChunk[pi]) > 0) {
        countedProfileChunk[pi] = chunk;
        streamStats[pi].packetsSent++;
    }
    if ((int32_t)(chunk - countedAudioChunk) > 0) {
        countedAudioChunk = chunk;
        audioPacketsSent++;
    }
}

static void stopStreamOnWriteFailure(ClientSession &session, const char* reason) {
    uint8_t pi = session.profileIndex;
    session.streaming = false;
    rtspWriteFailCount++;
    recordStreamStop(reason, pi + 1);
    bool otherStreaming = anotherSessionStreaming(session);
    session.reset();
    // A slow/failed client must not discard audio queued for other clients.
    if (!otherStreaming) audioRingBufferFlushPending = true;
    bool anyStillStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming && clients[i].profileIndex == pi) { anyStillStreaming = true; break; }
    }
    if (!anyStillStreaming) streamStats[pi].streaming = false;

    simplePrintln("STREAMING STOPPED: " + lastStreamStopReason + " | client dropped");
    mqttPublishState(true);
}

static void servicePendingTcp(ClientSession &session) {
    if (!session.tcpWriter.pending()) return;
    uint32_t now = millis();
    bool wasStalled = session.tcpWriter.stalled();
    auto result = session.tcpWriter.poll(now, [&](const uint8_t* data, size_t length) {
        int fd = session.client.fd();
        if (!session.client.connected() || fd < 0) { errno = ENOTCONN; return -1; }
        return (int)send(fd, data, length, MSG_DONTWAIT);
    });
    if (!wasStalled && session.tcpWriter.stalled()) rtspWriteStallCount++;
    if (result == CooperativeTcpWriter::TIMED_OUT) rtspWriteTimeoutCount++;
    if (result == CooperativeTcpWriter::FAILED || result == CooperativeTcpWriter::TIMED_OUT) {
        stopStreamOnWriteFailure(session, session.tcpFrameIsRtp ? "RTP write failed" : "RTSP write failed");
        return;
    }
    if (result != CooperativeTcpWriter::COMPLETE) return;
    if (shouldFlushRecoveredBacklog(session.tcpWriter.stalled(), session.streaming,
                                    anotherSessionStreaming(session),
                                    session.tcpFrameDropsAtQueue,
                                    audioRingBufferDropCount.load(std::memory_order_relaxed))) {
        audioRingBufferFlushPending = true;
    }
    if (session.tcpFrameIsRtp) recordRtpSent(session, session.tcpFrameSamples, session.tcpFrameChunk);
    session.tcpWriter.clear();
    session.tcpFrameIsRtp = false;
}

static void queueRtspResponse(ClientSession &session, const String &response) {
    session.tcpFrameIsRtp = false;
    session.tcpFrameDropsAtQueue = audioRingBufferDropCount.load(std::memory_order_relaxed);
    if (!session.tcpWriter.queue((const uint8_t*)response.c_str(), response.length(), millis())) {
        stopStreamOnWriteFailure(session, "RTSP write failed");
        return;
    }
    servicePendingTcp(session);
}

void sendRTPPacket(ClientSession &session, const int16_t* networkAudioData, int numSamples, uint32_t chunk) {
    if (session.tcpWriter.pending()) return;
    const uint16_t payloadSize = (uint16_t)(numSamples * (int)sizeof(int16_t));
    const uint16_t packetSize = (uint16_t)(12 + payloadSize);

    uint8_t header[12];
    header[0] = 0x80;
    header[1] = 96;
    header[2] = (uint8_t)((session.rtpSequence >> 8) & 0xFF);
    header[3] = (uint8_t)(session.rtpSequence & 0xFF);
    header[4] = (uint8_t)((session.rtpTimestamp >> 24) & 0xFF);
    header[5] = (uint8_t)((session.rtpTimestamp >> 16) & 0xFF);
    header[6] = (uint8_t)((session.rtpTimestamp >> 8) & 0xFF);
    header[7] = (uint8_t)(session.rtpTimestamp & 0xFF);
    header[8]  = (uint8_t)((rtpSSRC >> 24) & 0xFF);
    header[9]  = (uint8_t)((rtpSSRC >> 16) & 0xFF);
    header[10] = (uint8_t)((rtpSSRC >> 8) & 0xFF);
    header[11] = (uint8_t)(rtpSSRC & 0xFF);

    bool success = false;
    if (session.transport == TRANSPORT_TCP) {
        if (!session.client.connected()) {
            session.streaming = false;
            return;
        }
        if (!rtpPacketScratch) {
            stopStreamOnWriteFailure(session, "RTP write failed (no scratch buffer)");
            return;
        }
        rtpPacketScratch[0] = 0x24;
        rtpPacketScratch[1] = 0x00;
        rtpPacketScratch[2] = (uint8_t)((packetSize >> 8) & 0xFF);
        rtpPacketScratch[3] = (uint8_t)(packetSize & 0xFF);
        memcpy(rtpPacketScratch + 4, header, sizeof(header));
        memcpy(rtpPacketScratch + 4 + sizeof(header), networkAudioData, payloadSize);
        size_t totalLen = 4 + sizeof(header) + payloadSize;
        if (!session.tcpWriter.queue(rtpPacketScratch, totalLen, millis())) {
            stopStreamOnWriteFailure(session, "RTP write failed");
            return;
        }
        session.tcpFrameIsRtp = true;
        session.tcpFrameSamples = (uint16_t)numSamples;
        session.tcpFrameChunk = chunk;
        session.tcpFrameDropsAtQueue = audioRingBufferDropCount.load(std::memory_order_relaxed);
        servicePendingTcp(session);
        return;
    } else {
        if (session.clientRtpPort == 0) {
            session.streaming = false;
            return;
        }
        session.udpSocket.beginPacket(session.clientRtpAddress, session.clientRtpPort);
        session.udpSocket.write(header, sizeof(header));
        session.udpSocket.write((const uint8_t*)networkAudioData, payloadSize);
        if (session.udpSocket.endPacket()) {
            success = true;
        }
    }
    if (success) {
        recordRtpSent(session, (uint16_t)numSamples, chunk);
    }
}

static void drainRtcpPackets(ClientSession &session) {
    if (session.transport != TRANSPORT_UDP) return;
    uint8_t buffer[64];
    int packetSize = session.rtcpSocket.parsePacket();
    while (packetSize > 0) {
        while (packetSize > 0) {
            int toRead = packetSize;
            if (toRead > (int)sizeof(buffer)) toRead = sizeof(buffer);
            int readNow = session.rtcpSocket.read(buffer, toRead);
            if (readNow <= 0) break;
            packetSize -= readNow;
        }
        packetSize = session.rtcpSocket.parsePacket();
    }
}

// Audio streaming
void streamAudio() {
    bool anyStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) { anyStreaming = true; break; }
    }
    if (!anyStreaming) return;
    if (!audioRingBuffer) return;

    if (audioRingBufferFlushPending) {
        audioRingBufferFlushPending = false;
        flushAudioRingBuffer();
    }
    uint8_t chunksProcessed = 0;
    while (chunksProcessed < 4) {
        bool ready = false;
        for (uint8_t i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].streaming && !clients[i].tcpWriter.pending()) { ready = true; break; }
        }
        if (!ready) break;
        size_t itemSize = 0;
        // Byte buffers merge adjacent producer writes. Only borrow the bytes
        // this RTP packet can carry, so returning it cannot discard queued audio.
        int16_t* audioChunk = (int16_t*)xRingbufferReceiveUpTo(
            audioRingBuffer, &itemSize, 0,
            (size_t)currentBufferSize * sizeof(int16_t));
        if (!audioChunk) break;

        int samplesRead = itemSize / sizeof(int16_t);
        if (samplesRead > currentBufferSize) samplesRead = currentBufferSize;

        // Prepare one network-order copy and reuse for all active client sessions.
        for (int i = 0; i < samplesRead; ++i) {
            uint16_t s = (uint16_t)audioChunk[i];
            s = (uint16_t)((s << 8) | (s >> 8));
            i2s_16bit_network_buffer[i] = (int16_t)s;
        }
        uint32_t chunk = ++audioChunkSequence;
        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].streaming || clients[i].tcpWriter.pending()) continue;
            sendRTPPacket(clients[i], i2s_16bit_network_buffer, samplesRead, chunk);
        }
        vRingbufferReturnItem(audioRingBuffer, (void*)audioChunk);
        if (audioRingBufferFlushPending) {
            audioRingBufferFlushPending = false;
            flushAudioRingBuffer();
            break;
        }
        chunksProcessed++;
    }
}

static uint8_t detectProfileFromRequest(const String &request) {
    int lineEnd = request.indexOf("\r\n");
    String firstLine = (lineEnd > 0) ? request.substring(0, lineEnd) : request;
    if (firstLine.indexOf("/audio2") >= 0) return 1;
    return 0;
}

static String rtspHeaderValue(const String &request, const char *headerName) {
    String marker = String(headerName) + ":";
    int start = request.indexOf(marker);
    if (start < 0) return String();
    start += marker.length();
    while (start < (int)request.length() && (request[start] == ' ' || request[start] == '\t')) start++;
    int end = request.indexOf("\r\n", start);
    if (end < 0) end = request.length();
    String value = request.substring(start, end);
    value.trim();
    return value;
}

static bool anyRtspSessionStreaming(uint8_t profileIndex = 255) {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].streaming) continue;
        if (profileIndex == 255 || clients[i].profileIndex == profileIndex) return true;
    }
    return false;
}

static void parseTransportHeader(ClientSession &session, const String &request, String &transportResponse, uint8_t clientIdx) {
    // Auto-select transport based on target: BirdNET-Go=TCP, BirdNET-Pi=UDP
    bool forceTcp = (streamProfiles[session.profileIndex].target == STREAM_TARGET_BIRDNET_GO);
    bool forceUdp = !forceTcp;

    if (forceUdp) {
        session.transport = TRANSPORT_UDP;
        int transportPos = request.indexOf("Transport:");
        if (transportPos >= 0) {
            String transportLine = request.substring(transportPos);
            int endPos = transportLine.indexOf("\r\n");
            if (endPos > 0) transportLine = transportLine.substring(0, endPos);
            transportLine.toLowerCase();
            int clientPortPos = transportLine.indexOf("client_port=");
            if (clientPortPos >= 0) {
                String portStr = transportLine.substring(clientPortPos + 12);
                int dashPos = portStr.indexOf("-");
                if (dashPos > 0) portStr = portStr.substring(0, dashPos);
                int semiPos = portStr.indexOf(";");
                if (semiPos > 0) portStr = portStr.substring(0, semiPos);
                portStr.trim();
                session.clientRtpPort = (uint16_t)portStr.toInt();
            }
        }
        if (session.client.connected()) session.clientRtpAddress = session.client.remoteIP();
        session.serverRtpPort = (uint16_t)(5004 + (clientIdx * 2));
        session.udpSocket.stop();
        session.rtcpSocket.stop();
        session.udpSocket.begin(session.serverRtpPort);
        session.rtcpSocket.begin(session.serverRtpPort + 1);
        transportResponse = "RTP/AVP/UDP;unicast;client_port=" + String(session.clientRtpPort) + "-" + String(session.clientRtpPort + 1);
        transportResponse += ";server_port=" + String(session.serverRtpPort) + "-" + String(session.serverRtpPort + 1);
        transportResponse += ";source=" + WiFi.localIP().toString();
        transportResponse += ";ssrc=" + String(rtpSSRC, HEX);
    } else {
        session.transport = TRANSPORT_TCP;
        transportResponse = "RTP/AVP/TCP;unicast;interleaved=0-1";
    }
}

// RTSP handling
void handleRTSPCommand(ClientSession &session, String request, uint8_t clientIdx) {
    String response;
    response.reserve(512);
    String cseq = "1";
    int cseqPos = request.indexOf("CSeq: ");
    if (cseqPos >= 0) {
        cseq = request.substring(cseqPos + 6, request.indexOf("\r", cseqPos));
        cseq.trim();
    }
    session.profileIndex = detectProfileFromRequest(request);
    String requestUserAgent = rtspHeaderValue(request, "User-Agent");
    if (requestUserAgent.length()) {
        requestUserAgent.replace("\r", "");
        requestUserAgent.replace("\n", "");
        requestUserAgent.toCharArray(session.userAgent, sizeof(session.userAgent));
    }

    // Reject disabled streams
    if (!streamEnabled[session.profileIndex]) {
        response += "RTSP/1.0 454 Session Not Found\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "\r\n";
        simplePrintln("Rejected disabled stream" + String(session.profileIndex + 1) + " from " + session.client.remoteIP().toString());
        queueRtspResponse(session, response);
        return;
    }

    int methodEnd = request.indexOf(' ');
    if (methodEnd <= 0) methodEnd = request.length();
    lastRtspCommand = request.substring(0, methodEnd);
    lastRtspCommandMs = millis();
    lastRTSPActivity = lastRtspCommandMs;

    if (request.startsWith("OPTIONS")) {
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n";

    } else if (request.startsWith("DESCRIBE")) {
        String ip = WiFi.localIP().toString();
        String streamPath = (session.profileIndex == 1) ? "/audio2" : "/audio1";
        String sdp = "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 " + ip + "\r\n";
        sdp += "s=ESP32 RTSP Mic " + streamPath + " (" + String(currentSampleRate) + "Hz, 16-bit PCM)\r\n";
        sdp += "c=IN IP4 " + ip + "\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=audio 0 RTP/AVP 96\r\n";
        sdp += "a=rtpmap:96 L16/" + String(currentSampleRate) + "/1\r\n";
        sdp += "a=control:track1\r\n";

        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Content-Type: application/sdp\r\n";
        response += "Content-Base: rtsp://" + ip + ":8554" + streamPath + "/\r\n";
        response += "Content-Length: " + String(sdp.length()) + "\r\n\r\n";
        response += sdp;

    } else if (request.startsWith("SETUP")) {
        session.sessionId = String(random(100000000, 999999999));
        rtspSessionId = session.sessionId;
        String transportResponse;
        parseTransportHeader(session, request, transportResponse, clientIdx);
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Session: " + session.sessionId + "\r\n";
        response += "Transport: " + transportResponse + "\r\n\r\n";

    } else if (request.startsWith("PLAY")) {
        String ip = WiFi.localIP().toString();
        String streamPath = (session.profileIndex == 1) ? "/audio2" : "/audio1";
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Session: " + session.sessionId + "\r\n";
        response += "Range: npt=0.000-\r\n";
        response += "RTP-Info: url=rtsp://" + ip + ":8554" + streamPath + "/track1;seq=0;rtptime=0\r\n\r\n";

        bool wasAnyStreaming = anyRtspSessionStreaming();
        bool wasProfileStreaming = anyRtspSessionStreaming(session.profileIndex);
        if (!wasAnyStreaming) {
            flushAudioRingBuffer();
        }
        session.streaming = true;
        session.rtpSequence = 0;
        session.rtpTimestamp = 0;
        session.packetsSent = 0;
        session.playStartedAtMs = millis();
        session.lastRtpPacketMs = session.playStartedAtMs;
        if (!wasAnyStreaming) {
            audioPacketsSent = 0;
            lastStatsReset = millis();
            recentAudioRate.reset(lastStatsReset, audioPacketsSent);
            streamStartedAtMs = millis();
        }
        lastRtspPlayMs = millis();
        streamStats[session.profileIndex].streaming = true;
        streamStats[session.profileIndex].lastPlayMs = millis();
        if (!wasProfileStreaming) {
            streamStats[session.profileIndex].packetsSent = 0;
            streamStats[session.profileIndex].statsResetMs = millis();
            recentStreamRate[session.profileIndex].reset(
                streamStats[session.profileIndex].statsResetMs, 0);
        }
        rtspPlayCount++;
        lastRtpPacketMs = millis();
        simplePrintln("STREAMING STARTED stream" + String(session.profileIndex + 1));
        mqttPublishState(true);

    } else if (request.startsWith("TEARDOWN")) {
        unsigned long now = millis();
        IPAddress remoteIp = session.client.remoteIP();
        unsigned long sessionAgeMs = session.connectedAtMs ? now - session.connectedAtMs : 0;
        unsigned long streamAgeMs = session.playStartedAtMs ? now - session.playStartedAtMs : 0;
        unsigned long lastRtpAgeMs = session.lastRtpPacketMs ? now - session.lastRtpPacketMs : 0;
        String teardownLog;
        teardownLog.reserve(260);
        teardownLog += F("RTSP TEARDOWN: client=");
        teardownLog += remoteIp.toString();
        teardownLog += F(" slot=");
        teardownLog += String(clientIdx);
        teardownLog += F(" stream=");
        teardownLog += String(session.profileIndex + 1);
        teardownLog += F(" transport=");
        teardownLog += session.transport == TRANSPORT_UDP ? F("UDP") : F("TCP");
        teardownLog += F(" cseq=");
        teardownLog += cseq;
        teardownLog += F(" session_age_ms=");
        teardownLog += String(sessionAgeMs);
        teardownLog += F(" stream_age_ms=");
        teardownLog += String(streamAgeMs);
        teardownLog += F(" packets=");
        teardownLog += String(session.packetsSent);
        teardownLog += F(" last_rtp_ms=");
        teardownLog += String(lastRtpAgeMs);
        teardownLog += F(" rssi=");
        teardownLog += String(WiFi.RSSI());
        teardownLog += F("dBm user_agent=");
        teardownLog += session.userAgent[0] ? session.userAgent : "unknown";
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n";
        response += "Session: " + session.sessionId + "\r\n\r\n";
        bool wasStreaming = session.streaming;
        session.streaming = false;
        bool anyStillStreaming = false;
        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (i != clientIdx && clients[i].streaming && clients[i].profileIndex == session.profileIndex) {
                anyStillStreaming = true; break;
            }
        }
        if (!anyStillStreaming) streamStats[session.profileIndex].streaming = false;
        rtspTeardownCount++;
        if (wasStreaming) recordStreamStop("Client requested TEARDOWN", session.profileIndex + 1);
        simplePrintln(teardownLog);
        if (wasStreaming) simplePrintln("STREAMING STOPPED (" + lastStreamStopReason + ")");
        mqttPublishState(true);
    } else if (request.startsWith("GET_PARAMETER")) {
        response += "RTSP/1.0 200 OK\r\n";
        response += "CSeq: " + cseq + "\r\n\r\n";
    } else {
        response += "RTSP/1.0 501 Not Implemented\r\n";
        response += "CSeq: " + cseq + "\r\n\r\n";
        simplePrintln("RTSP unsupported command: " + lastRtspCommand);
    }
    queueRtspResponse(session, response);
}

// RTSP processing
void processRTSP(ClientSession &session, uint8_t clientIdx) {
    if (!session.client.connected() || session.tcpWriter.pending()) return;
    // Bound input work too. TCP RTCP frames can contain NUL bytes or exceed
    // the text buffer; discard their declared binary length before parsing.
    uint8_t passes = 0;
    while (passes++ < 8) {
        if (session.interleavedBytesRemaining && session.parseBufferPos) {
            int skip = session.parseBufferPos;
            if (skip > session.interleavedBytesRemaining) skip = session.interleavedBytesRemaining;
            session.interleavedBytesRemaining -= skip;
            session.parseBufferPos -= skip;
            memmove(session.parseBuffer, session.parseBuffer + skip, session.parseBufferPos);
            session.parseBuffer[session.parseBufferPos] = '\0';
            continue;
        }
        if (!session.interleavedBytesRemaining && session.parseBufferPos >= 4 && session.parseBuffer[0] == '$') {
            session.interleavedBytesRemaining = (uint16_t(session.parseBuffer[2]) << 8) | session.parseBuffer[3];
            session.parseBufferPos -= 4;
            memmove(session.parseBuffer, session.parseBuffer + 4, session.parseBufferPos);
            session.parseBuffer[session.parseBufferPos] = '\0';
            continue;
        }
        char* endOfHeader = (!session.interleavedBytesRemaining && session.parseBuffer[0] != '$')
            ? strstr((char*)session.parseBuffer, "\r\n\r\n") : nullptr;
        if (endOfHeader) {
            int headerLen = (endOfHeader - (char*)session.parseBuffer) + 4;
            *endOfHeader = '\0';
            String request((char*)session.parseBuffer);
            int remaining = session.parseBufferPos - headerLen;
            if (remaining > 0) memmove(session.parseBuffer, session.parseBuffer + headerLen, remaining);
            session.parseBufferPos = remaining;
            session.parseBuffer[remaining] = '\0';
            handleRTSPCommand(session, request, clientIdx);
            if (!session.client.connected() || session.tcpWriter.pending()) return;
            continue;
        }
        int available = session.client.available();
        if (available <= 0) return;
        int room = sizeof(session.parseBuffer) - session.parseBufferPos - 1;
        if (room <= 0) {
            simplePrintln("RTSP buffer overflow - resetting");
            session.parseBufferPos = 0;
            session.parseBuffer[0] = '\0';
            return;
        }
        if (available > room) available = room;
        int bytesRead = session.client.read(session.parseBuffer + session.parseBufferPos, available);
        if (bytesRead <= 0) return;
        session.parseBufferPos += bytesRead;
        session.parseBuffer[session.parseBufferPos] = '\0';
    }
}

static int findFreeClientSlot() {
    for (uint8_t i = 0; i < maxActiveClients; i++) {
        if (!clients[i].client.connected()) return (int)i;
    }
    return -1;
}

void getStreamClientCounts(uint8_t &s1, uint8_t &s2) {
    s1 = 0; s2 = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected()) {
            if (clients[i].profileIndex == 1) s2++; else s1++;
        }
    }
}

uint8_t getRtspClientCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected()) count++;
    }
    return count;
}

String getRtspClientSummary() {
    String summary;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].client.connected()) continue;
        if (summary.length()) summary += ",";
        summary += clients[i].client.remoteIP().toString();
        summary += "/audio";
        summary += String(clients[i].profileIndex + 1);
    }
    return summary;
}

void stopRtspClientsForStream(uint8_t profileIndex, const char* reason) {
    if (profileIndex >= 2) return;
    bool hadStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].profileIndex != profileIndex) continue;
        if (clients[i].streaming) hadStreaming = true;
        clients[i].reset();
    }
    streamStats[profileIndex].streaming = false;
    streamStats[profileIndex].clientCount = 0;
    isStreaming = anyRtspSessionStreaming();
    if (!isStreaming) {
        rtspClient = WiFiClient();
        rtspSessionId = "";
    }
    if (hadStreaming && reason && reason[0]) {
        recordStreamStop(reason, profileIndex + 1);
    }
}

void stopAllRtspClients(const char* reason) {
    browserAudioStop();
    bool hadStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) hadStreaming = true;
        clients[i].reset();
    }
    for (uint8_t i = 0; i < 2; i++) {
        streamStats[i].streaming = false;
        streamStats[i].clientCount = 0;
    }
    rtspClient = WiFiClient();
    rtspSessionId = "";
    isStreaming = false;
    if (hadStreaming && reason && reason[0]) {
        recordStreamStop(reason, 0);
    }
}

void refreshLegacyRtspState() {
    isStreaming = false;
    rtspClient = WiFiClient();
    rtspSessionId = "";
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected() && !rtspClient.connected()) {
            rtspClient = clients[i].client;
            lastRtspClientIp = rtspClient.remoteIP().toString();
        }
        if (clients[i].streaming) {
            isStreaming = true;
            if (rtspSessionId.length() == 0) rtspSessionId = clients[i].sessionId;
        }
    }
}



void serviceRtspClients() {

        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            // WiFiClient::operator bool() calls connected(); do not combine it
            // with !connected(), which would miss established disconnections.
            if ((clients[i].connectedAtMs != 0 || clients[i].streaming) && !clients[i].client.connected()) {
                bool wasStreaming = clients[i].streaming;
                if (wasStreaming) {
                    rtspDisconnectCount++;
                    recordStreamStop("TCP client disconnected", clients[i].profileIndex + 1);
                }
                clients[i].reset();
                if (wasStreaming) mqttPublishState(true);
            }
            if (clients[i].client.connected() && !clients[i].streaming) {
                if (millis() - clients[i].lastActivity > 30000UL) {
                    clients[i].reset();
                }
            }
        }

        WiFiClient newClient = rtspServer.accept();
        if (newClient) {
            int slot = findFreeClientSlot();
            if (slot >= 0) {
                clients[slot].client = newClient;
                clients[slot].client.setNoDelay(true);
                clients[slot].lastActivity = millis();
                clients[slot].connectedAtMs = clients[slot].lastActivity;
                clients[slot].playStartedAtMs = 0;
                clients[slot].lastRtpPacketMs = 0;
                clients[slot].userAgent[0] = '\0';
                clients[slot].parseBufferPos = 0;
                lastRtspClientConnectMs = millis();
                rtspConnectCount++;
                simplePrintln("New RTSP client connected in slot " + String(slot) + " from: " + newClient.remoteIP().toString());
                mqttPublishState(true);
            } else {
                newClient.stop();
                simplePrintln("RTSP client rejected: max sessions reached");
            }
        }

        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].client.connected()) {
                servicePendingTcp(clients[i]);
                if (!clients[i].client.connected()) continue;
                drainRtcpPackets(clients[i]);
                if (clients[i].client.available()) {
                    clients[i].lastActivity = millis();
                    lastRTSPActivity = clients[i].lastActivity;
                }
                processRTSP(clients[i], i);
            }
        }
        streamAudio();
}
