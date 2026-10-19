#include <nlohmann/json.hpp>

#include "cardinal/model.h"

namespace cardinal {
namespace {

using nlohmann::json;

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double num(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : 0.0;
}

std::vector<std::string> strings(const json& j, const char* key) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& v : *it)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

}  // namespace

Board parseBoard(const json& j) {
    return {str(j, "id"), str(j, "name"), j.value("closed", false)};
}

List parseList(const json& j) {
    return {str(j, "id"), str(j, "idBoard"), str(j, "name"), num(j, "pos"), 0};
}

Label parseLabel(const json& j) { return {str(j, "id"), str(j, "name"), str(j, "color")}; }

Member parseMember(const json& j) {
    return {str(j, "id"), str(j, "fullName"), str(j, "initials")};
}

Comment parseComment(const json& j) {
    Comment c;
    c.id = str(j, "id");
    c.authorId = str(j, "idMemberCreator");
    c.date = str(j, "date");
    auto data = j.find("data");
    if (data != j.end() && data->is_object()) c.text = str(*data, "text");
    return c;
}

Card parseCard(const json& j) {
    Card c;
    c.id = str(j, "id");
    c.listId = str(j, "idList");
    c.name = str(j, "name");
    c.desc = str(j, "desc");
    c.pos = num(j, "pos");
    c.labelIds = strings(j, "idLabels");
    c.memberIds = strings(j, "idMembers");
    c.dateLastActivity = str(j, "dateLastActivity");
    auto badges = j.find("badges");
    if (badges != j.end() && badges->is_object()) c.commentCount = badges->value("comments", 0);
    return c;
}

}  // namespace cardinal
