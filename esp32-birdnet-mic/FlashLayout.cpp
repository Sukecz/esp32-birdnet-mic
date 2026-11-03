#include "FlashLayout.h"
#if FLASH_LAYOUT_TOOL
#include "FlashLayoutTable.h"
#include <Preferences.h>
#include <esp_flash.h>
#include <esp_flash_chips/esp_flash_types.h> // IDF 6 no longer exports chip internals via esp_flash.h
#include <esp_flash_encrypt.h>
#include <esp_secure_boot.h>
#include <esp_ota_ops.h>
#include <esp_image_format.h>
#include <esp_rom_md5.h>
#include <esp_flash_partitions.h>

extern bool otaInProgress;
extern volatile unsigned long scheduledRebootAt;
extern void beginOtaUpdate();
extern void endOtaUpdate(bool installed);
extern void scheduleReboot(bool factoryReset, uint32_t delayMs);
extern void webui_pushLog(const String& line);

namespace flashlayout {
using namespace flashtable;
static bool queued = false, working = false, recovery = false;
static unsigned long startAt = 0;
static String lastError;
// Retain the original table only if both the new write and restoration failed.
// Keep the running board alive and permit retry instead of scheduling a reboot.
static uint8_t* recoveryTable = nullptr;

struct Buffer {
    uint8_t* p;
    explicit Buffer(size_t size) : p(static_cast<uint8_t*>(malloc(size))) {}
    ~Buffer() { free(p); }
};

static void md5(const uint8_t* bytes, uint32_t length, uint8_t digest[16]) {
    md5_context_t context;
    esp_rom_md5_init(&context);
    esp_rom_md5_update(&context, bytes, length);
    esp_rom_md5_final(digest, &context);
}

static bool supported() {
    uint32_t size = 0;
    return esp_flash_get_size(nullptr, &size) == ESP_OK && size == 4 * 1024 * 1024 &&
           !esp_flash_encryption_enabled() && !esp_secure_boot_enabled();
}

static Layout detect() {
    if (!supported()) return Unknown;
    Buffer bytes(kSectorSize), scratch(kSectorSize);
    if (!bytes.p || !scratch.p || esp_flash_read(nullptr, bytes.p, kTableAddress, kSectorSize) != ESP_OK)
        return Unknown;
    return classify(bytes.p, scratch.p, md5);
}

static bool savePending(bool pending) {
    Preferences prefs;
    if (!prefs.begin("flashlay", false)) return false;
    bool ok = prefs.putBool("pending", pending) == 1;
    prefs.end();
    return ok;
}

// Bypass the SDK guard only for the partition-table sector. Preserve the
// original guard for all other addresses, including other tasks' writes.
static esp_err_t (*savedGuard)(void*, size_t, size_t) = nullptr;
static esp_err_t tableGuard(void* arg, size_t address, size_t size) {
    if (address == kTableAddress && size <= kSectorSize) return ESP_OK;
    return savedGuard ? savedGuard(arg, address, size) : ESP_ERR_INVALID_ARG;
}
struct TableDriver {
    esp_flash_t* chip = esp_flash_default_chip;
    const esp_flash_os_functions_t* saved;
    esp_flash_os_functions_t scoped;
    TableDriver() : saved(chip->os_func), scoped(*saved) {
        savedGuard = saved->region_protected;
        scoped.region_protected = tableGuard;
        chip->os_func = &scoped;
    }
    ~TableDriver() { chip->os_func = saved; savedGuard = nullptr; }
    bool erase() { return esp_flash_erase_region(chip, kTableAddress, kSectorSize) == ESP_OK; }
    bool write(const uint8_t* bytes) { return esp_flash_write(chip, bytes, kTableAddress, kSectorSize) == ESP_OK; }
    bool read(uint8_t* bytes) { return esp_flash_read(chip, bytes, kTableAddress, kSectorSize) == ESP_OK; }
};

static bool confirmApp0() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    // Establish app0 as the boot target even after a USB install or an OTA
    // staged from another browser. Then remove the stale app1 boot record.
    if (!run || run->address != 0x10000 ||
        esp_ota_set_boot_partition(run) != ESP_OK ||
        esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) return false;
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t state;
    // mark_app_valid confirms the active metadata entry. get_state_partition
    // may instead return an older entry when both records refer to app0.
    if (!boot || boot->address != run->address) return false;
    const esp_partition_t* other = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (!other) return false;
    esp_err_t result = esp_ota_get_state_partition(other, &state);
    // A freshly installed board (or selecting app0 twice) may have no app1
    // metadata. The invalidate API returns ESP_FAIL in that harmless case.
    if (result == ESP_ERR_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    if (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED) return true;
    return esp_ota_invalidate_inactive_ota_data_slot() == ESP_OK &&
           esp_ota_get_state_partition(other, &state) == ESP_ERR_NOT_FOUND;
}

static bool enlarge() {
    Buffer original(kSectorSize), enlarged(kSectorSize), scratch(kSectorSize);
    if (!original.p || !enlarged.p || !scratch.p) {
        lastError = F("Not enough memory. Restart the device and try again.");
        return false;
    }
    if (recoveryTable) memcpy(original.p, recoveryTable, kSectorSize);
    else if (esp_flash_read(nullptr, original.p, kTableAddress, kSectorSize) != ESP_OK) {
        lastError = F("Could not read flash. Preparation was not started.");
        return false;
    }
    if (!supported() || classify(original.p, scratch.p, md5) != Default || !confirmApp0()) {
        lastError = F("Could not verify the device. Preparation was not started.");
        return false;
    }
    build(enlarged.p, Enlarged, md5);
    RewriteResult result;
    {
        TableDriver driver;
        result = rewrite(driver, original.p, enlarged.p, scratch.p);
    } // Restore the flash protection before any NVS/log/network activity.
    if (result == RecoveryRequired) {
        if (!recoveryTable) { recoveryTable = original.p; original.p = nullptr; }
        recovery = true;
        lastError = F("Flash could not be verified. Keep power connected and retry preparation. USB recovery may be needed.");
        return false;
    }
    free(recoveryTable);
    recoveryTable = nullptr;
    recovery = false;
    if (result == Restored) {
        lastError = F("Preparation failed. The original flash layout was restored. You can try again.");
        return false;
    }
    return true;
}

static bool copyToApp0() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* app0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (!run || run->address != 0x150000 || !app0 || app0->address != 0x10000) return false;
    esp_image_metadata_t image = {};
    esp_partition_pos_t pos = {run->address, run->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &image) != ESP_OK ||
        !image.image_len || image.image_len > app0->size) return false;
    Buffer chunk(4096);
    if (!chunk.p) return false;
    esp_ota_handle_t handle;
    if (esp_ota_begin(app0, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) return false;
    for (uint32_t offset = 0; offset < image.image_len;) {
        size_t length = image.image_len - offset;
        if (length > 4096) length = 4096;
        if (esp_partition_read(run, offset, chunk.p, length) != ESP_OK ||
            esp_ota_write(handle, chunk.p, length) != ESP_OK) {
            esp_ota_abort(handle);
            return false;
        }
        offset += length;
        delay(1);
    }
    // A persisted request precedes selecting app0. Power loss before selection
    // leaves the original app1 bootable; startup reports an interrupted attempt.
    return esp_ota_end(handle) == ESP_OK && savePending(true) &&
           esp_ota_set_boot_partition(app0) == ESP_OK;
}

