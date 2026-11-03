#include <gtest/gtest.h>

#include "cardinal/image_refs.h"

using namespace cardinal;
using V = std::vector<std::string>;

TEST(ImageRefs, MarkdownImageWithTitleAsTrelloPastesIt) {
    EXPECT_EQ(imageUrlsIn("antes\n![image.png](https://trello.com/1/cards/c/attachments/a/download/image.png \"\")\ndepois"),
              V{"https://trello.com/1/cards/c/attachments/a/download/image.png"});
}

TEST(ImageRefs, MarkdownImageWithoutExtensionStillCounts) {
    EXPECT_EQ(imageUrlsIn("![foto](https://example.com/render?id=4)"), V{"https://example.com/render?id=4"});
}

TEST(ImageRefs, AngleBracketsAndParenthesesInUrl) {
    EXPECT_EQ(imageUrlsIn("![a](<https://x.com/a b.png>) ![b](https://x.com/w_(1).png)"),
              (V{"https://x.com/a b.png", "https://x.com/w_(1).png"}));
}

TEST(ImageRefs, BareImageLinksButNotOtherLinks) {
    EXPECT_EQ(imageUrlsIn("veja https://x.com/a.JPG, e https://x.com/doc.pdf ou (https://x.com/b.webp?w=2)."),
              (V{"https://x.com/a.JPG", "https://x.com/b.webp?w=2"}));
}

TEST(ImageRefs, OnlyWebUrls) {
    EXPECT_TRUE(imageUrlsIn("![x](file:///etc/a.png) ![y](javascript:alert(1)) ![z](data:image/png;base64,AA)").empty());
}

TEST(ImageRefs, RepeatsAreDroppedInOrder) {
    EXPECT_EQ(imageUrlsIn("![a](https://x.com/1.png) https://x.com/2.gif https://x.com/1.png"),
              (V{"https://x.com/1.png", "https://x.com/2.gif"}));
}

TEST(ImageRefs, PlainLinkIsNotAnImage) {
    EXPECT_TRUE(imageUrlsIn("[link](https://x.com/page) e ![sem fim](https://x.com/a.png").size() == 1);
    EXPECT_TRUE(imageUrlsIn("nada aqui ![alt]\n(https://x.com/page)").empty());
}

TEST(ImageRefs, ImageAttachments) {
    Attachment a;
    a.name = "foto.jpeg";
    EXPECT_TRUE(isImageAttachment(a));
    a.mimeType = "application/pdf";
    EXPECT_FALSE(isImageAttachment(a));
    a.mimeType = "image/svg+xml";
    EXPECT_FALSE(isImageAttachment(a));
    a.mimeType = "image/png";
    a.name = "sem-extensao";
    EXPECT_TRUE(isImageAttachment(a));
    Attachment link;
    link.url = "https://x.com/page";
    link.name = "https://x.com/page";
    EXPECT_FALSE(isImageAttachment(link));
}

