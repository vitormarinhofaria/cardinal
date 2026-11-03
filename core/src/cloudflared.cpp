#include "cardinal/cloudflared.h"

#include <openssl/evp.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <fstream>
#include <sstream>

namespace cardinal {

const char* const kCloudflaredVersion = "2026.10.7";

std::optional<CloudflaredAsset> cloudflaredAssetFor(const std::string& machine) {
    if (machine == "x86_64" || machine == "amd64")
        return CloudflaredAsset{"cloudflared-linux-amd64",
                                "1f99fb518b83dec2ba8d870a7cc991d82a77e6196295a3d2754ba17ab9f91872"};
    if (machine == "aarch64" || machine == "arm64")
        return CloudflaredAsset{"cloudflared-linux-arm64",
                                "3df43aedc9ff291d1735b5cb576cb97e0b157488796db2e32b28aecabb445775"};
    if (machine.rfind("armv7", 0) == 0)
        return CloudflaredAsset{"cloudflared-linux-arm",
                                "74f690f6707182839b63e0465726c7776dd63f0a174390b911d5d71f35cab94e"};
    return std::nullopt;
}

std::string sha256Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &len, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("sha256 failed");
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < len; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 15];
    }
    return out;
}

namespace {

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string thisMachine() {
    utsname u{};
    return uname(&u) == 0 ? u.machine : "";
}

}  // namespace

std::filesystem::path ensureCloudflared(HttpClient& http, const std::filesystem::path& dir,
                                        const CloudflaredSource& source) {
    const std::string machine = source.machine.empty() ? thisMachine() : source.machine;
    auto asset = source.asset ? source.asset : cloudflaredAssetFor(machine);
    if (!asset) throw CloudflaredUnavailable("no cloudflared build for " + machine);

    namespace fs = std::filesystem;
    fs::create_directories(dir);
    const fs::path target = dir / (std::string("cloudflared-") + kCloudflaredVersion);
    std::error_code ec;
    if (fs::exists(target, ec) && sha256Hex(readFile(target)) == asset->sha256) return target;

    Download d;
    d.url = source.baseUrl + "/" + kCloudflaredVersion + "/" + asset->name;
    d.maxBytes = 64L * 1024 * 1024;  // no Trello headers: this goes to GitHub
    HttpResponse r = http.download(std::move(d)).get();
    if (r.status != 200)
        throw std::runtime_error("cloudflared download: HTTP " + std::to_string(r.status));
    if (sha256Hex(r.body) != asset->sha256)
        throw CloudflaredUnavailable("cloudflared download does not match the pinned hash");

    const fs::path part = target.string() + ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
        if (!out) throw std::runtime_error("cloudflared: cannot write " + part.string());
    }
    ::chmod(part.c_str(), 0755);
    fs::rename(part, target);
    for (const auto& e : fs::directory_iterator(dir, ec))  // older pinned versions
        if (e.path().filename().string().rfind("cloudflared-", 0) == 0 && e.path() != target)
            fs::remove(e.path(), ec);
    return target;
}

}  // namespace cardinal
