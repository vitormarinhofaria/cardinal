#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cardinal/executor.h"
#include "cardinal/http.h"

namespace cardinal {

struct ImageCacheOptions {
    std::filesystem::path dir;  // created if missing
    std::string apiKey;
    std::string token;
    // Origins ("https://trello.com") that get the OAuth header. Never anyone else's.
    std::vector<std::string> authOrigins = {"https://trello.com", "https://api.trello.com"};
    long maxImageBytes = 25L * 1024 * 1024;
    std::uintmax_t diskBudget = 300ULL * 1024 * 1024;  // oldest files go past this
    int threads = 4;
    bool pruneOnStart = true;
};

// Images (attachments, pictures in descriptions and comments) kept on disk, one file per URL.
// A file already on disk is answered without the network, so pictures show offline too.
class ImageCache {
public:
    // Called on a worker thread (never the UI thread): the file, or nullopt if it could not be
    // fetched. Heavy work (decoding) may run right there.
    using Done = std::function<void(std::optional<std::filesystem::path>)>;

    ImageCache(HttpClient& http, ImageCacheOptions options);
    ~ImageCache();  // callbacks still queued are dropped
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    void fetch(const std::string& url, Done done);

    std::filesystem::path pathFor(const std::string& url) const;
    // Deletes the least recently used files until the folder fits the budget. Runs at startup.
    void prune();

    static std::string originOf(const std::string& url);  // "https://host[:port]", lowercase

private:
    void run(const std::string& url);
    void finish(const std::string& url, std::optional<std::filesystem::path> file);

    HttpClient& http_;
    ImageCacheOptions options_;
    std::mutex mutex_;
    std::map<std::string, std::vector<Done>> waiting_;  // URL -> callers, one download each
    std::set<std::string> failed_;                       // refused (4xx) this session: not retried
    std::unique_ptr<ThreadPoolExecutor> pool_;           // last: its threads stop first
};

}  // namespace cardinal
