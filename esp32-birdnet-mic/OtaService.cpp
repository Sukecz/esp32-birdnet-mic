#include "AppRuntime.h"
#include "WebRuntime.h"
#include "OtaService.h"
#include <HTTPClient.h>
#include <Update.h>
#include <errno.h>
#include <stdlib.h>
bool requireMutationAuth(bool preparation = false);
static const char* OFFICIAL_OTA_HOST = "esp32mic.msmeteo.cz";
static const uint32_t OTA_VERSION_CACHE_MS = 10UL * 60UL * 1000UL;
static const uint32_t OTA_VERSION_ERROR_CACHE_MS = 60UL * 60UL * 1000UL;
static bool otaUploadOk = false;
static bool otaUploadBlocked = false;
static String otaUploadError;
static bool otaVersionChecked = false;
static unsigned long otaVersionCheckedAtMs = 0;
static String otaLatestVersion;
static String otaVersionCheckError;
static SemaphoreHandle_t otaVersionMutex = nullptr;
static volatile bool otaVersionCheckRunning = false;

static bool fetchLatestOtaVersion(bool forceRefresh, String &latestOut, String &errorOut);
static bool startOtaVersionCheckAsync();
static void getOtaVersionSnapshot(bool &checkedOut, unsigned long &checkedAtOut,
                                  String &latestOut, String &errorOut, bool &runningOut);
static bool otaVersionCacheFresh(bool checked, unsigned long checkedAt, const String &error);
static bool isNewerFirmwareAvailable(const String &latestVersion);

// Helper functions in main

static String getDefaultOtaUrl() {
    if (!FW_OTA_ARTIFACT_STR || FW_OTA_ARTIFACT_STR[0] == '\0') return String();
    return String("http://") + OFFICIAL_OTA_HOST + "/updates/" + FW_OTA_ARTIFACT_STR;
}

static String getOtaVersionUrl() {
    return String("http://") + OFFICIAL_OTA_HOST + "/updates/ota-version.txt";
}

static String htmlEscape(const String &s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '&') o += F("&amp;");
        else if (c == '<') o += F("&lt;");
        else if (c == '>') o += F("&gt;");
        else if (c == '"') o += F("&quot;");
        else o += c;
    }
    return o;
}

