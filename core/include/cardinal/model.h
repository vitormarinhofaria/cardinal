#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace cardinal {

struct Board {
    std::string id;
    std::string name;
    bool closed = false;
};

struct List {
    std::string id;
    std::string boardId;
    std::string name;
    double pos = 0;
    int cardCount = 0;
};

struct Label {
    std::string id;
    std::string name;
    std::string color;
};

struct Member {
    std::string id;
    std::string fullName;
    std::string initials;
};

struct Comment {
    std::string id;
    std::string authorId;
    std::string text;
    std::string date;
};

struct Card {
    std::string id;
    std::string listId;
    std::string name;
    std::string desc;
    double pos = 0;
    std::vector<std::string> labelIds;
    std::vector<std::string> memberIds;
    int commentCount = 0;
    std::string dateLastActivity;
};

struct BoardSnapshot {
    Board board;
    std::vector<List> lists;  // open lists, cardCount = open cards in the list
    std::vector<Label> labels;
    std::vector<Member> members;
};

struct CardCore {
    Card card;
    std::vector<Comment> comments;  // newest first, as Trello returns them
};

struct CheckItem {
    std::string id;
    std::string name;
    bool done = false;
};

struct Checklist {
    std::string id;
    std::string name;
    std::vector<CheckItem> items;
};

struct Attachment {
    std::string id;
    std::string name;
    std::string url;
};

struct CardExtras {
    std::vector<Checklist> checklists;
    std::vector<Attachment> attachments;
    std::vector<Member> members;
};

// A board activity entry. `dataJson` is the raw `data` object for consumers that need more.
struct Action {
    std::string id;
    std::string type;
    std::string date;
    std::string memberId;
    std::string cardId;
    std::string listId;  // list the card is in after the action, when the action says so
    std::string dataJson;
};

Board parseBoard(const nlohmann::json& j);
List parseList(const nlohmann::json& j);
Label parseLabel(const nlohmann::json& j);
Member parseMember(const nlohmann::json& j);
Comment parseComment(const nlohmann::json& j);
Card parseCard(const nlohmann::json& j);
Checklist parseChecklist(const nlohmann::json& j);
Attachment parseAttachment(const nlohmann::json& j);
Action parseAction(const nlohmann::json& j);

}  // namespace cardinal
