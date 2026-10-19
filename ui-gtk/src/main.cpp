#include <adwaita.h>

#include <string>

#include "cardinal/version.h"

int main(int, char**) {
    g_print("cardinal %s (libadwaita %d.%d)\n", std::string(cardinal::version()).c_str(),
            ADW_MAJOR_VERSION, ADW_MINOR_VERSION);
    return 0;
}
