#include <cstring>
#include <string>
// Interposes libVideoFXLocal.so's exported GetOSInfo(key, out) which parses
// /etc/lsb-release and crashes on non-Ubuntu distros (CachyOS: DISTRIB_RELEASE="rolling").
// Claim Ubuntu 22.04 (a supported OS) — used only for the model-compatibility check.
int GetOSInfo(const char* key, std::string& out) {
    const std::string k = key ? key : "";
    if (k == "ID") out = "ubuntu";
    else if (k.find("RELEASE") != std::string::npos || k.find("VERSION") != std::string::npos) out = "22.04";
    else out = "Ubuntu";
    return 0;
}
