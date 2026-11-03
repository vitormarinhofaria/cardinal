#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cardinal {

// A public URL that forwards to a local HTTP port.
class Tunnel {
public:
    virtual ~Tunnel() = default;
    // Starts forwarding to http://127.0.0.1:`port` and returns the public base URL (no trailing
    // slash). Throws on failure; `cancelled` is polled while waiting. Closes any previous one.
    virtual std::string open(unsigned short port, std::function<bool()> cancelled) = 0;
    virtual bool alive() = 0;  // false once the forwarding died
    virtual void close() = 0;  // idempotent
};

// The first https://<name>.trycloudflare.com in cloudflared's log output, or "".
std::string parseQuickTunnelUrl(const std::string& text);

// A Cloudflare "quick tunnel" (no account; a new random URL each time): runs `binary` as a child
// process that dies with the thread that opened it.
class CloudflaredTunnel : public Tunnel {
public:
    // `home`: an empty directory used as HOME, so a user's ~/.cloudflared config never interferes.
    CloudflaredTunnel(std::filesystem::path binary, std::filesystem::path home,
                      std::function<void(const std::string&)> log = {},
                      std::vector<std::string> extraArgs = {});
    ~CloudflaredTunnel() override;
    CloudflaredTunnel(const CloudflaredTunnel&) = delete;
    CloudflaredTunnel& operator=(const CloudflaredTunnel&) = delete;

    std::string open(unsigned short port, std::function<bool()> cancelled) override;
    bool alive() override;
    void close() override;

    std::chrono::seconds startTimeout{30};

private:
    void drain(int fd);

    std::filesystem::path binary_, home_;
    std::function<void(const std::string&)> log_;
    std::vector<std::string> extraArgs_;

    std::mutex mutex_;
    std::condition_variable changed_;
    int pid_ = -1;
    bool exited_ = false;   // the child is gone (reaped or its output ended)
    bool outputEnded_ = false;
    std::string output_;    // kept until the URL shows up, for error messages
    std::string url_;
    std::thread reader_;
};

// No tunnel: the local URL itself (development against a local mock Trello).
class DirectTunnel : public Tunnel {
public:
    std::string open(unsigned short port, std::function<bool()>) override {
        return "http://127.0.0.1:" + std::to_string(port);
    }
    bool alive() override { return true; }
    void close() override {}
};

}  // namespace cardinal
