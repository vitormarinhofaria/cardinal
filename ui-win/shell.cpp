#include "pch.h"

#include "shell.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <vector>

#include "win_util.h"

#ifndef CARDINAL_TRELLO_API_KEY
#define CARDINAL_TRELLO_API_KEY ""
#endif

namespace cardinal::win {
namespace {

class MemoryCredentials : public CredentialStore {
public:
    explicit MemoryCredentials(std::string token) {
        if (!token.empty()) value_ = Credentials{std::move(token)};
    }
    std::optional<Credentials> load() override { return value_; }
    void save(const Credentials& c) override { value_ = c; }
    void clear() override { value_.reset(); }

private:
    std::optional<Credentials> value_;
};

std::filesystem::path localAppData() {
    PWSTR path = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) out = path;
    ::CoTaskMemFree(path);
    return out;
}

std::string envOr(const char* name, const std::string& fallback) {
    char* value = nullptr;
    size_t len = 0;
    std::string out = fallback;
    if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
    free(value);
    return out;
}

}  // namespace

Options parseCommandLine() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(winrt::to_string(argv[i]));
    ::LocalFree(argv);

    Options o;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string name = args[i], value;
        bool inlineValue = false;
        if (auto eq = name.find('='); eq != std::string::npos) {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
            inlineValue = true;
        }
        auto take = [&]() -> std::string {
            if (inlineValue) return value;
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (name == "--api-base") o.apiBase = take();
        else if (name == "--api-key") o.apiKey = take();
        else if (name == "--token") o.token = take();
        else if (name == "--data-dir") o.dataDir = take();
        else if (name == "--poll-seconds") o.pollSeconds = std::atoi(take().c_str());
        else if (name == "--open-card") o.openCard = take();
        else if (name == "--no-keyring") o.noKeyring = true;
    }
    if (o.pollSeconds <= 0) o.pollSeconds = 20;
    return o;
}

Shell::Shell(Options opts) : options(std::move(opts)) {
    const Options& o = options;
    if (!o.dataDir.empty()) {
        dataDir = o.dataDir;
    } else {
        auto u8 = (localAppData() / "cardinal").u8string();
        dataDir.assign(u8.begin(), u8.end());
    }

    AppConfig cfg;
    cfg.dataDir = std::filesystem::path(widen(dataDir));
    cfg.apiKey = !o.apiKey.empty() ? o.apiKey : envOr("CARDINAL_API_KEY", CARDINAL_TRELLO_API_KEY);
    if (!o.apiBase.empty()) cfg.apiBase = o.apiBase;
    cfg.pollInterval = std::chrono::seconds(o.pollSeconds);

    if (!o.token.empty() || o.noKeyring)
        credentials = std::make_unique<MemoryCredentials>(o.token);
    else
        credentials = makeWinCredCredentialStore();

    session = std::make_unique<Session>(cfg, *credentials, browser, dispatcher);
}

Shell::~Shell() {
    session.reset();  // before the credentials and dispatcher it holds references to
}

}  // namespace cardinal::win
