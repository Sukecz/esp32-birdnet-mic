#pragma once
#include <stdint.h>

// 32 obsolete discovery records, two legacy availability records, 32 entities.
struct DiscoveryProgress {
    static constexpr uint16_t total = 66;
    uint16_t step = 0;
    uint32_t lastAttempt = 0;
    uint32_t interval = 0;
    bool failed = false;

    void reset() { step = 0; interval = 0; failed = false; }
    bool done() const { return step == total; }
    bool ready(uint32_t now) const {
        return !done() && (interval == 0 || uint32_t(now - lastAttempt) >= interval);
    }
    void completeStep(bool ok, uint32_t now) {
        if (done()) return;
        lastAttempt = now;
        failed = !ok;
        interval = ok ? 1000 : 60000;
        if (ok) ++step;
    }
};
