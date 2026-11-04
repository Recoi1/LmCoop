#pragma once
// Scrubbing of a crash log before it leaves the PC (crash_report.cpp). Only text that is sent goes through
// this; the saved copy stays on the player's PC. It removes what must not be uploaded: passwords and tokens,
// IP addresses, Steam ids, invite codes, e-mail addresses, credentials in URLs, the Windows / Linux / macOS
// user name in paths, the PC's name, the player's chosen name and the program's own data folders.
// Header only, no regular expressions (logs are large; tools/recomp/crash_redact_test.cpp tests it).
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace port::crash {

struct Scrub {
    // Exact texts to replace (case-insensitive), for example the user name or a data folder.
    std::vector<std::pair<std::string, std::string>> literals;
};

namespace detail {

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline char low(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }
inline bool nameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ' ' || c == '~' || c == '+';
}

inline std::string replaceAll(const std::string& s, const std::string& needle, const std::string& with) {
    if (needle.size() < 3) return s;  // a one or two letter user name would wreck the log
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        bool hit = i + needle.size() <= s.size();
        for (size_t k = 0; hit && k < needle.size(); k++) hit = low(s[i + k]) == low(needle[k]);
        if (hit) out += with, i += needle.size();
        else out += s[i++];
    }
    return out;
}

// X:\Users\<name>, X:/Users/<name>, /home/<name>, /Users/<name>  ->  %USERPROFILE% / ~
inline std::string userPaths(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t skip = 0;
        const char* rep = nullptr;
        if (i + 9 <= s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && s[i + 1] == ':' && (s[i + 2] == '\\' || s[i + 2] == '/')) {
            const std::string w = s.substr(i + 3, 5);
            if (w.size() == 5 && low(w[0]) == 'u' && low(w[1]) == 's' && low(w[2]) == 'e' && low(w[3]) == 'r' && low(w[4]) == 's' &&
                i + 8 < s.size() && (s[i + 8] == '\\' || s[i + 8] == '/')) {
                skip = i + 9, rep = "%USERPROFILE%";
            }
        } else if (s.compare(i, 6, "/home/") == 0) {
            skip = i + 6, rep = "~";
        } else if (s.compare(i, 7, "/Users/") == 0) {
            skip = i + 7, rep = "~";
        }
        if (rep) {
            size_t e = skip;
            while (e < s.size() && nameChar(s[e]) && s[e] != '\\' && s[e] != '/') e++;
            while (e > skip && (s[e - 1] == ' ' || s[e - 1] == '.')) e--;  // a trailing space or dot is not part of the name
            out += rep;
            i = e;
            continue;
        }
        out += s[i++];
    }
    return out;
}

