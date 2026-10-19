#ifndef AURA_XREGEX_H_
#define AURA_XREGEX_H_

#include <cstddef>
#include <string>
#include <vector>

#include "sys/xsystem.h"  // for AU_API

namespace au {
namespace re {

/**
 * @brief Test whether a regex pattern matches anywhere in the string.
 *
 * @param content  The string to search in.
 * @param regex    ECMAScript regex pattern.
 * @return true if at least one match is found.
 *
 * @code
 *   au::re::matchRegexInString("hello123", "\\d+");        // true
 *   au::re::matchRegexInString("hello",    "\\d+");        // false
 *   au::re::matchRegexInString("IMG_001.png", "\\.png$"); // true
 * @endcode
 */
AU_API bool matchRegexInString(const std::string& content, const std::string& regex);

/**
 * @brief Return the first substring that matches the regex pattern.
 *
 * @param content  The string to search in.
 * @param regex    ECMAScript regex pattern.
 * @return The first matched substring, or empty string if no match.
 *
 * @code
 *   au::re::getFirstMatchInString("w=128,h=256", "\\d+"); // "128"
 *   au::re::getFirstMatchInString("no digits",  "\\d+"); // ""
 * @endcode
 */
AU_API std::string getFirstMatchInString(const std::string& content, const std::string& regex);

/**
 * @brief Return the last substring that matches the regex pattern.
 *
 * @param content  The string to search in.
 * @param regex    ECMAScript regex pattern.
 * @return The last matched substring, or empty string if no match.
 *
 * @code
 *   au::re::getLastMatchInString("w=128,h=256", "\\d+"); // "256"
 * @endcode
 */
AU_API std::string getLastMatchInString(const std::string& content, const std::string& regex);

/**
 * @brief Collect all non-overlapping substrings that match the regex pattern.
 *
 * @param content  The string to search in.
 * @param regex    ECMAScript regex pattern.
 * @return Vector of matched substrings (empty if no match).
 *
 * @code
 *   au::re::getAllMatchesInString("a1b22c333", "\\d+");
 *   // returns {"1", "22", "333"}
 * @endcode
 */
AU_API std::vector<std::string> getAllMatchesInString(const std::string& content, const std::string& regex);

/**
 * @brief Replace all occurrences that match the regex pattern.
 *
 * @param content      The original string.
 * @param regex        ECMAScript regex pattern.
 * @param replacement  The replacement string (supports $1, $2 back-references).
 * @return The resulting string after all replacements, or original if no match.
 *
 * @code
 *   au::re::replaceAllMatchesInString("a1b2c3", "\\d", "X");
 *   // returns "aXbXcX"
 *
 *   au::re::replaceAllMatchesInString("2024-01-15", "(\\d{4})-(\\d{2})", "$2/$1");
 *   // returns "01/2024-15"
 * @endcode
 */
AU_API std::string replaceAllMatchesInString(const std::string& content, const std::string& regex, const std::string& replacement);

/**
 * @brief Replace only the n-th occurrence (0-based) that matches the regex pattern.
 *
 * @param content      The original string.
 * @param regex        ECMAScript regex pattern.
 * @param replacement  The replacement string.
 * @param idx          Zero-based index of the match to replace.
 * @return The resulting string with the specified match replaced,
 *         or original if no match at that index.
 *
 * @code
 *   au::re::replaceSpecificMatchInString("a1b2c3", "\\d", "X", 0); // "aXb2c3"
 *   au::re::replaceSpecificMatchInString("a1b2c3", "\\d", "X", 2); // "a1b2cX"
 * @endcode
 */
AU_API std::string replaceSpecificMatchInString(const std::string& content, const std::string& regex, const std::string& replacement, std::size_t idx);

} // namespace re
} // namespace au

#endif // AURA_XREGEX_H_
