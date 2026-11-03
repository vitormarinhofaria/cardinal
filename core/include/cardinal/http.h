#pragma once

#include <future>
#include <memory>
#include <string>
#include <vector>

namespace cardinal {

enum class Method { Get, Post, Put, Delete };

struct HttpResponse {
    int status = 0;
    std::string body;
};

// A GET for a file (an image): may follow redirects, carries extra headers, and fails (as a
// transport error) when the body grows past `maxBytes` (0 = no limit).
struct Download {
    std::string url;
    std::vector<std::string> headers;  // "Name: value"
    long maxBytes = 0;
};

// Transport. A transport failure (no connection, timeout) is reported as an exception in the
// future; any HTTP status, including 4xx/5xx, is a normal HttpResponse.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual std::future<HttpResponse> request(Method method, std::string url, std::string body) = 0;
    // Default: a plain GET without the extras (enough for fakes).
    virtual std::future<HttpResponse> download(Download d) {
        return request(Method::Get, std::move(d.url), "");
    }
    // Abort transfers in flight, fail everything queued and refuse new requests. Idempotent.
    virtual void shutdown() {}
};

std::unique_ptr<HttpClient> makeCurlHttpClient(int workers = 4);

}  // namespace cardinal
