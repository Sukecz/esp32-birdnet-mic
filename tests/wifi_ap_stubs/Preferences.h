#pragma once
#include <vector>
#include <cstring>
extern std::vector<uint8_t> testSaved;
extern bool testWriteFails;
class Preferences {
public:
 bool begin(const char *, bool) { return true; }
 void end() {}
 size_t getBytesLength(const char *) { return testSaved.size(); }
 size_t getBytes(const char *, void *out, size_t size) { memcpy(out, testSaved.data(), size); return size; }
 size_t putBytes(const char *, const void *p, size_t size) {
   if (testWriteFails) return 0;
   const auto *bytes = static_cast<const uint8_t *>(p); testSaved.assign(bytes, bytes + size); return size;
 }
 bool isKey(const char *) { return !testSaved.empty(); }
 bool remove(const char *) { if (testWriteFails) return false; testSaved.clear(); return true; }
};
