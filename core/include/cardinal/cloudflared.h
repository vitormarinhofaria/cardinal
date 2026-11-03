#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include "cardinal/http.h"

namespace cardinal {

// The cloudflared release Cardinal runs, pinned: the binary is checked against these hashes, so a
// new version means updating the table in cloudflared.cpp.
struct CloudflaredAsset {
    std::string name;    // file name in the GitHub release
    std::string sha256;  // lowercase hex
};
extern const char* const kCloudflaredVersion;

// The asset for a `uname -m` machine name, or nullopt when there is no build for it.
std::optional<CloudflaredAsset> cloudflaredAssetFor(const std::string& machine);

class CloudflaredUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CloudflaredSource {
    std::string baseUrl = "https://github.com/cloudflare/cloudflared/releases/download";
    std::string machine;  // empty: this machine's
    std::optional<CloudflaredAsset> asset;  // tests: instead of the pinned table
};

// Returns the path of a verified cloudflared under `dir` (the app's own, not on PATH), downloading
// it the first time. Throws CloudflaredUnavailable when there is no build for this machine or the
// download does not match the pinned hash (permanent), std::exception for network trouble.
std::filesystem::path ensureCloudflared(HttpClient& http, const std::filesystem::path& dir,
                                        const CloudflaredSource& source = {});

std::string sha256Hex(const std::string& data);

}  // namespace cardinal
