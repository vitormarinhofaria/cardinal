#include <gtest/gtest.h>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "cardinal/cloudflared.h"
#include "cardinal/tunnel.h"

using namespace cardinal;
using namespace std::chrono_literals;

namespace {

struct FakeDownloads : HttpClient {
    std::future<HttpResponse> request(Method, std::string, std::string) override {
        throw std::logic_error("unexpected request");
    }
    std::future<HttpResponse> download(Download d) override {
        std::lock_guard lk(mu);
        urls.push_back(d.url);
        headers = d.headers;
        std::promise<HttpResponse> p;
        p.set_value(reply);
        return p.get_future();
    }
    std::mutex mu;
    HttpResponse reply{200, "binary"};
    std::vector<std::string> urls;
    std::vector<std::string> headers;
};

class TempDir {
public:
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("cardinal-cf-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

}  // namespace

TEST(Cloudflared, PinnedAssetsPerMachine) {
    ASSERT_TRUE(cloudflaredAssetFor("x86_64"));
    EXPECT_EQ(cloudflaredAssetFor("x86_64")->name, "cloudflared-linux-amd64");
    EXPECT_EQ(cloudflaredAssetFor("aarch64")->name, "cloudflared-linux-arm64");
    EXPECT_EQ(cloudflaredAssetFor("armv7l")->name, "cloudflared-linux-arm");
    EXPECT_FALSE(cloudflaredAssetFor("riscv64"));
    EXPECT_EQ(cloudflaredAssetFor("x86_64")->sha256.size(), 64u);
}

TEST(Cloudflared, Sha256) {
    EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Cloudflared, DownloadsVerifiesAndReusesWithoutDownloadingAgain) {
    TempDir dir;
    FakeDownloads http;
    CloudflaredSource src;
    src.baseUrl = "https://example.test/rel";
    src.asset = CloudflaredAsset{"cloudflared-test", sha256Hex("binary")};
    std::ofstream(dir.path / "cloudflared-1999.1.1") << "old";

    auto path = ensureCloudflared(http, dir.path, src);
    EXPECT_EQ(path, dir.path / (std::string("cloudflared-") + kCloudflaredVersion));
    ASSERT_EQ(http.urls.size(), 1u);
    EXPECT_EQ(http.urls[0], std::string("https://example.test/rel/") + kCloudflaredVersion + "/cloudflared-test");
    EXPECT_TRUE(http.headers.empty());  // never the Trello credentials
    struct stat st{};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_TRUE(st.st_mode & S_IXUSR);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "cloudflared-1999.1.1"));  // old version gone

    ensureCloudflared(http, dir.path, src);
    EXPECT_EQ(http.urls.size(), 1u);
}

TEST(Cloudflared, WrongHashLeavesNothingAndIsPermanent) {
    TempDir dir;
    FakeDownloads http;
    http.reply = {200, "tampered"};
    CloudflaredSource src;
    src.asset = CloudflaredAsset{"cloudflared-test", sha256Hex("binary")};
    EXPECT_THROW(ensureCloudflared(http, dir.path, src), CloudflaredUnavailable);
    EXPECT_TRUE(std::filesystem::is_empty(dir.path));
}

TEST(Cloudflared, HttpErrorIsTransient) {
    TempDir dir;
    FakeDownloads http;
    http.reply = {503, ""};
    CloudflaredSource src;
    src.asset = CloudflaredAsset{"cloudflared-test", sha256Hex("binary")};
    try {
        ensureCloudflared(http, dir.path, src);
        FAIL() << "no throw";
    } catch (const CloudflaredUnavailable&) {
        FAIL() << "a server error should be retried, not permanent";
    } catch (const std::exception&) {
    }
}

TEST(Cloudflared, UnknownMachineIsPermanent) {
    TempDir dir;
    FakeDownloads http;
    CloudflaredSource src;
    src.machine = "riscv64";
    EXPECT_THROW(ensureCloudflared(http, dir.path, src), CloudflaredUnavailable);
    EXPECT_TRUE(http.urls.empty());
}

TEST(QuickTunnelUrl, FoundInCloudflaredLog) {
    EXPECT_EQ(parseQuickTunnelUrl(
                  "2026-11-03T16:40:01Z INF |  https://brave-otter-lake-sun.trycloudflare.com                    |"),
              "https://brave-otter-lake-sun.trycloudflare.com");
    EXPECT_EQ(parseQuickTunnelUrl("INF Requesting new quick Tunnel on trycloudflare.com..."), "");
    EXPECT_EQ(parseQuickTunnelUrl("POST https://api.trycloudflare.com/tunnel then https://a-b.trycloudflare.com"),
              "https://a-b.trycloudflare.com");
}

// ---- the real process handling, with a stand-in script ----

namespace {

std::filesystem::path script(const std::filesystem::path& dir, const std::string& body) {
    auto p = dir / "fake-cloudflared";
    std::ofstream(p) << "#!/bin/sh\n" << body;
    ::chmod(p.c_str(), 0755);
    return p;
}

}  // namespace

TEST(CloudflaredTunnelProcess, ReturnsTheUrlAndDiesOnClose) {
    TempDir dir;
    auto bin = script(dir.path,
                      "echo \"args: $*\" >&2\n"
                      "echo \"home: $HOME\" >&2\n"
                      "echo 'INF |  https://test-tunnel.trycloudflare.com  |' >&2\n"
                      "exec sleep 30\n");
    CloudflaredTunnel tunnel(bin, dir.path / "home");
    EXPECT_EQ(tunnel.open(4321, {}), "https://test-tunnel.trycloudflare.com");
    EXPECT_TRUE(tunnel.alive());
    auto start = std::chrono::steady_clock::now();
    tunnel.close();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    EXPECT_FALSE(tunnel.alive());
}

TEST(CloudflaredTunnelProcess, NoticesWhenTheProcessDies) {
    TempDir dir;
    auto bin = script(dir.path, "echo 'https://short-lived.trycloudflare.com' >&2\nsleep 0.2\n");
    CloudflaredTunnel tunnel(bin, dir.path / "home");
    EXPECT_EQ(tunnel.open(1, {}), "https://short-lived.trycloudflare.com");
    bool dead = false;
    for (int i = 0; i < 100 && !dead; ++i) {
        dead = !tunnel.alive();
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(dead);
}

TEST(CloudflaredTunnelProcess, ExitWithoutUrlIsAnError) {
    TempDir dir;
    auto bin = script(dir.path, "echo 'ERR failed to request quick Tunnel' >&2\nexit 1\n");
    CloudflaredTunnel tunnel(bin, dir.path / "home");
    try {
        tunnel.open(1, {});
        FAIL() << "no throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("failed to request quick Tunnel"), std::string::npos) << e.what();
    }
}

TEST(CloudflaredTunnelProcess, PassesPortAndIsolatedHome) {
    TempDir dir;
    auto out = dir.path / "seen";
    auto bin = script(dir.path, "echo \"$* HOME=$HOME\" > '" + out.string() +
                                    "'\necho https://x-y.trycloudflare.com >&2\nexec sleep 30\n");
    CloudflaredTunnel tunnel(bin, dir.path / "home");
    tunnel.open(4321, {});
    tunnel.close();
    std::string seen;
    std::getline(std::ifstream(out), seen);
    EXPECT_NE(seen.find("tunnel --no-autoupdate --url http://127.0.0.1:4321"), std::string::npos) << seen;
    EXPECT_NE(seen.find("HOME=" + (dir.path / "home").string()), std::string::npos) << seen;
}
