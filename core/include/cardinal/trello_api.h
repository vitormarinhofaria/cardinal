#pragma once

#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cardinal/model.h"
#include "cardinal/scheduler.h"

namespace cardinal {

// Non-2xx answer from Trello (401 invalid token, 404 gone, 429 limited after retries, ...).
class ApiError : public std::runtime_error {
public:
    ApiError(int status, std::string body)
        : std::runtime_error("trello http " + std::to_string(status)),
          status_(status),
          body_(std::move(body)) {}
    int status() const { return status_; }
    const std::string& body() const { return body_; }

private:
    int status_;
    std::string body_;
};

struct CardEdit {
    std::optional<std::string> name;
    std::optional<std::string> desc;
};

// Percent-encodes every byte outside the RFC 3986 unreserved set.
std::string urlEncode(std::string_view s);

// Typed Trello REST calls that ask only for the fields the UI shows. The returned futures are
// deferred: the JSON is parsed when .get() is called, so call it on a worker thread. Failures
// surface from .get() as ApiError (HTTP status) or std::runtime_error (transport, bad JSON).
class TrelloApi {
public:
    TrelloApi(Requester& requester, std::string key, std::string token,
              std::string baseUrl = "https://api.trello.com/1");

    std::future<std::vector<Board>> boards();
    std::future<BoardSnapshot> board(const std::string& boardId, Priority priority);
    std::future<std::vector<Card>> listCards(const std::string& listId, Priority priority);
    std::future<CardCore> cardCore(const std::string& cardId);
    std::future<CardExtras> cardExtras(const std::string& cardId);
    std::future<CardFields> cardFields(const std::string& cardId);
    std::future<std::vector<Action>> actionsSince(const std::string& boardId,
                                                  const std::string& iso);

    std::future<Card> moveCard(const std::string& cardId, const std::string& listId, double pos);
    std::future<Card> createCard(const std::string& listId, const std::string& name);
    std::future<Card> updateCard(const std::string& cardId, const CardEdit& edit);
    std::future<void> addLabel(const std::string& cardId, const std::string& labelId);
    std::future<void> addMember(const std::string& cardId, const std::string& memberId);
    std::future<Comment> addComment(const std::string& cardId, const std::string& text);

private:
    Requester& requester_;
    std::string key_;
    std::string token_;
    std::string base_;
};

}  // namespace cardinal
