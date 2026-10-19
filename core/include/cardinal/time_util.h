#pragma once

#include <ctime>
#include <string>

namespace cardinal {

// "2026-10-19T12:05:00.000Z": the format Trello uses for action dates and `since`.
std::string isoUtc(std::time_t t);

}  // namespace cardinal
