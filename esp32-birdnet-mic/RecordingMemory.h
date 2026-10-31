#pragma once
#include <stddef.h>

namespace recordingmemory {
// Recording has no RTSP ring/clients. Retain this working reserve after the
// producer/SD task allocations; their stacks are budgeted separately. A PCM
// queue placed in internal RAM still uses the larger 72 KiB reserve.
constexpr size_t kWorkingReserveBytes = 48 * 1024;
}
