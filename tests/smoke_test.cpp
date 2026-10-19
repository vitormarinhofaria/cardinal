#include <gtest/gtest.h>

#include "cardinal/version.h"

TEST(Smoke, LinksCore) { EXPECT_EQ(cardinal::version(), "0.1.0"); }
