#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include "cardinal/image_cache.h"
#include "http_server.h"

using namespace cardinal;
using namespace cardinal::testing;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

class ImageCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cardinal-images-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    ImageCacheOptions options(std::vector<std::string> authOrigins = {}) {
        ImageCacheOptions o;
        o.dir = dir_;
        o.apiKey = "KEY";
        o.token = "TOKEN";
        o.authOrigins = std::move(authOrigins);
        return o;
    }

    static std::optional<fs::path> get(ImageCache& cache, const std::string& url) {
        auto p = std::make_shared<std::promise<std::optional<fs::path>>>();
        auto f = p->get_future();
        cache.fetch(url, [p](std::optional<fs::path> file) { p->set_value(std::move(file)); });
        EXPECT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        return f.get();
    }

    fs::path dir_;
};

}  // namespace

TEST_F(ImageCacheTest, DownloadsOnceThenAnswersFromDiskWithoutTheNetwork) {
    RoutedServer server([](const HttpRequestInfo&) { return HttpReply{200, "PNGDATA"}; });
    const std::string url = server.url() + "/a.png";
    auto http = makeCurlHttpClient(2);
    {
        ImageCache cache(*http, options());
        auto file = get(cache, url);
        ASSERT_TRUE(file);
        EXPECT_EQ(slurp(*file), "PNGDATA");
        EXPECT_TRUE(get(cache, url));
    }
    ImageCache again(*http, options());  // next run of the app
    auto file = get(again, url);
    ASSERT_TRUE(file);
    EXPECT_EQ(slurp(*file), "PNGDATA");
    EXPECT_EQ(server.count("GET", "/a.png"), 1u);
}

TEST_F(ImageCacheTest, OAuthHeaderGoesOnlyToTrelloOrigins) {
    RoutedServer trello([](const HttpRequestInfo&) { return HttpReply{200, "T"}; });
    RoutedServer other([](const HttpRequestInfo&) { return HttpReply{200, "O"}; });
    auto http = makeCurlHttpClient(2);
    ImageCache cache(*http, options({trello.url()}));
    ASSERT_TRUE(get(cache, trello.url() + "/download/x.png"));
    ASSERT_TRUE(get(cache, other.url() + "/x.png"));
    ASSERT_EQ(trello.requests().size(), 1u);
    EXPECT_NE(trello.requests()[0].headers.find(
                  "authorization: oauth oauth_consumer_key=\"key\", oauth_token=\"token\""),
              std::string::npos);
    ASSERT_EQ(other.requests().size(), 1u);
    EXPECT_EQ(other.requests()[0].headers.find("authorization"), std::string::npos);
}

TEST_F(ImageCacheTest, RedirectToAnotherHostDropsTheToken) {
    RoutedServer storage([](const HttpRequestInfo&) { return HttpReply{200, "S3"}; });
    const std::string target = "http://localhost:" + storage.url().substr(storage.url().rfind(':') + 1) + "/blob";
    RoutedServer trello([&](const HttpRequestInfo&) { return HttpReply{302, "", {"Location: " + target}}; });
    auto http = makeCurlHttpClient(1);
    ImageCache cache(*http, options({trello.url()}));
    auto file = get(cache, trello.url() + "/download/x.png");
    ASSERT_TRUE(file);
    EXPECT_EQ(slurp(*file), "S3");
    ASSERT_EQ(storage.requests().size(), 1u);
    EXPECT_EQ(storage.requests()[0].headers.find("authorization"), std::string::npos);
}

TEST_F(ImageCacheTest, SimultaneousRequestsShareOneDownload) {
    RoutedServer server([](const HttpRequestInfo&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return HttpReply{200, "X"};
    });
    auto http = makeCurlHttpClient(4);
    ImageCache cache(*http, options());
    const std::string url = server.url() + "/a.png";
    std::vector<std::future<std::optional<fs::path>>> results;
    for (int i = 0; i < 4; ++i) {
        auto p = std::make_shared<std::promise<std::optional<fs::path>>>();
        results.push_back(p->get_future());
        cache.fetch(url, [p](std::optional<fs::path> f) { p->set_value(std::move(f)); });
    }
    for (auto& r : results) EXPECT_TRUE(r.get());
    EXPECT_EQ(server.count("GET", "/a.png"), 1u);
}

TEST_F(ImageCacheTest, ErrorsAreNotCachedAndRefusalsNotRetriedThisSession) {
    RoutedServer server([](const HttpRequestInfo& r) {
        return r.path == "/gone.png" ? HttpReply{404, "nope"} : HttpReply{500, "busy"};
    });
    auto http = makeCurlHttpClient(1);
    ImageCache cache(*http, options());
    EXPECT_FALSE(get(cache, server.url() + "/gone.png"));
    EXPECT_FALSE(get(cache, server.url() + "/gone.png"));
    EXPECT_EQ(server.count("GET", "/gone.png"), 1u);
    EXPECT_FALSE(get(cache, server.url() + "/busy.png"));
    EXPECT_FALSE(get(cache, server.url() + "/busy.png"));
    EXPECT_EQ(server.count("GET", "/busy.png"), 2u);  // a server error may pass
    EXPECT_FALSE(fs::exists(cache.pathFor(server.url() + "/gone.png")));
}

TEST_F(ImageCacheTest, TooBigIsRefused) {
    RoutedServer server([](const HttpRequestInfo&) { return HttpReply{200, std::string(5000, 'x')}; });
    auto http = makeCurlHttpClient(1);
    auto o = options();
    o.maxImageBytes = 1000;
    ImageCache cache(*http, o);
    EXPECT_FALSE(get(cache, server.url() + "/big.png"));
    EXPECT_FALSE(fs::exists(cache.pathFor(server.url() + "/big.png")));
}

TEST_F(ImageCacheTest, OnlyWebUrlsAreFetched) {
    auto http = makeCurlHttpClient(1);
    ImageCache cache(*http, options());
    EXPECT_FALSE(get(cache, "file:///etc/passwd"));
    EXPECT_FALSE(get(cache, "ftp://x/a.png"));
}

TEST_F(ImageCacheTest, PruneDropsTheLeastRecentlyUsedFiles) {
    auto http = makeCurlHttpClient(1);
    auto o = options();
    o.diskBudget = 2500;
    o.pruneOnStart = false;
    ImageCache cache(*http, o);
    const auto now = fs::file_time_type::clock::now();
    for (int i = 0; i < 4; ++i) {
        fs::path p = dir_ / ("f" + std::to_string(i) + ".img");
        std::ofstream(p) << std::string(1000, 'x');
        fs::last_write_time(p, now - std::chrono::hours(10 - i));  // f0 oldest
    }
    cache.prune();
    EXPECT_FALSE(fs::exists(dir_ / "f0.img"));
    EXPECT_FALSE(fs::exists(dir_ / "f1.img"));
    EXPECT_TRUE(fs::exists(dir_ / "f2.img"));
    EXPECT_TRUE(fs::exists(dir_ / "f3.img"));
}

TEST(ImageCacheOrigin, OriginOf) {
    EXPECT_EQ(ImageCache::originOf("HTTPS://Trello.com/1/x?y"), "https://trello.com");
    EXPECT_EQ(ImageCache::originOf("http://127.0.0.1:8099/1/a"), "http://127.0.0.1:8099");
    EXPECT_EQ(ImageCache::originOf("https://trello.com@evil.com/a"), "");
    EXPECT_EQ(ImageCache::originOf("no-scheme"), "");
}
