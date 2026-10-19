#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cardinal/http.h"

namespace cardinal {
namespace {

struct GlobalCurl {
    GlobalCurl() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~GlobalCurl() { curl_global_cleanup(); }
};

size_t collect(char* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

class CurlHttpClient : public HttpClient {
public:
    explicit CurlHttpClient(int workers) {
        static GlobalCurl global;
        for (int i = 0; i < workers; ++i) threads_.emplace_back([this] { work(); });
    }

    ~CurlHttpClient() override {
        shutdown();
        for (auto& t : threads_) t.join();
    }

    void shutdown() override {
        std::deque<Task> dropped;
        {
            std::lock_guard lk(mutex_);
            stop_ = true;
            aborting_ = true;  // transfers in flight notice this within ~50 ms
            dropped.swap(tasks_);
        }
        wake_.notify_all();
        for (auto& t : dropped)
            t.promise.set_exception(std::make_exception_ptr(std::runtime_error("http: shutting down")));
    }

    std::future<HttpResponse> request(Method method, std::string url, std::string body) override {
        Task task{method, std::move(url), std::move(body), {}};
        auto future = task.promise.get_future();
        {
            std::lock_guard lk(mutex_);
            if (stop_) {
                task.promise.set_exception(std::make_exception_ptr(std::runtime_error("http: shutting down")));
                return future;
            }
            tasks_.push_back(std::move(task));
        }
        wake_.notify_one();
        return future;
    }

private:
    struct Task {
        Method method;
        std::string url;
        std::string body;
        std::promise<HttpResponse> promise;
    };

    // Each worker owns one easy handle, so connections are reused (keep-alive, HTTP/2).
    void work() {
        CURL* curl = curl_easy_init();
        CURLM* multi = curl_multi_init();  // also owns the connection cache: keep-alive still works
        while (true) {
            Task task;
            {
                std::unique_lock lk(mutex_);
                wake_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (tasks_.empty()) break;  // stop_ and drained
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            perform(multi, curl, task);
        }
        curl_multi_cleanup(multi);
        curl_easy_cleanup(curl);
    }

    void perform(CURLM* multi, CURL* curl, Task& task) {
        curl_easy_reset(curl);
        std::string out;
        curl_easy_setopt(curl, CURLOPT_URL, task.url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        switch (task.method) {
            case Method::Get:
                break;
            case Method::Post:
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                break;
            case Method::Put:
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
                break;
            case Method::Delete:
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
                break;
        }
        if (task.method != Method::Get && task.method != Method::Delete) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, task.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(task.body.size()));
        }
        // Drive the transfer ourselves so a shutdown can abort it within ~50 ms.
        curl_multi_add_handle(multi, curl);
        int running = 1;
        CURLcode rc = CURLE_OK;
        bool aborted = false;
        while (running) {
            curl_multi_perform(multi, &running);
            if (!running) break;
            if (aborting_) {
                aborted = true;
                break;
            }
            curl_multi_poll(multi, nullptr, 0, 50, nullptr);
        }
        if (!aborted) {
            int left = 0;
            while (CURLMsg* m = curl_multi_info_read(multi, &left))
                if (m->msg == CURLMSG_DONE && m->easy_handle == curl) rc = m->data.result;
        }
        curl_multi_remove_handle(multi, curl);
        if (aborted) {
            task.promise.set_exception(std::make_exception_ptr(std::runtime_error("http: shutting down")));
            return;
        }
        if (rc != CURLE_OK) {
            task.promise.set_exception(std::make_exception_ptr(
                std::runtime_error(std::string("http: ") + curl_easy_strerror(rc))));
            return;
        }
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        task.promise.set_value({static_cast<int>(status), std::move(out)});
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> tasks_;
    bool stop_ = false;
    std::atomic<bool> aborting_{false};
    std::vector<std::thread> threads_;
};

}  // namespace

std::unique_ptr<HttpClient> makeCurlHttpClient(int workers) {
    return std::make_unique<CurlHttpClient>(workers);
}

}  // namespace cardinal
