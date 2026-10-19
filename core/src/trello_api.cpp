#include "cardinal/trello_api.h"

#include <nlohmann/json.hpp>
#include <type_traits>
#include <utility>

namespace cardinal {
namespace {

using nlohmann::json;
using Params = std::vector<std::pair<std::string, std::string>>;

constexpr const char* kCardListFields = "name,idList,pos,idLabels,idMembers,dateLastActivity,badges";
constexpr const char* kActionFilter =
    "commentCard,createCard,updateCard,deleteCard,copyCard,moveCardToBoard,moveCardFromBoard,"
    "addLabelToCard,removeLabelFromCard,addMemberToCard,removeMemberFromCard,createList,updateList";

std::string encodeParams(const Params& params) {
    std::string out;
    for (const auto& [k, v] : params) {
        if (!out.empty()) out += '&';
        out += urlEncode(k) + '=' + urlEncode(v);
    }
    return out;
}

json parseOk(const HttpResponse& r) {
    if (r.status < 200 || r.status >= 300) throw ApiError(r.status, r.body);
    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error("trello: invalid JSON in response");
    return j;
}

template <class T, class Fn>
std::vector<T> mapArray(const json& j, Fn fn) {
    std::vector<T> out;
    if (!j.is_array()) return out;
    out.reserve(j.size());
    for (const auto& e : j) out.push_back(fn(e));
    return out;
}

json field(const json& j, const char* key) {
    auto it = j.find(key);
    return it == j.end() ? json::array() : *it;
}

// Parse on .get(): no thread per request, and failures surface where the caller handles them.
template <class Fn>
auto then(std::future<HttpResponse> response, Fn fn) {
    using R = std::invoke_result_t<Fn, const json&>;
    return std::async(std::launch::deferred,
                      [response = std::move(response), fn = std::move(fn)]() mutable -> R {
                          return fn(parseOk(response.get()));
                      });
}

}  // namespace

std::string urlEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

TrelloApi::TrelloApi(Requester& requester, std::string key, std::string token, std::string baseUrl)
    : requester_(requester),
      key_(std::move(key)),
      token_(std::move(token)),
      base_(std::move(baseUrl)) {}

namespace {

struct Url {
    const std::string& base;
    const std::string& key;
    const std::string& token;
    std::string operator()(const std::string& path, Params params = {}) const {
        params.emplace_back("key", key);
        params.emplace_back("token", token);
        return base + path + '?' + encodeParams(params);
    }
};

}  // namespace

std::future<std::vector<Board>> TrelloApi::boards() {
    Url url{base_, key_, token_};
    return then(requester_.submit(Priority::Visible, Method::Get,
                                  url("/members/me/boards",
                                      {{"fields", "name,closed"}, {"filter", "open"}})),
                [](const json& j) { return mapArray<Board>(j, parseBoard); });
}

std::future<BoardSnapshot> TrelloApi::board(const std::string& boardId, Priority priority) {
    Url url{base_, key_, token_};
    const std::string path = "/boards/" + urlEncode(boardId);
    auto info = requester_.submit(priority, Method::Get,
                                  url(path, {{"fields", "name,closed"},
                                             {"labels", "all"},
                                             {"label_fields", "name,color"},
                                             {"label_limit", "1000"},
                                             {"members", "all"},
                                             {"member_fields", "fullName,initials"}}));
    auto lists = requester_.submit(priority, Method::Get,
                                   url(path + "/lists", {{"filter", "open"},
                                                         {"fields", "name,pos"},
                                                         {"cards", "open"},
                                                         {"card_fields", "id"}}));
    return std::async(std::launch::deferred, [info = std::move(info), lists = std::move(lists),
                                              boardId]() mutable {
        // Read both before throwing so neither response is left dangling.
        HttpResponse infoResponse = info.get();
        HttpResponse listsResponse = lists.get();
        json infoJson = parseOk(infoResponse);
        json listsJson = parseOk(listsResponse);

        BoardSnapshot snap;
        snap.board = parseBoard(infoJson);
        snap.labels = mapArray<Label>(field(infoJson, "labels"), parseLabel);
        snap.members = mapArray<Member>(field(infoJson, "members"), parseMember);
        snap.lists = mapArray<List>(listsJson, [&](const json& l) {
            List list = parseList(l);
            list.boardId = boardId;
            auto cards = l.find("cards");
            list.cardCount = cards != l.end() && cards->is_array() ? static_cast<int>(cards->size()) : 0;
            return list;
        });
        return snap;
    });
}

std::future<std::vector<Card>> TrelloApi::listCards(const std::string& listId, Priority priority) {
    Url url{base_, key_, token_};
    return then(requester_.submit(priority, Method::Get,
                                  url("/lists/" + urlEncode(listId) + "/cards",
                                      {{"fields", kCardListFields}})),
                [](const json& j) { return mapArray<Card>(j, parseCard); });
}

std::future<CardCore> TrelloApi::cardCore(const std::string& cardId) {
    Url url{base_, key_, token_};
    return then(
        requester_.submit(Priority::Interactive, Method::Get,
                          url("/cards/" + urlEncode(cardId),
                              {{"fields", std::string("name,desc,closed,") + kCardListFields},
                               {"actions", "commentCard"},
                               {"actions_limit", "50"},
                               {"action_fields", "data,date,idMemberCreator"}})),
        [](const json& j) {
            return CardCore{parseCard(j), mapArray<Comment>(field(j, "actions"), parseComment)};
        });
}

std::future<CardExtras> TrelloApi::cardExtras(const std::string& cardId) {
    Url url{base_, key_, token_};
    return then(requester_.submit(Priority::Visible, Method::Get,
                                  url("/cards/" + urlEncode(cardId),
                                      {{"fields", "id"},
                                       {"checklists", "all"},
                                       {"checklist_fields", "name"},
                                       {"attachments", "true"},
                                       {"attachment_fields", "name,url"},
                                       {"members", "true"},
                                       {"member_fields", "fullName,initials"}})),
                [](const json& j) {
                    return CardExtras{mapArray<Checklist>(field(j, "checklists"), parseChecklist),
                                      mapArray<Attachment>(field(j, "attachments"), parseAttachment),
                                      mapArray<Member>(field(j, "members"), parseMember)};
                });
}

std::future<CardFields> TrelloApi::cardFields(const std::string& cardId) {
    Url url{base_, key_, token_};
    return then(requester_.submit(Priority::Interactive, Method::Get,
                                  url("/cards/" + urlEncode(cardId),
                                      {{"fields", "name,desc,dateLastActivity"}})),
                [](const json& j) {
                    Card c = parseCard(j);
                    return CardFields{c.name, c.desc, c.dateLastActivity};
                });
}

std::future<std::vector<Action>> TrelloApi::actionsSince(const std::string& boardId,
                                                         const std::string& iso) {
    Url url{base_, key_, token_};
    return then(requester_.submit(Priority::Background, Method::Get,
                                  url("/boards/" + urlEncode(boardId) + "/actions",
                                      {{"since", iso}, {"limit", "1000"}, {"filter", kActionFilter}})),
                [](const json& j) { return mapArray<Action>(j, parseAction); });
}

std::future<Card> TrelloApi::moveCard(const std::string& cardId, const std::string& listId,
                                      double pos) {
    Url url{base_, key_, token_};
    return then(requester_.submit(Priority::Interactive, Method::Put,
                                  url("/cards/" + urlEncode(cardId)),
                                  encodeParams({{"idList", listId}, {"pos", std::to_string(pos)}})),
                [](const json& j) { return parseCard(j); });
}

std::future<Card> TrelloApi::createCard(const std::string& listId, const std::string& name) {
    Url url{base_, key_, token_};
    return then(requester_.submit(
                    Priority::Interactive, Method::Post, url("/cards"),
                    encodeParams({{"idList", listId}, {"name", name}, {"pos", "bottom"}})),
                [](const json& j) { return parseCard(j); });
}

std::future<Card> TrelloApi::updateCard(const std::string& cardId, const CardEdit& edit) {
    Url url{base_, key_, token_};
    Params body;
    if (edit.name) body.emplace_back("name", *edit.name);
    if (edit.desc) body.emplace_back("desc", *edit.desc);
    return then(requester_.submit(Priority::Interactive, Method::Put,
                                  url("/cards/" + urlEncode(cardId)), encodeParams(body)),
                [](const json& j) { return parseCard(j); });
}

std::future<void> TrelloApi::addLabel(const std::string& cardId, const std::string& labelId) {
    Url url{base_, key_, token_};
    return then(requester_.submit(Priority::Interactive, Method::Post,
                                  url("/cards/" + urlEncode(cardId) + "/idLabels"),
                                  encodeParams({{"value", labelId}})),
                [](const json&) {});
}

std::future<void> TrelloApi::addMember(const std::string& cardId, const std::string& memberId) {
    Url url{base_, key_, token_};
    return then(requester_.submit(Priority::Interactive, Method::Post,
                                  url("/cards/" + urlEncode(cardId) + "/idMembers"),
                                  encodeParams({{"value", memberId}})),
                [](const json&) {});
}

std::future<Comment> TrelloApi::addComment(const std::string& cardId, const std::string& text) {
    Url url{base_, key_, token_};
    return then(requester_.submit(Priority::Interactive, Method::Post,
                                  url("/cards/" + urlEncode(cardId) + "/actions/comments"),
                                  encodeParams({{"text", text}})),
                [](const json& j) { return parseComment(j); });
}

}  // namespace cardinal
