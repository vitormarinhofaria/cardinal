#pragma once

#include <future>
#include <memory>
#include <string>

namespace cardinal {

enum class Method { Get, Post, Put, Delete };

struct HttpResponse {
    int status = 0;
    std::string body;
};

// Transport. A transport failure (no connection, timeout) is reported as an exception in the
// future; any HTTP status, including 4xx/5xx, is a normal HttpResponse.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual std::future<HttpResponse> request(Method method, std::string url, std::string body) = 0;
};

std::unique_ptr<HttpClient> makeCurlHttpClient(int workers = 4);

}  // namespace cardinal