static void sendOtaPage(const String &message = String(), bool ok = true, int httpCode = 200) {
    String preparationError;
    const bool otaAllowed = flashlayout::otaAllowed(preparationError);
    String deviceUrl = "http://" + WiFi.localIP().toString() + "/ota";
    String defaultOtaUrl = getDefaultOtaUrl();
    bool versionChecked = false;
    bool versionChecking = false;
    unsigned long versionCheckedAt = 0;
    String latestVersion;
    String versionError;
    getOtaVersionSnapshot(versionChecked, versionCheckedAt, latestVersion, versionError, versionChecking);
    if (!otaVersionCacheFresh(versionChecked, versionCheckedAt, versionError)) {
        startOtaVersionCheckAsync();
        getOtaVersionSnapshot(versionChecked, versionCheckedAt, latestVersion, versionError, versionChecking);
    }
    bool versionCheckOk = versionChecked && versionError.length() == 0;
    bool updateAvailable = versionCheckOk && isNewerFirmwareAvailable(latestVersion);
    String html;
    html.reserve(8000);
    html += F("<!doctype html><html><head><meta charset='utf-8'>");
    html += F("<link rel=\"icon\" type=\"image/svg+xml\" href=\"data:image/svg+xml,%3Csvg%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%20viewBox%3D%220%200%2032%2032%22%3E%3Crect%20width%3D%2232%22%20height%3D%2232%22%20rx%3D%228%22%20fill%3D%22%230ea5e9%22%2F%3E%3Cpath%20d%3D%22M5%2022l5-7v-3a6%206%200%200%201%2012%200v1l5%202-5%202c-1%207-9%2010-17%205Z%22%20fill%3D%22white%22%2F%3E%3Cpath%20d%3D%22M10%2018q5%205%209-2%22%20fill%3D%22none%22%20stroke%3D%22%230284c7%22%20stroke-width%3D%222%22%20stroke-linecap%3D%22round%22%2F%3E%3Ccircle%20cx%3D%2218.5%22%20cy%3D%2211.5%22%20r%3D%221.2%22%20fill%3D%22%230f172a%22%2F%3E%3C%2Fsvg%3E\">");
    html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += F("<title>Firmware update</title><link rel='stylesheet' href='/ota.css'></head><body data-ota-checking='");
    html += versionChecking ? '1' : '0';
    html += F("' data-ota-allowed='");
    html += otaAllowed ? '1' : '0';
    html += F("'><div id='ota-progress' class='ota-progress' role='alert' aria-live='assertive'><div class='ota-progress-box'><div class='spinner'></div><h2 id='ota-progress-title'>Installing firmware…</h2><p id='ota-progress-text'>Do not disconnect power or close this page.</p><button type='button' id='ota-reconnect' hidden>Check connection again</button></div></div><div class='wrap'>");
    html += F("<div class='card'><h1>Firmware update</h1><p class='muted'>Device: <code>");
    html += htmlEscape(deviceUrl);
    html += F("</code><br>Current firmware: <strong>v");
    html += htmlEscape(String(FW_VERSION_STR));
    html += F("</strong><br>Board: <strong>");
    html += htmlEscape(String(FW_BOARD_NAME_STR));
    html += F("</strong> / ");
    html += htmlEscape(String(FW_CHIP_FAMILY_STR));
    html += F("</p><p><a class='btn secondary' href='/'>Back to Web UI</a></p></div>");
    if (message.length()) {
        html += F("<div class='msg ");
        html += ok ? F("ok") : F("bad");
        html += F("'>");
        html += htmlEscape(message);
        html += F("</div>");
    }
#if FLASH_LAYOUT_TOOL
    html += F("<div class='card' id='update-space' hidden><h2 id='update-space-title'>Prepare for future updates</h2><p class='muted' id='update-space-text'>Make more space for future firmware with one preparation step. Your settings are kept.</p><button type='button' id='update-space-prepare'>Prepare device</button><p class='muted' id='update-space-result' role='status' aria-live='polite'></p></div>");
#endif
    html += F("<p id='ota-preparation-required' class='msg' role='status'");
    if (otaAllowed) html += F(" hidden");
    html += F(">");
    html += htmlEscape(preparationError);
    html += F("</p>");
    html += F("<div class='card'><h2>Automatic update</h2><p class='muted'>Use this when the device has internet access. It downloads the latest board-specific app build from the project web flasher and installs it automatically.</p>");
    if (defaultOtaUrl.length()) {
        if (versionChecking) {
            html += F("<div class='version-state current'>Checking for firmware updates in the background…</div>");
        } else if (!versionCheckOk) {
            html += F("<div class='version-state bad'>Update check failed: ");
            html += htmlEscape(versionError);
            html += F("</div>");
        } else if (updateAvailable) {
            html += F("<div class='version-state update'>New firmware available: v");
            html += htmlEscape(latestVersion);
            html += F("</div><form id='ota-install-form' method='post' action='/ota/install'><button type='submit'");
            if (!otaAllowed) html += F(" disabled");
            html += F(">Download and install v");
            html += htmlEscape(latestVersion);
            html += F("</button></form>");
        } else {
            html += F("<div class='version-state current'><strong>Firmware is up to date.</strong><br><span class='muted'>Latest available: v");
            html += htmlEscape(latestVersion);
            html += F("</span></div>");
        }
        html += F("<p><a class='btn secondary' href='/ota?refresh=1'>Check again</a></p><p class='muted'>Firmware file: <code>");
        html += htmlEscape(defaultOtaUrl);
        html += F("</code></p>");
    } else {
        html += F("<p class='bad'>Automatic update is unavailable for this board profile. Upload a matching app-only firmware file manually.</p>");
    }
    html += F("</div>");
    html += F("<div class='card'><h2>Upload compiled file</h2><p class='muted'>Use this when the device has no internet access. Select the matching app-only file such as <code>firmware-app-c3.bin</code>, <code>firmware-app-s3.bin</code>, <code>firmware-app-c5.bin</code>, or <code>firmware-app-c6.bin</code>. Do not upload the USB <code>firmware.bin</code> merged image here.</p>");
    html += F("<form id='ota-upload-form' method='post' action='/ota/upload' enctype='multipart/form-data'>");
    html += F("<input type='file' name='firmware' accept='.bin,application/octet-stream' required");
    if (!otaAllowed) html += F(" disabled");
    html += F("><button type='submit'");
    if (!otaAllowed) html += F(" disabled");
    html += F(">Upload and install file</button></form></div>");
    html += F("<script src='/ota.js'></script>");
    html += F("</div></body></html>");
    web.sendHeader("Cache-Control", "no-store");
    web.send(httpCode, "text/html; charset=utf-8", html);
}

