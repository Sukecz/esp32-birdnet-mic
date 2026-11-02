#include "../esp32-birdnet-mic/SettingsValidation.h"
#include <string>
#include <cassert>
int main() {
    using namespace settingsvalidation;
    uint32_t u = 7; int32_t i = 7; float f = 7;
    for (const char* bad : {"", "-1", "+1", " 1", "1 ", "1.0", "4294967296", "18446744073709551616"}) {
        assert(!parseUInt32Strict(std::string(bad), u)); assert(u == 7);
    }
    assert(parseUInt32Strict(std::string("4294967295"), u) && u == UINT32_MAX);
    assert(parseInt32Strict(std::string("-2147483648"), i) && i == INT32_MIN);
    assert(parseInt32Strict(std::string("+2147483647"), i) && i == INT32_MAX);
    for (const char* bad : {"2147483648", "-2147483649", "-", " 1", "1x"})
        assert(!parseInt32Strict(std::string(bad), i));
    for (const char* bad : {"", "nan", "inf", "-inf", "1e100", "1.2x"})
        assert(!parseFloatStrict(std::string(bad), f));
    assert(parseFloatStrict(std::string("1.25"), f) && f == 1.25f);
    assert(validSchedule(true, true, 1320, 360));
    assert(!validSchedule(true, false, 360, 360));
    assert(!validSchedule(false, true, 0, 1));
    assert(!validSchedule(false, false, 1440, 1));
}
