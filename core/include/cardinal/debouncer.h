#pragma once

#include <chrono>
#include <optional>

namespace cardinal {

// Folds a burst of signals into one: due once they stop for `quiet`, or `maxDelay` after the
// first at the latest. Not thread-safe.
class Debouncer {
public:
    using Clock = std::chrono::steady_clock;

    Debouncer(std::chrono::milliseconds quiet, std::chrono::milliseconds maxDelay)
        : quiet_(quiet), maxDelay_(maxDelay) {}

    void note(Clock::time_point now) {
        if (!first_) first_ = now;
        last_ = now;
    }
    bool due(Clock::time_point now) const {
        return first_ && (now - *last_ >= quiet_ || now - *first_ >= maxDelay_);
    }
    bool pending() const { return first_.has_value(); }
    void reset() {
        first_.reset();
        last_.reset();
    }

private:
    std::chrono::milliseconds quiet_, maxDelay_;
    std::optional<Clock::time_point> first_, last_;
};

}  // namespace cardinal