static bool isLikelyMergedFirmwareName(String filename) {
    filename.toLowerCase();
    int slash = filename.lastIndexOf('/');
    if (slash >= 0) filename = filename.substring(slash + 1);
    slash = filename.lastIndexOf('\\');
    if (slash >= 0) filename = filename.substring(slash + 1);
    if (filename == "firmware.bin") return true;
    if (filename.startsWith("firmware-") && !filename.startsWith("firmware-app-")) return true;
    return false;
}

static bool validateOtaImageSize(size_t imageSize, String &errorOut) {
    if (imageSize == 0) {
        errorOut = F("No firmware data received.");
        return false;
    }
    if (imageSize == 4194304UL || imageSize == 8388608UL) {
        errorOut = F("This looks like a merged USB firmware image. OTA needs the matching app-only firmware-app-*.bin file.");
        return false;
    }
    // Arduino-ESP32 returns the next OTA partition's exact size, unlike the
    // old ESP8266 sketch-space formula. Update validates the image itself.
    uint32_t capacity = ESP.getFreeSketchSpace();
    if (!capacity || imageSize > capacity) {
        errorOut = F("Firmware image is larger than the OTA app partition. Use the matching app-only firmware-app-*.bin file.");
        return false;
    }
    return true;
}

static bool isSafeOtaUrl(const String &url) {
    String expected = getDefaultOtaUrl();
    return expected.length() > 0 && url == expected;
}

static bool isFirmwareContentType(String contentType) {
    int separator = contentType.indexOf(';');
    if (separator >= 0) contentType = contentType.substring(0, separator);
    contentType.trim();
    contentType.toLowerCase();
    return contentType == "application/octet-stream" ||
           contentType == "application/macbinary" ||
           contentType == "binary/octet-stream";
}

static bool parseFirmwareVersion(String version, uint32_t &majorOut, uint32_t &minorOut) {
    version.trim();
    if (version.startsWith("v") || version.startsWith("V")) version.remove(0, 1);
    int dot = version.indexOf('.');
    if (dot <= 0 || dot != version.lastIndexOf('.') || dot >= (int)version.length() - 1) return false;

    String majorPart = version.substring(0, dot);
    String minorPart = version.substring(dot + 1);
    for (size_t i = 0; i < majorPart.length(); i++) {
        if (!isDigit((unsigned char)majorPart[i])) return false;
    }
    for (size_t i = 0; i < minorPart.length(); i++) {
        if (!isDigit((unsigned char)minorPart[i])) return false;
    }

    errno = 0;
    char* end = nullptr;
    unsigned long majorValue = strtoul(majorPart.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || majorValue > UINT32_MAX) return false;
    errno = 0;
    end = nullptr;
    unsigned long minorValue = strtoul(minorPart.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || minorValue > UINT32_MAX) return false;

    majorOut = (uint32_t)majorValue;
    minorOut = (uint32_t)minorValue;
    return true;
}

static int compareFirmwareVersions(const String &left, const String &right, bool &validOut) {
    uint32_t leftMajor = 0, leftMinor = 0, rightMajor = 0, rightMinor = 0;
    validOut = parseFirmwareVersion(left, leftMajor, leftMinor) &&
               parseFirmwareVersion(right, rightMajor, rightMinor);
    if (!validOut) return 0;
    if (leftMajor != rightMajor) return leftMajor > rightMajor ? 1 : -1;
    if (leftMinor != rightMinor) return leftMinor > rightMinor ? 1 : -1;
    return 0;
}

static void ensureOtaVersionMutex() {
    if (!otaVersionMutex) otaVersionMutex = xSemaphoreCreateMutex();
}

