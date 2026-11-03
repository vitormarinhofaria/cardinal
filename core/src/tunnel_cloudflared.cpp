#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <regex>
#include <stdexcept>

#include "cardinal/tunnel.h"

namespace cardinal {

std::string parseQuickTunnelUrl(const std::string& text) {
    static const std::regex re(R"(https://[a-z0-9-]+\.trycloudflare\.com)");
    std::smatch m;
    if (std::regex_search(text, m, re)) {
        if (m.str() != "https://api.trycloudflare.com") return m.str();  // the API it calls, not ours
        auto rest = text.substr(static_cast<size_t>(m.position() + m.length()));
        return parseQuickTunnelUrl(rest);
    }
    return "";
}

CloudflaredTunnel::CloudflaredTunnel(std::filesystem::path binary, std::filesystem::path home,
                                     std::function<void(const std::string&)> log,
                                     std::vector<std::string> extraArgs)
    : binary_(std::move(binary)), home_(std::move(home)), log_(std::move(log)),
      extraArgs_(std::move(extraArgs)) {}

CloudflaredTunnel::~CloudflaredTunnel() { close(); }

std::string CloudflaredTunnel::open(unsigned short port, std::function<bool()> cancelled) {
    close();
    std::filesystem::create_directories(home_);

    // Everything the child needs is built before fork: after it, only async-signal-safe calls.
    std::vector<std::string> args{binary_.string(), "tunnel", "--no-autoupdate", "--url",
                                  "http://127.0.0.1:" + std::to_string(port)};
    args.insert(args.end(), extraArgs_.begin(), extraArgs_.end());
    std::vector<std::string> env{"HOME=" + home_.string(), "PATH=/usr/bin:/bin"};
    std::vector<char*> argv, envp;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) throw std::runtime_error("cloudflared: pipe failed");
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw std::runtime_error("cloudflared: fork failed");
    }
    if (pid == 0) {
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);  // dies with the thread that started it
        if (::getppid() != parent) ::_exit(1);
        ::setpgid(0, 0);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) ::dup2(devnull, 0);
        ::dup2(fds[1], 1);
        ::dup2(fds[1], 2);
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    {
        std::lock_guard lk(mutex_);
        pid_ = pid;
        exited_ = false;
        outputEnded_ = false;
        output_.clear();
        url_.clear();
    }
    reader_ = std::thread([this, fd = fds[0]] { drain(fd); });

    const auto deadline = std::chrono::steady_clock::now() + startTimeout;
    std::unique_lock lk(mutex_);
    while (url_.empty()) {
        if (outputEnded_) {
            std::string out = output_;
            lk.unlock();
            close();
            throw std::runtime_error("cloudflared exited: " + out.substr(out.size() > 400 ? out.size() - 400 : 0));
        }
        if ((cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline) {
            lk.unlock();
            close();
            throw std::runtime_error(cancelled && cancelled() ? "cloudflared: cancelled"
                                                              : "cloudflared: no tunnel URL in time");
        }
        changed_.wait_for(lk, std::chrono::milliseconds(200));
    }
    return url_;
}

void CloudflaredTunnel::drain(int fd) {
    char buf[4096];
    std::string line;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] != '\n') {
                if (line.size() < 4096) line += buf[i];
                continue;
            }
            if (log_ && (line.find(" ERR ") != std::string::npos || line.find("trycloudflare") != std::string::npos))
                log_("cloudflared: " + line);
            std::lock_guard lk(mutex_);
            if (url_.empty()) {
                output_ += line + "\n";
                if (output_.size() > 16384) output_.erase(0, output_.size() - 16384);
                url_ = parseQuickTunnelUrl(line);
                if (!url_.empty()) changed_.notify_all();
            }
            line.clear();
        }
    }
    ::close(fd);
    std::lock_guard lk(mutex_);
    outputEnded_ = true;
    changed_.notify_all();
}

bool CloudflaredTunnel::alive() {
    std::lock_guard lk(mutex_);
    if (pid_ < 0 || exited_) return false;
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) exited_ = true;
    return !exited_ && !outputEnded_;
}

void CloudflaredTunnel::close() {
    pid_t pid;
    bool reapedAlready;
    {
        std::lock_guard lk(mutex_);
        pid = pid_;
        reapedAlready = exited_;  // alive() already collected it: the pid may belong to another
        pid_ = -1;
    }
    if (pid > 0 && !reapedAlready) {
        ::kill(-pid, SIGTERM);
        ::kill(pid, SIGTERM);  // in case setpgid had not happened yet
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 40 && !reaped; ++i) {
            if (::waitpid(pid, &status, WNOHANG) == pid) reaped = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!reaped) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
    }
    if (reader_.joinable()) reader_.join();
    std::lock_guard lk(mutex_);
    exited_ = true;
}

}  // namespace cardinal
