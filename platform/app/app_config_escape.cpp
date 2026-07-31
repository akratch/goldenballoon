// app_config_escape.cpp — SDL-free value escaping for the app-prefs ini store.
//
// Split out of app_config.cpp so it links into a tiny unit test (no SDL). See
// app_config.h for the contract: escapeValue produces a single line with no raw
// newline/carriage-return, so a value (e.g. a dropped/browsed filename, which on
// macOS/Linux may legally contain '\n') can never inject a forged key=value line
// into mdkr64_app.ini on reload. unescapeValue is the exact inverse.
#include "app_config.h"

namespace AppConfig {

std::string escapeValue(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string unescapeValue(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            char n = value[++i];
            switch (n) {
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case '\\': out += '\\'; break;
                default:   out += '\\'; out += n; break;  // unknown: keep literal
            }
        } else {
            out += value[i];
        }
    }
    return out;
}

}  // namespace AppConfig