static void getOtaVersionSnapshot(bool &checkedOut, unsigned long &checkedAtOut,
                                  String &latestOut, String &errorOut, bool &runningOut) {
    ensureOtaVersionMutex();
    if (otaVersionMutex && xSemaphoreTake(otaVersionMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        checkedOut = otaVersionChecked;
        checkedAtOut = otaVersionCheckedAtMs;
        latestOut = otaLatestVersion;
        errorOut = otaVersionCheckError;
        runningOut = otaVersionCheckRunning;
        xSemaphoreGive(otaVersionMutex);
        return;
    }
    checkedOut = false;
    checkedAtOut = 0;
    latestOut = "";
    errorOut = F("Firmware update status is temporarily busy.");
    runningOut = true;
}

static bool otaVersionCacheFresh(bool checked, unsigned long checkedAt, const String &error) {
    if (!checked) return false;
    uint32_t cacheMs = error.length() == 0 ? OTA_VERSION_CACHE_MS : OTA_VERSION_ERROR_CACHE_MS;
    return (millis() - checkedAt) < cacheMs;
}

static void storeOtaVersionResult(const String &latest, const String &error) {
    ensureOtaVersionMutex();
    if (!otaVersionMutex || xSemaphoreTake(otaVersionMutex, portMAX_DELAY) != pdTRUE) return;
    otaVersionChecked = true;
    otaVersionCheckedAtMs = millis();
    otaLatestVersion = latest;
    otaVersionCheckError = error;
    otaVersionCheckRunning = false;
    xSemaphoreGive(otaVersionMutex);
}

static bool fetchOtaVersionFromNetwork(String &latestOut, String &errorOut) {
    latestOut = "";
    errorOut = "";
    if (!FW_OTA_ARTIFACT_STR || FW_OTA_ARTIFACT_STR[0] == '\0') {
        errorOut = F("Automatic update is unavailable for this board profile.");
    } else {
        WiFiClient client;
        HTTPClient http;
        http.setConnectTimeout(2000);
        http.setTimeout(3000);
        if (!http.begin(client, getOtaVersionUrl())) {
            errorOut = F("Could not open the firmware version URL.");
        } else {
            int code = http.GET();
            if (code != HTTP_CODE_OK) {
                errorOut = String("Firmware version check failed, HTTP ") + String(code);
            } else if (http.getSize() > 32) {
                errorOut = F("Firmware server returned an invalid version response.");
            } else {
                String version = http.getString();
                version.trim();
                uint32_t majorValue = 0, minorValue = 0;
                if (version.length() == 0 || version.length() > 24 ||
                    !parseFirmwareVersion(version, majorValue, minorValue)) {
                    errorOut = F("Firmware server returned an invalid version.");
                } else {
                    latestOut = version;
                }
            }
            http.end();
        }
    }
    return errorOut.length() == 0;
}

static bool fetchLatestOtaVersion(bool forceRefresh, String &latestOut, String &errorOut) {
    bool checked = false;
    bool running = false;
    unsigned long checkedAt = 0;
    getOtaVersionSnapshot(checked, checkedAt, latestOut, errorOut, running);
    if (!forceRefresh && otaVersionCacheFresh(checked, checkedAt, errorOut)) {
        return errorOut.length() == 0;
    }

    bool ok = fetchOtaVersionFromNetwork(latestOut, errorOut);
    storeOtaVersionResult(latestOut, errorOut);
    return ok;
}

static void otaVersionCheckTask(void* /*arg*/) {
    String latest;
    String error;
    fetchOtaVersionFromNetwork(latest, error);
    storeOtaVersionResult(latest, error);
    vTaskDelete(nullptr);
}

static bool startOtaVersionCheckAsync() {
    ensureOtaVersionMutex();
    if (!otaVersionMutex || xSemaphoreTake(otaVersionMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    if (otaVersionCheckRunning) {
        xSemaphoreGive(otaVersionMutex);
        return true;
    }
    otaVersionCheckRunning = true;
    xSemaphoreGive(otaVersionMutex);

    BaseType_t created = xTaskCreate(otaVersionCheckTask, "ota_check", 5120, nullptr, 1, nullptr);
    if (created == pdPASS) return true;
    storeOtaVersionResult("", F("Could not start the firmware update check."));
    return false;
}

static bool isNewerFirmwareAvailable(const String &latestVersion) {
    bool valid = false;
    int comparison = compareFirmwareVersions(latestVersion, String(FW_VERSION_STR), valid);
    return valid && comparison > 0;
}

static bool streamUpdate(NetworkClient &stream, int contentLength, String &errorOut) {
    if (!flashlayout::otaAllowed(errorOut)) return false;
    if (contentLength <= 0) {
        errorOut = F("Firmware server did not provide a valid Content-Length.");
        return false;
    }
    if (!Update.begin((size_t)contentLength)) {
        errorOut = String("Update begin failed: ") + Update.errorString();
        return false;
    }

    uint8_t buffer[1024];
    size_t writtenTotal = 0;
    int remaining = contentLength;
    unsigned long lastDataMs = millis();

    while (remaining > 0) {
        size_t available = stream.available();
        if (available == 0) {
            if (!stream.connected()) {
                errorOut = String("Firmware connection closed after ") + writtenTotal +
                           " of " + contentLength + " bytes.";
                Update.abort();
                return false;
            }
            if (millis() - lastDataMs > 15000) {
                errorOut = F("Update download timed out.");
                Update.abort();
                return false;
            }
            delay(1);
            continue;
        }
        lastDataMs = millis();
        size_t toRead = available;
        if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
        if (toRead > (size_t)remaining) toRead = (size_t)remaining;
        int readLen = stream.readBytes(buffer, toRead);
        if (readLen <= 0) continue;
        size_t written = Update.write(buffer, (size_t)readLen);
        if (written != (size_t)readLen) {
            errorOut = String("Flash write failed: ") + Update.errorString();
            Update.abort();
            return false;
        }
        writtenTotal += written;
        remaining -= readLen;
        yield();
    }

    if (writtenTotal != (size_t)contentLength) {
        errorOut = F("Firmware download size did not match Content-Length.");
        Update.abort();
        return false;
    }
    if (!validateOtaImageSize(writtenTotal, errorOut)) {
        Update.abort();
        return false;
    }
    if (!Update.end()) {
        errorOut = String("Update end failed: ") + Update.errorString();
        return false;
    }
    if (!Update.isFinished()) {
        errorOut = F("Update was not fully written.");
        return false;
    }
    return true;
}

static bool installOtaFromUrl(const String &url, String &errorOut) {
    if (!isSafeOtaUrl(url)) {
        errorOut = F("No matching official automatic OTA image is available for this board profile.");
        return false;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, url)) {
        errorOut = F("Could not open firmware URL.");
        return false;
    }
    const char* headerKeys[] = {"Content-Type"};
    http.collectHeaders(headerKeys, 1);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        errorOut = String("Firmware download failed, HTTP ") + String(code);
        http.end();
        return false;
    }

    if (!isFirmwareContentType(http.header("Content-Type"))) {
        errorOut = F("Firmware server returned an unexpected Content-Type.");
        http.end();
        return false;
    }
    int contentLength = http.getSize();
    if (contentLength <= 0) {
        errorOut = F("Firmware server did not provide a valid Content-Length.");
        http.end();
        return false;
    }
    if (!validateOtaImageSize((size_t)contentLength, errorOut)) {
        http.end();
        return false;
    }
    NetworkClient* stream = http.getStreamPtr();
    if (!stream) {
        errorOut = F("Firmware response stream is unavailable.");
        http.end();
        return false;
    }

    beginOtaUpdate();
    webui_pushLog(String("OTA pull update from ") + url);
    bool ok = streamUpdate(*stream, contentLength, errorOut);
    http.end();
    endOtaUpdate(ok);
    return ok;
}

static void httpOtaPage() {
    if (web.hasArg("refresh") && web.arg("refresh") == "1") {
        startOtaVersionCheckAsync();
    }
    sendOtaPage();
}

static void httpOtaInstall() {
    if (!requireMutationAuth()) return;
    String error;
    if (!flashlayout::otaAllowed(error)) {
        sendOtaPage(error, false, 409);
        return;
    }

    String latestVersion;
    String versionError;
    if (!fetchLatestOtaVersion(true, latestVersion, versionError)) {
        webui_pushLog(String("OTA install blocked: version check failed: ") + versionError);
        sendOtaPage(String("Update blocked: ") + versionError, false);
        return;
    }
    if (!isNewerFirmwareAvailable(latestVersion)) {
        String reason = String("No newer firmware is available. Installed v") + FW_VERSION_STR +
                        ", latest v" + latestVersion + ".";
        webui_pushLog(String("OTA install blocked: ") + reason);
        sendOtaPage(reason, false);
        return;
    }

    String url = getDefaultOtaUrl();
    bool ok = installOtaFromUrl(url, error);
    if (ok) {
        web.sendHeader("X-ESP32MIC-OTA-Reboot", "1");
        sendOtaPage(F("Firmware installed. Device will reboot now."), true);
        scheduleReboot(false, 700);
    } else {
        webui_pushLog(String("OTA pull failed: ") + error);
        sendOtaPage(String("Update failed: ") + error, false);
    }
}

static void httpOtaStatus() {
    bool checked = false;
    bool checking = false;
    unsigned long checkedAt = 0;
    String latestVersion;
    String error;
    getOtaVersionSnapshot(checked, checkedAt, latestVersion, error, checking);
    if (!otaVersionCacheFresh(checked, checkedAt, error)) {
        startOtaVersionCheckAsync();
        getOtaVersionSnapshot(checked, checkedAt, latestVersion, error, checking);
    }
    bool checkOk = checked && error.length() == 0;
    bool updateAvailable = checkOk && isNewerFirmwareAvailable(latestVersion);
    String status = checking ? F("checking")
                             : (checkOk ? (updateAvailable ? F("update_available") : F("up_to_date"))
                                        : F("check_failed"));
    uint32_t checkAgeSec = checked
                               ? (uint32_t)((millis() - checkedAt) / 1000UL)
                               : 0;

    String json;
    json.reserve(320);
    json += F("{\"ok\":");
    json += checkOk ? F("true") : F("false");
    json += F(",\"status\":\"");
    json += status;
    json += F("\",\"current_version\":\"");
    json += jsonEscape(String(FW_VERSION_STR));
    json += F("\",\"latest_version\":\"");
    json += jsonEscape(latestVersion);
    json += F("\",\"update_available\":");
    json += updateAvailable ? F("true") : F("false");
    json += F(",\"checking\":");
    json += checking ? F("true") : F("false");
    json += F(",\"check_age_sec\":");
    json += String(checkAgeSec);
    json += F(",\"error\":\"");
    json += jsonEscape(error);
    json += F("\"}");
    apiSendJSON(json);
}

static void httpOtaUploadDone() {
    if (!requireMutationAuth()) return;

    if (otaUploadOk) {
        web.sendHeader("X-ESP32MIC-OTA-Reboot", "1");
        sendOtaPage(F("Firmware uploaded and installed. Device will reboot now."), true);
        scheduleReboot(false, 700);
    } else {
        endOtaUpdate(false);
        sendOtaPage(String("Upload failed: ") + otaUploadError, false, otaUploadBlocked ? 409 : 200);
    }
}

static void httpOtaUploadChunk() {
    HTTPUpload &upload = web.upload();
    if (upload.status == UPLOAD_FILE_START) {
        otaUploadOk = false;
        otaUploadBlocked = false;
        otaUploadError = "";
        if (!hasMutationAuth()) {
            otaUploadError = F("forbidden");
            Update.abort();
            return;
        }
        if (recording::busy()) {
            otaUploadError = F("Stop and save recording before updating firmware.");
            otaUploadBlocked = true;
            return;
        }
        if (flashlayout::busy()) {
            otaUploadError = F("Update preparation is in progress. Please wait.");
            return;
        }
        if (isLikelyMergedFirmwareName(upload.filename)) {
            otaUploadError = F("This looks like a merged USB firmware image. OTA needs the matching app-only firmware-app-*.bin file.");
            Update.abort();
            return;
        }
        if (!flashlayout::otaAllowed(otaUploadError)) {
            otaUploadBlocked = true;
            return;
        }
        beginOtaUpdate();
        webui_pushLog(String("OTA upload start: ") + upload.filename);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            otaUploadError = String("Update begin failed: ") + Update.errorString();
            endOtaUpdate(false);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (otaUploadError.length() == 0) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                otaUploadError = String("Flash write failed: ") + Update.errorString();
                Update.abort();
                endOtaUpdate(false);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (otaUploadError.length() == 0) {
            if (!validateOtaImageSize(upload.totalSize, otaUploadError)) {
                Update.abort();
            } else if (!Update.end(true)) {
                otaUploadError = String("Update end failed: ") + Update.errorString();
            } else if (!Update.isFinished()) {
                otaUploadError = F("Update was not fully written.");
            } else {
                otaUploadOk = true;
                webui_pushLog(String("OTA upload installed, bytes=") + String(upload.totalSize));
            }
        }
        endOtaUpdate(otaUploadOk);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        otaUploadError = F("Upload aborted.");
        Update.abort();
        endOtaUpdate(false);
    }
}


void registerOtaRoutes() {
    web.on("/ota", HTTP_GET, httpOtaPage);
    web.on("/ota/install", HTTP_POST, httpOtaInstall);
    web.on("/ota/upload", HTTP_POST, httpOtaUploadDone, httpOtaUploadChunk);
    web.on("/api/ota_status", HTTP_GET, httpOtaStatus);
}
