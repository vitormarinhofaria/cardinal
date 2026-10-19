#include "cardinal/time_util.h"

#include <array>

namespace cardinal {

std::string isoUtc(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::array<char, 32> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return buf.data();
}

}  // namespace cardinal
