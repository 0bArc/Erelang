// builtin/ai: the table of known formats and the helpers built on it.
#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>
#include "ai_internal.hpp"
#include "erelang/runtime.hpp"

namespace erelang::ai {

namespace {

std::vector<Kind> build_kinds() {
    const std::string date = "[0-9]{4}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12][0-9]|3[01])";
    const std::string time = "(?:[01][0-9]|2[0-3]):[0-5][0-9](?::[0-5][0-9])?";
    // The strings must outlive the table, so they are interned once.
    static const std::string datetime = date + "[T ]" + time;
    static const std::string dateStr = date;
    static const std::string timeStr = time;
    return {
        {"bool", "true|false", nullptr, "true or false", "true", true},
        {"int", "[+-]?[0-9]+", "[+-]?[0-9]*", "a whole number, optionally signed", "-42", true},
        {"float", "[+-]?[0-9]*\\.[0-9]+(?:[eE][+-]?[0-9]+)?", nullptr, "a decimal number", "3.14", true},
        {"hex", "0[xX][0-9a-fA-F]+", nullptr, "a hexadecimal number with 0x prefix", "0xFF", true},
        {"hexcolor", "#(?:[0-9a-fA-F]{6}|[0-9a-fA-F]{3})", nullptr, "a CSS hex color", "#7c3aed", false},
        {"ipv4", "(?:(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])\\.){3}(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])",
         nullptr, "an IPv4 address", "192.168.0.1", true},
        {"ipv6", "(?:[0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}", nullptr, "an IPv6 address in full form",
         "2001:0db8:0000:0000:0000:ff00:0042:8329", true},
        {"mac", "(?:[0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}", nullptr, "a MAC address", "00:1A:2B:3C:4D:5E", true},
        {"uuid", "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}", nullptr,
         "a UUID", "123e4567-e89b-12d3-a456-426614174000", true},
        {"date", dateStr.c_str(), nullptr, "an ISO date (YYYY-MM-DD)", "2026-10-21", true},
        {"datetime", datetime.c_str(), nullptr, "an ISO date and time", "2026-10-21T12:30:00", true},
        {"time", timeStr.c_str(), nullptr, "a 24 hour time (HH:MM or HH:MM:SS)", "12:30", true},
        {"email", "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}", "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+",
         "an email address", "me@site.io", false},
        {"url", "(?:https?|wss?|ftp)://[^\\s/?#]+(?:[/?#]\\S*)?", "(?:https?|wss?|ftp)://\\S*",
         "a web address", "https://erelang.dev/docs", false},
        {"semver", "[0-9]+\\.[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?", nullptr, "a semantic version", "1.4.2", true},
        {"phone", "(?:\\+[0-9][0-9 ().-]{7,14}[0-9]|\\(?[0-9]{3}\\)?[ .-][0-9]{3}[ .-][0-9]{4})", nullptr, "a phone number", "+1 555-123-4567", false},
        {"card", "[0-9]{4}[ -]?[0-9]{4}[ -]?[0-9]{4}[ -]?[0-9]{1,7}", nullptr, "a payment card number shape",
         "4111 1111 1111 1111", false},
        {"zip", "[0-9]{5}(?:-[0-9]{4})?", nullptr, "a US zip code", "90210", true},
        {"slug", "[a-z0-9]+(?:-[a-z0-9]+)*", nullptr, "a lowercase URL slug", "my-first-post", true},
        {"word", "[A-Za-z]+", "[A-Za-z]*", "letters only", "hello", true},
    };
}

const std::regex& compiled(const std::string& rx) {
    static std::vector<std::pair<std::string, std::regex>> cache;
    for (const auto& [k, v] : cache) if (k == rx) return v;
    if (cache.size() >= 64) cache.erase(cache.begin());
    cache.emplace_back(rx, std::regex(rx));
    return cache.back().second;
}

std::string search_regex(const Kind& k) {
    std::string r = std::string("(?:") + k.regex + ")";
    if (k.bounded) r = "\\b" + r + "\\b";
    return r;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const std::vector<Kind>& kinds() {
    static const std::vector<Kind> table = build_kinds();
    return table;
}

const Kind* find_kind(std::string_view name) {
    for (const auto& k : kinds()) if (name == k.name) return &k;
    return nullptr;
}

std::string kind_of(const std::string& text) {
    if (text.empty()) return "empty";
    // Order matters: the most specific format first.
    static const char* order[] = {"bool", "int", "float", "hex", "hexcolor", "ipv4", "ipv6", "mac", "uuid",
                                  "date", "datetime", "time", "email", "url", "semver", "zip", "phone",
                                  "card", "word", "slug"};
    for (const char* name : order) {
        const Kind* k = find_kind(name);
        if (k && full_match(k->regex, text)) return k->name;
    }
    return "text";
}

std::string majority_kind(const std::vector<std::string>& samples) {
    std::vector<std::pair<std::string, int>> counts;
    for (const auto& s : samples) {
        const std::string k = kind_of(s);
        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& p) { return p.first == k; });
        if (it == counts.end()) counts.emplace_back(k, 1); else ++it->second;
    }
    std::string best = "text";
    int bestCount = 0;
    for (const auto& [k, n] : counts) if (n > bestCount) { best = k; bestCount = n; }
    return best;
}

std::vector<std::string> extract(const std::string& text, const std::string& kind) {
    std::vector<std::string> out;
    const Kind* k = find_kind(kind);
    if (!k) return out;
    try {
        const std::regex& re = compiled(search_regex(*k));
        for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
            out.push_back((*it)[0].str());
        }
    } catch (...) {}
    return out;
}

std::string redact(const std::string& text, const std::string& kind) {
    const Kind* k = find_kind(kind);
    if (!k) return text;
    try {
        return std::regex_replace(text, compiled(search_regex(*k)), "[" + upper(k->name) + "]");
    } catch (...) { return text; }
}

std::string redact_all(const std::string& text) {
    std::string out = text;
    for (const char* name : {"email", "url", "uuid", "ipv4", "mac", "card", "phone"}) out = redact(out, name);
    return out;
}

std::string make_list(const std::vector<std::string>& items) {
    std::vector<Value> values;
    values.reserve(items.size());
    for (const auto& s : items) values.push_back(Value::from_string(s));
    const int id = g_nextListId++;
    g_lists[id] = std::move(values);
    return std::string("list:") + std::to_string(id);
}

std::vector<std::string> read_list(const std::string& handle, bool* ok) {
    std::vector<std::string> out;
    if (ok) *ok = false;
    if (handle.rfind("list:", 0) != 0) return out;
    int id = 0;
    try { id = std::stoi(handle.substr(5)); } catch (...) { return out; }
    const auto it = g_lists.find(id);
    if (it == g_lists.end()) return out;
    if (ok) *ok = true;
    for (const auto& v : it->second) out.push_back(legacy_string(v));
    return out;
}

}  // namespace erelang::ai
