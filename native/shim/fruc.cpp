#include "fruc.h"
#include <cstring>

#ifndef COS_HAS_FRUC
Fruc::Fruc() {}
Fruc::~Fruc() {}
bool Fruc::Probe(std::string& detail) { detail = "FRUC not built in (COS_HAS_FRUC unset)"; return false; }
bool Fruc::Start(int, int) { lastError_ = "FRUC not built in"; return false; }
void Fruc::Stop() { ready_ = false; }
bool Fruc::Interpolate(const uint8_t*, const uint8_t*, int, int, std::vector<uint8_t>&) {
    lastError_ = "FRUC not built in"; return false;
}
#endif // !COS_HAS_FRUC
