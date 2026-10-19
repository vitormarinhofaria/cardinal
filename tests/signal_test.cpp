#include <gtest/gtest.h>

#include "cardinal/viewmodel.h"

using namespace cardinal;

TEST(Signal, CallsSlotsInConnectionOrder) {
    Signal<int> s;
    std::vector<int> seen;
    s.connect([&](int v) { seen.push_back(v * 10); });
    s.connect([&](int v) { seen.push_back(v * 100); });
    s.emit(2);
    EXPECT_EQ(seen, (std::vector<int>{20, 200}));
}

TEST(Signal, SlotDisconnectedByAnEarlierSlotIsNotCalledInTheSameEmit) {
    Signal<> s;
    int secondCalls = 0;
    int secondId = 0;
    s.connect([&] { s.disconnect(secondId); });  // e.g. the UI tearing down a page
    secondId = s.connect([&] { ++secondCalls; });
    s.emit();
    EXPECT_EQ(secondCalls, 0);
    s.emit();
    EXPECT_EQ(secondCalls, 0);
}

TEST(Signal, SlotConnectedDuringEmitWaitsForTheNextEmit) {
    Signal<> s;
    int late = 0;
    s.connect([&] { s.connect([&] { ++late; }); });
    s.emit();
    EXPECT_EQ(late, 0);
    s.emit();
    EXPECT_EQ(late, 1);
}

TEST(Signal, DisconnectingTwiceOrAnUnknownIdIsHarmless) {
    Signal<> s;
    int id = s.connect([] {});
    s.disconnect(id);
    s.disconnect(id);
    s.disconnect(999);
    s.emit();
}
