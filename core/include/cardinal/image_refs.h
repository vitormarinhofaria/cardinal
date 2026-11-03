#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cardinal/model.h"

namespace cardinal {

// The path of `url` (before any ? or #) ends in a raster image extension Cardinal can show.
bool isImageUrl(std::string_view url);

// A teammate's attachment that is a picture (by MIME type, or by its file name/URL).
bool isImageAttachment(const Attachment& a);

// Images a description or comment shows: markdown images `![alt](url "title")` and bare
// http(s) links to an image file, in order of appearance, without repeats. Only http(s) URLs.
std::vector<std::string> imageUrlsIn(std::string_view text);

// `text` without its markdown images (`![alt](url)`), for showing it next to their thumbnails.
// Blank lines left behind are collapsed and the result is trimmed.
std::string withoutMarkdownImages(std::string_view text);

}  // namespace cardinal
