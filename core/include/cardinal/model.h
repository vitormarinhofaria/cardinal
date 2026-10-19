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

Board parseBoard(const nlohmann::json& j);
List parseList(const nlohmann::json& j);
Label parseLabel(const nlohmann::json& j);
Member parseMember(const nlohmann::json& j);
Comment parseComment(const nlohmann::json& j);
Card parseCard(const nlohmann::json& j);

}  // namespace cardinal
