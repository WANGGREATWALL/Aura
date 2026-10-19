#include "regex/xregex.h"
#include "log/xlogger.h"

#include <regex>

namespace au {
namespace re {

bool matchRegexInString(const std::string& content, const std::string& regex)
{
    try {
        std::regex pattern{regex};
        return std::regex_search(content, pattern);
    } catch (const std::regex_error& e) {
        XLOG_E("Invalid regex '%s': %s\n", regex.c_str(), e.what());
        return false;
    }
}

std::string getFirstMatchInString(const std::string& content, const std::string& regex)
{
    try {
        std::regex pattern{regex};
        std::smatch match;
        if (std::regex_search(content, match, pattern)) {
            return match.str();
        }
    } catch (const std::regex_error& e) {
        XLOG_E("Invalid regex '%s': %s\n", regex.c_str(), e.what());
    }
    return std::string();
}

std::string getLastMatchInString(const std::string& content, const std::string& regex)
{
    try {
        std::regex pattern{regex};
        std::sregex_iterator it(content.begin(), content.end(), pattern);
        std::sregex_iterator end;

        std::string last;
        for (; it != end; ++it) {
            last = it->str();
        }
        return last;
    } catch (const std::regex_error& e) {
        XLOG_E("Invalid regex '%s': %s\n", regex.c_str(), e.what());
    }
    return std::string();
}

std::vector<std::string> getAllMatchesInString(const std::string& content, const std::string& regex)
{
    std::vector<std::string> strings;
    try {
        std::regex pattern{regex};
        std::sregex_iterator it(content.begin(), content.end(), pattern);
        std::sregex_iterator end;

        for (; it != end; ++it) {
            strings.push_back(it->str());
        }
    } catch (const std::regex_error& e) {
        XLOG_E("Invalid regex '%s': %s\n", regex.c_str(), e.what());
    }
    return strings;
}

std::string replaceAllMatchesInString(const std::string& content, const std::string& regex, const std::string& replacement)
{
    try {
        std::regex pattern{regex};
        return std::regex_replace(content, pattern, replacement);
    } catch (const std::regex_error& e) {
        XLOG_E("Invalid regex '%s': %s\n", regex.c_str(), e.what());
        return content;
    }
}

std::string replaceSpecificMatchInString(const std::string& content, const std::string& regex, const std::string& replacement, std::size_t idx)
{
    try {
        std::regex pattern{regex};
        std::sregex_iterator it(content.begin(), content.end(), pattern);
        std::sregex_iterator end;

        std::size_t idxCurrent = 0;
        for (; it != end; ++it) {
            if (idxCurrent == idx) {
                std::string result = content;
                const auto& match = *it;
                result.replace(match.position(), match.length(), replacement);
                return result;
            }
            ++idxCurrent;
        }
    } catch (const std::regex_error& e) {
        XLOG_E("Invalid regex '%s': %s\n", regex.c_str(), e.what());
    }
    return content;
}

} // namespace re
} // namespace au

// AURA_NS_WRAPPED
