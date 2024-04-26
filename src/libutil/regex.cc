#include <string>
#include <regex>

namespace nix::regex {
std::string quoteRegexChars(const std::string & raw)
{
    static auto specialRegex = std::regex(R"([.^$\\*+?()\[\]{}|])");
    return std::regex_replace(raw, specialRegex, R"(\$&)");
}

std::regex storePathRegex(const std::string & storeDir)
{
    return std::regex(quoteRegexChars(storeDir) + R"(/[0-9a-z]+[0-9a-zA-Z\+\-\._\?=]*)");
}

}
