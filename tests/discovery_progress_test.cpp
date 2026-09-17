#include "../esp32-birdnet-mic/DiscoveryProgress.h"
#include <cassert>
#include <cstdint>

int main() {
    DiscoveryProgress progress;
    assert(progress.ready(0));
    progress.completeStep(true, 0);
    assert(progress.step == 1 && !progress.ready(999) && progress.ready(1000));
    progress.completeStep(false, 1000);
    assert(progress.failed && progress.step == 1);
    assert(!progress.ready(60999) && progress.ready(61000));
    progress.completeStep(true, 61000);
    assert(!progress.failed && progress.step == 2);
    // Reconnect/manual refresh must restart the complete retained set.
    progress.reset();
    assert(progress.step == 0 && progress.ready(61000));
    uint32_t now = UINT32_MAX - 500;
    progress.completeStep(true, now);
    assert(!progress.ready(498) && progress.ready(499));
    now = 499;
    while (!progress.done()) {
        assert(progress.ready(now));
        progress.completeStep(true, now);
        now += 1000;
    }
    assert(progress.step == 66 && !progress.ready(now));
    progress.completeStep(true, now);
    assert(progress.step == 66);
    progress.reset();
    assert(progress.ready(now));
}