// a.b.c.d with groups of 1 to 3 digits, 0 to 255, not part of a longer dotted number.
inline std::string ipv4(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (isDigit(s[i]) && (i == 0 || (!isDigit(s[i - 1]) && s[i - 1] != '.'))) {
            size_t p = i;
            int groups = 0;
            bool ok = true;
            while (groups < 4) {
                size_t d = p;
                while (d < s.size() && isDigit(s[d])) d++;
                if (d == p || d - p > 3 || std::stoi(s.substr(p, d - p)) > 255) { ok = false; break; }
                groups++;
                p = d;
                if (groups < 4) {
                    if (p < s.size() && s[p] == '.') p++;
                    else { ok = false; break; }
                }
            }
            if (ok && groups == 4 && !(p < s.size() && (isDigit(s[p]) || (s[p] == '.' && p + 1 < s.size() && isDigit(s[p + 1]))))) {
                out += "<ip>";
                i = p;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// A token of hex digits and colons with "::" or at least five colons (an IPv6 address; clock times stay).
inline std::string ipv6(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if ((isHex(s[i]) || s[i] == ':') && (i == 0 || !(isHex(s[i - 1]) || s[i - 1] == ':' || std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_'))) {
            size_t e = i;
            int colons = 0;
            bool dbl = false;
            while (e < s.size() && (isHex(s[e]) || s[e] == ':')) {
                if (s[e] == ':') colons++, dbl |= e > i && s[e - 1] == ':';
                e++;
            }
            const bool wordAfter = e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_');
            if (colons >= 2 && (dbl || colons >= 5) && e - i >= 4 && !wordAfter) {
                out += "<ip>";
                i = e;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

inline std::string steamIds(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (isDigit(s[i]) && (i == 0 || !isDigit(s[i - 1]))) {
            size_t e = i;
            while (e < s.size() && isDigit(s[e])) e++;
            if (e - i == 17 && s.compare(i, 4, "7656") == 0) {
                out += "<steamid>";
                i = e;
                continue;
            }
            out.append(s, i, e - i);
            i = e;
            continue;
        }
        out += s[i++];
    }
    return out;
}

// "password=hunter2", "password: x", "token=...", "secret=..." (key, then = or :, then the value).
inline std::string secrets(const std::string& s) {
    static const char* keys[] = {"password", "passwd", "token", "secret", "apikey", "api_key"};
    std::string out = s;
    for (const char* k : keys) {
        const std::string key = k;
        std::string res;
        res.reserve(out.size());
        for (size_t i = 0; i < out.size();) {
            bool hit = i + key.size() <= out.size();
            for (size_t j = 0; hit && j < key.size(); j++) hit = low(out[i + j]) == key[j];
            if (hit) {
                size_t p = i + key.size();
                while (p < out.size() && (out[p] == ' ' || out[p] == '"' || out[p] == '\'')) p++;
                if (p < out.size() && (out[p] == '=' || out[p] == ':')) {
                    p++;
                    while (p < out.size() && (out[p] == ' ' || out[p] == '"' || out[p] == '\'')) p++;
                    size_t e = p;
                    while (e < out.size() && !std::isspace(static_cast<unsigned char>(out[e])) && out[e] != '"' && out[e] != '\'' && out[e] != '&' && out[e] != ',' && out[e] != ';') e++;
                    res += key + "=<hidden>";
                    i = e;
                    continue;
                }
            }
            res += out[i++];
        }
        out = std::move(res);
    }
    return out;
}

// scheme://user:pass@host  and  turn:user:pass@host  ->  scheme://<creds>@host
inline std::string urlCreds(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t after = 0;
        if (s.compare(i, 3, "://") == 0) after = i + 3;
        else if (s.compare(i, 5, "turn:") == 0) after = i + 5;
        else if (s.compare(i, 6, "turns:") == 0) after = i + 6;
        if (after) {
            size_t e = after;
            while (e < s.size() && s[e] != '@' && s[e] != '/' && !std::isspace(static_cast<unsigned char>(s[e])) && s[e] != '"' && s[e] != '\'') e++;
            if (e < s.size() && s[e] == '@' && e > after) {
                out.append(s, i, after - i);
                out += "<creds>";
                i = e;  // the @ and the host follow
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// local@domain.tld
inline std::string emails(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '@' && !out.empty()) {
            size_t b = out.size();
            while (b > 0 && (std::isalnum(static_cast<unsigned char>(out[b - 1])) || out[b - 1] == '.' || out[b - 1] == '_' || out[b - 1] == '-' || out[b - 1] == '+')) b--;
            if (b > 0 && out[b - 1] == '>') { out += s[i++]; continue; }
            size_t e = i + 1;
            while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '.' || s[e] == '-')) e++;
            const std::string domain = s.substr(i + 1, e - i - 1);
            if (b < out.size() && domain.find('.') != std::string::npos && domain.front() != '.' && domain.back() != '.') {
                out.resize(b);
                out += "<email>";
                i = e;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// "lobby ABC234:" (the internet host's invite code) and "Invite code: ABC234"
inline std::string inviteCodes(const std::string& s) {
    std::string out = s;
    for (const char* lead : {"lobby ", "Invite code: ", "invite code: ", "code=", "lobby_id=", "LM_ONLINE_CODE="}) {
        const std::string l = lead;
        std::string res;
        for (size_t i = 0; i < out.size();) {
            if (out.compare(i, l.size(), l) == 0) {
                size_t e = i + l.size();
                size_t n = 0;
                while (e + n < out.size() && (std::isupper(static_cast<unsigned char>(out[e + n])) || isDigit(out[e + n]))) n++;
                if (n >= 6 && n <= 12) {
                    res += l + "<code>";
                    i = e + n;
                    continue;
                }
            }
            res += out[i++];
        }
        out = std::move(res);
    }
    return out;
}

}  // namespace detail

inline std::string redact(std::string text, const Scrub& scrub) {
    text = detail::userPaths(text);
    for (const auto& [needle, with] : scrub.literals) text = detail::replaceAll(text, needle, with);
    text = detail::urlCreds(text);
    text = detail::secrets(text);
    text = detail::emails(text);
    text = detail::steamIds(text);
    text = detail::ipv4(text);
    text = detail::ipv6(text);
    text = detail::inviteCodes(text);
    return text;
}

}  // namespace port::crash
