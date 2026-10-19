#if ENABLE_TEST_XREGEX

#include "gtest/gtest.h"
#include "regex/xregex.h"

TEST(XRegex, MatchSearch) {
    const std::string text = "Hello, my name is Vincent. Email: vincent@example.com";
    EXPECT_TRUE(au::re::matchRegexInString(text, "Vincent"));
    EXPECT_TRUE(au::re::matchRegexInString(text, R"(\w+@\w+\.\w+)"));
    EXPECT_FALSE(au::re::matchRegexInString(text, "Wang"));

    // The API searches for a substring; it does not require a full-string match.
    EXPECT_TRUE(au::re::matchRegexInString("123ab", R"(\d+)"));
    EXPECT_TRUE(au::re::matchRegexInString("", ".*"));
    EXPECT_FALSE(au::re::matchRegexInString("abc", R"([)"));
}

TEST(XRegex, GetMatches) {
    const std::string text = "cat bat hat";
    const auto all = au::re::getAllMatchesInString(text, R"(\w+at)");
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0], "cat");
    EXPECT_EQ(all[1], "bat");
    EXPECT_EQ(all[2], "hat");
    EXPECT_EQ(au::re::getFirstMatchInString(text, R"(\w+at)"), "cat");
    EXPECT_EQ(au::re::getLastMatchInString(text, R"(\w+at)"), "hat");

    EXPECT_EQ(au::re::getFirstMatchInString("abc", R"(\d+)"), "");
    EXPECT_EQ(au::re::getLastMatchInString("abc", R"(\d+)"), "");
    EXPECT_TRUE(au::re::getAllMatchesInString("abc", R"(\d+)").empty());
    EXPECT_TRUE(au::re::getAllMatchesInString("", R"(\w+)").empty());
    EXPECT_EQ(au::re::getFirstMatchInString("abc", R"([)"), "");
    EXPECT_EQ(au::re::getLastMatchInString("abc", R"([)"), "");
    EXPECT_TRUE(au::re::getAllMatchesInString("abc", R"([)").empty());
}

TEST(XRegex, ReplaceAll) {
    EXPECT_EQ(au::re::replaceAllMatchesInString("hello hello", "hello", "hi"), "hi hi");
    EXPECT_EQ(au::re::replaceAllMatchesInString("2024-01-15", R"((\d{4})-(\d{2}))", "$2/$1"), "01/2024-15");
    EXPECT_EQ(au::re::replaceAllMatchesInString("hello world", "xyz", "hi"), "hello world");
    EXPECT_EQ(au::re::replaceAllMatchesInString("abc", R"([)", "X"), "abc");
}

TEST(XRegex, ReplaceSpecific) {
    const std::string text = "aa bb aa";
    EXPECT_EQ(au::re::replaceSpecificMatchInString(text, "aa", "cc", 0), "cc bb aa");
    EXPECT_EQ(au::re::replaceSpecificMatchInString(text, "aa", "cc", 1), "aa bb cc");
    EXPECT_EQ(au::re::replaceSpecificMatchInString(text, "aa", "cc", 999), text);
    EXPECT_EQ(au::re::replaceSpecificMatchInString(text, "xyz", "cc", 0), text);
    EXPECT_EQ(au::re::replaceSpecificMatchInString(text, R"([)", "cc", 0), text);

    // This function inserts replacement literally, unlike regex_replace.
    EXPECT_EQ(au::re::replaceSpecificMatchInString("a1b2", R"(\d)", "$1", 0), "a$1b2");
}

#endif  // ENABLE_TEST_XREGEX