void begin() {
    Preferences prefs;
    if (!prefs.begin("flashlay", true)) return;
    bool pending = prefs.getBool("pending", false);
    prefs.end();
    if (!pending) return;
    if (detect() == Enlarged) { savePending(false); return; }
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (detect() != Default || !run || run->address != 0x10000) {
        savePending(false);
        lastError = F("Preparation was interrupted. Please try again.");
        return;
    }
    // Before any Wi-Fi, audio, timer or MQTT work is started. On success reboot
    // immediately: the SDK's cached partition descriptors still use the old map.
    if (enlarge()) { ESP.restart(); return; }
    if (recovery) otaInProgress = true;
    else savePending(false);
}

bool busy() { return queued || working || recovery; }

bool otaAllowed(String& error) {
    if (busy()) {
        error = F("Update preparation is in progress. Please wait.");
        return false;
    }
    uint32_t size = 0;
    if (esp_flash_get_size(nullptr, &size) != ESP_OK) {
        error = F("Could not verify update preparation. Please reload this page and try again.");
        return false;
    }
    // These configurations cannot use this tool. Preserve their ordinary OTA
    // rather than requiring an unavailable preparation operation.
    if (size != 4 * 1024 * 1024 || esp_flash_encryption_enabled() || esp_secure_boot_enabled()) return true;
    Buffer bytes(kSectorSize), scratch(kSectorSize);
    if (!bytes.p || !scratch.p || esp_flash_read(nullptr, bytes.p, kTableAddress, kSectorSize) != ESP_OK) {
        error = F("Could not verify update preparation. Please reload this page and try again.");
        return false;
    }
    const Layout layout = classify(bytes.p, scratch.p, md5);
    if (layout == Default) {
        error = F("Please prepare your device first using the Prepare device button above.");
        return false;
    }
    int count = 0;
    if (esp_partition_table_verify(reinterpret_cast<const esp_partition_info_t*>(bytes.p), false, &count) != ESP_OK) {
        error = F("Could not verify update preparation. Please reload this page and try again.");
        return false;
    }
    // Preserve valid custom layouts. Always compare the SDK's cached running
    // and target descriptors against flash so a rewrite alone cannot enable
    // another update before the required reboot has happened.
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (run && next && run->address != next->address) {
        const Entry* entries = reinterpret_cast<const Entry*>(bytes.p);
        bool runMatches = false, nextMatches = false;
        for (int i = 0; i < count; ++i) {
            if (entries[i].type != ESP_PARTITION_TYPE_APP) continue;
            if (entries[i].offset == run->address && entries[i].size == run->size) runMatches = true;
            if (entries[i].offset == next->address && entries[i].size == next->size) nextMatches = true;
        }
        if (runMatches && nextMatches) return true;
    }
    error = F("The flash layout changed. Please restart the device before updating.");
    return false;
}

