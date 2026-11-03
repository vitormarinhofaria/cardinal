#include "cardinal/image_cache.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace cardinal {
namespace fs = std::filesystem;

namespace {

// Stable across runs and platforms (std::hash is not promised to be).
std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

ImageCache::ImageCache(HttpClient& http, ImageCacheOptions options)
    : http_(http), options_(std::move(options)) {
    for (auto& o : options_.authOrigins) o = originOf(o);
    std::error_code ec;
    fs::create_directories(options_.dir, ec);
    pool_ = std::make_unique<ThreadPoolExecutor>(options_.threads);
    if (options_.pruneOnStart) pool_->post([this] { prune(); });
}

ImageCache::~ImageCache() { pool_.reset(); }

std::string ImageCache::originOf(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    const size_t end = url.find_first_of("/?#", scheme + 3);
    std::string origin = url.substr(0, end);
    for (auto& c : origin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (origin.find('@') != std::string::npos) return "";  // user:pass@host: never authenticate
    return origin;
}

fs::path ImageCache::pathFor(const std::string& url) const {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.img", static_cast<unsigned long long>(fnv1a(url)));
    return options_.dir / name;
}

void ImageCache::fetch(const std::string& url, Done done) {
    {
        std::lock_guard lk(mutex_);
        auto [it, first] = waiting_.try_emplace(url);
        it->second.push_back(std::move(done));
        if (!first) return;  // already on its way
    }
    pool_->post([this, url] { run(url); });
}

void ImageCache::run(const std::string& url) {
    const fs::path file = pathFor(url);
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) {
        fs::last_write_time(file, fs::file_time_type::clock::now(), ec);  // recently used
        finish(url, file);
        return;
    }
    bool refused;
    {
        std::lock_guard lk(mutex_);
        refused = failed_.count(url) > 0;
    }
    const std::string origin = originOf(url);
    if (refused || (origin.rfind("https://", 0) != 0 && origin.rfind("http://", 0) != 0)) {
        finish(url, std::nullopt);
        return;
    }
    Download d;
    d.url = url;
    d.maxBytes = options_.maxImageBytes;
    if (std::find(options_.authOrigins.begin(), options_.authOrigins.end(), origin) != options_.authOrigins.end())
        d.headers.push_back("Authorization: OAuth oauth_consumer_key=\"" + options_.apiKey +
                            "\", oauth_token=\"" + options_.token + "\"");
    HttpResponse r;
    try {
        r = http_.download(std::move(d)).get();
    } catch (const std::exception&) {
        finish(url, std::nullopt);  // offline or too big: try again next time
        return;
    }
    if (r.status != 200 || r.body.empty()) {
        if (r.status >= 400 && r.status < 500 && r.status != 429) {
            std::lock_guard lk(mutex_);
            failed_.insert(url);
        }
        finish(url, std::nullopt);
        return;
    }
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
        if (!out) {
            fs::remove(tmp, ec);
            finish(url, std::nullopt);
            return;
        }
    }
    fs::rename(tmp, file, ec);  // atomic: a reader never sees half a file
    finish(url, ec ? std::nullopt : std::optional<fs::path>(file));
}

void ImageCache::finish(const std::string& url, std::optional<fs::path> file) {
    std::vector<Done> callers;
    {
        std::lock_guard lk(mutex_);
        auto it = waiting_.find(url);
        if (it == waiting_.end()) return;
        callers = std::move(it->second);
        waiting_.erase(it);
    }
    for (auto& done : callers) done(file);
}

void ImageCache::prune() {
    struct Entry {
        fs::path path;
        fs::file_time_type when;
        std::uintmax_t size;
    };
    std::vector<Entry> entries;
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(options_.dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (!it->is_regular_file(e)) continue;
        if (it->path().extension() == ".tmp") {  // left by a crash (or being written right now)
            if (it->last_write_time(e) < fs::file_time_type::clock::now() - std::chrono::minutes(10))
                fs::remove(it->path(), e);
            continue;
        }
        Entry entry{it->path(), it->last_write_time(e), it->file_size(e)};
        if (e) continue;
        total += entry.size;
        entries.push_back(std::move(entry));
    }
    if (total <= options_.diskBudget) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.when < b.when; });
    const std::uintmax_t target = options_.diskBudget / 10 * 8;  // leave room: no prune every start
    for (const auto& entry : entries) {
        if (total <= target) break;
        std::error_code e;
        if (fs::remove(entry.path, e)) total -= entry.size;
    }
}

}  // namespace cardinal
