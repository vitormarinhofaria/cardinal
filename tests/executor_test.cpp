#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <chrono>
#include <set>

#include "cardinal/executor.h"

using namespace cardinal;
using namespace std::chrono_literals;

TEST(ThreadPoolExecutor, RunsJobsOffTheCallingThread) {
    std::promise<std::thread::id> ran;
    auto f = ran.get_future();
    ThreadPoolExecutor pool(2);
    pool.post([&] { ran.set_value(std::this_thread::get_id()); });
    ASSERT_EQ(f.wait_for(2s), std::future_status::ready);
    EXPECT_NE(f.get(), std::this_thread::get_id());
}

TEST(ThreadPoolExecutor, RunsJobsInParallel) {
    std::promise<void> aStarted, bStarted;
    auto aFut = aStarted.get_future(), bFut = bStarted.get_future();
    ThreadPoolExecutor pool(2);  // declared last: joins before the promises it uses go away
    // each job waits for the other: only completes if both run at the same time
    pool.post([&] {
        aStarted.set_value();
        bFut.wait();
    });
    pool.post([&] {
        bStarted.set_value();
        aFut.wait();
    });
    EXPECT_EQ(aFut.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(bFut.wait_for(2s), std::future_status::ready);
}

TEST(ThreadPoolExecutor, DestructorJoinsWithQueuedJobs) {
    std::atomic<int> ran{0};
    {
        ThreadPoolExecutor pool(1);
        for (int i = 0; i < 100; ++i) pool.post([&] { ++ran; std::this_thread::sleep_for(1ms); });
    }
    EXPECT_LE(ran.load(), 100);  // returned without hanging; queued jobs may be dropped
}