bool start(String& error) {
    // A browser/network may repeat a POST after losing its acknowledgement.
    // Acknowledge the existing operation without queuing or starting it twice.
    if (queued || working) return true;
    if (!recovery && (otaInProgress || scheduledRebootAt)) {
        error = F("Another operation is in progress. Please wait.");
        return false;
    }
    if (!recovery) {
        const Layout layout = detect();
        if (layout == Enlarged) return true;
        if (layout != Default) {
            error = F("This device's flash layout is not supported for preparation.");
            return false;
        }
    }
    queued = true;
    startAt = millis() + 300; // Send the response before stopping any services.
    lastError = "";
    return true;
}

void loop() {
    if (!queued || static_cast<int32_t>(millis() - startAt) < 0) return;
    queued = false;
    working = true;
    beginOtaUpdate();
    const esp_partition_t* run = esp_ota_get_running_partition();
    bool ok = false;
    if (run && run->address == 0x10000) {
        if (savePending(true)) ok = enlarge();
        else lastError = F("Could not save preparation progress. Nothing was changed.");
    } else if (detect() == Default) {
        ok = copyToApp0();
        if (!ok) lastError = F("Could not prepare the firmware copy. Please try again.");
    }
    if (ok) {
        webui_pushLog(F("Update preparation: restarting to finish and verify the larger firmware space."));
        scheduleReboot(false, 500);
        // Keep working true until reboot; never expose a cached old map as ready.
    } else {
        working = false;
        if (!recovery) { savePending(false); endOtaUpdate(false); }
        webui_pushLog(lastError);
    }
}

String statusJson() {
    const Layout layout = detect();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    String otaError;
    const bool allowed = otaAllowed(otaError);
    const bool ready = layout == Enlarged && next && next->size == kEnlargedCapacity && allowed;
    const char* state = queued || working ? "working" : recovery ? "recovery_required" :
                        lastError.length() ? "error" : ready ? "ready" :
                        layout == Default ? "available" : !allowed ? "check_failed" : "unsupported";
    String json = String(F("{\"state\":\"")) + state + F("\",\"capacity\":") +
                  String(next ? next->size : 0) + F(",\"message\":\"") + lastError +
                  F("\",\"ota_allowed\":") + (allowed ? F("true") : F("false")) +
                  F(",\"ota_message\":\"") + otaError + F("\"}");
    // All messages above are fixed strings, never user/flash supplied JSON text.
    return json;
}
} // namespace flashlayout
#else
namespace flashlayout {
void begin() {}
void loop() {}
bool busy() { return false; }
bool otaAllowed(String&) { return true; }
bool start(String&) { return false; }
String statusJson() { return F("{\"state\":\"unsupported\",\"capacity\":0,\"message\":\"\",\"ota_allowed\":true,\"ota_message\":\"\"}"); }
} // namespace flashlayout
#endif
