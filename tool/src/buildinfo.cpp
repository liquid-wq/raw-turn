#include "buildinfo.h"

#include <cstdio>

namespace buildinfo {

const char* string() {
    static char buf[96];
    if (!buf[0]) {
        // __DATE__/__TIME__ stammen aus dem Uebersetzungslauf dieser Datei
        // und aendern sich bei jedem Neubau.
        std::snprintf(buf, sizeof(buf), "Build %d (%s) %s %s",
                      kBuild, kBuildDate, __DATE__, __TIME__);
    }
    return buf;
}

}  // namespace buildinfo
