// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: in-game menu language packs (port/docs/language-packs.md).
// The launcher passes the pack in use as TLG_LANG_PACK=<path of the JSON file>. Its "menu" section
// maps the menu's English text to the translation: {"menu": {"Save game": "Salvar jogo", ...}}.
// Without the variable, without the file, or for a text the pack lacks, the menu stays English.
// A translation whose printf placeholders (%d, %s, %.1f, ...) differ from the English one is not
// used: a wrong placeholder would crash the game in snprintf.
#pragma once

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace TlgLang {

// A small JSON reader: enough for a language pack (objects, arrays, strings, numbers, literals).
// Kept: the string values of the top-level "menu" object and the "code" string; the rest is skipped.
class Reader {
public:
    explicit Reader(const std::string& text) : s(text) {}

    bool Menu(std::unordered_map<std::string, std::string>& out, std::string& code) {
        Space();
        if (!Eat('{')) return false;
        Space();
        if (Eat('}')) return true;
        for (;;) {
            std::string key;
            Space();
            if (!String(key)) return false;
            Space();
            if (!Eat(':')) return false;
            Space();
            if (key == "menu" && Peek() == '{') {
                if (!Texts(out)) return false;
            } else if (key == "code" && Peek() == '"') {
                if (!String(code)) return false;
            } else if (!Skip()) {
                return false;
            }
            Space();
            if (Eat('}')) return true;
            if (!Eat(',')) return false;
        }
    }

private:
    const std::string& s;
    size_t i = 0;

    char Peek() const { return i < s.size() ? s[i] : '\0'; }
    bool Eat(char c) {
        if (Peek() != c) return false;
        ++i;
        return true;
    }
    void Space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    bool Texts(std::unordered_map<std::string, std::string>& out) {
        Eat('{');
        Space();
        if (Eat('}')) return true;
        for (;;) {
            std::string key, value;
            Space();
            if (!String(key)) return false;
            Space();
            if (!Eat(':')) return false;
            Space();
            if (Peek() == '"') {
                if (!String(value)) return false;
                if (!value.empty()) out[key] = value;
            } else if (!Skip()) {
                return false;
            }
            Space();
            if (Eat('}')) return true;
            if (!Eat(',')) return false;
        }
    }

    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned& cp) {
        if (i + 4 > s.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool String(std::string& out) {
        if (!Eat('"')) return false;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    unsigned low;
                    if (!Hex4(low)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                Utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool Skip(int depth = 0) {
        if (depth > 64) return false;
        Space();
        const char c = Peek();
        if (c == '"') {
            std::string ignored;
            return String(ignored);
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++i;
            Space();
            if (Eat(close)) return true;
            for (;;) {
                Space();
                if (c == '{') {
                    std::string ignored;
                    if (!String(ignored)) return false;
                    Space();
                    if (!Eat(':')) return false;
                }
                if (!Skip(depth + 1)) return false;
                Space();
                if (Eat(close)) return true;
                if (!Eat(',')) return false;
            }
        }
        const size_t start = i;
        while (i < s.size() && std::strchr(",}] \t\r\n", s[i]) == nullptr) ++i;
        return i > start;
    }
};

// The printf conversions of a text in order ("%d%s"). Same rule as langpack.py: flags without the
// space flag, then a conversion letter; anything else ("0% turns", "100%:", "%%") is plain text.
// The menu's own format strings never use the space flag.
inline std::string Placeholders(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') continue;
        size_t j = i + 1;
        while (j < text.size() && text[j] != '\0' && std::strchr("-+#0123456789.*", text[j])) ++j;
        while (j < text.size() && text[j] != '\0' && std::strchr("hlLqjzt", text[j])) ++j;
        if (j < text.size() && text[j] != '\0' && std::strchr("diouxXeEfFgGaAcsp", text[j])) {
            out += text.substr(i, j - i + 1);
            i = j;
        }
    }
    return out;
}

struct Pack {
    std::unordered_map<std::string, std::string> texts;
    std::string code; // "ja", "zh_CN", ...; empty without a pack
};

inline const Pack& Loaded() {
    static const Pack pack = [] {
        Pack result;
        auto& loaded = result.texts;
        const char* path = std::getenv("TLG_LANG_PACK");
        if (!path || !*path) return result;
        std::FILE* f = std::fopen(path, "rb");
        if (!f) {
            std::fprintf(stderr, "[tlg] language pack %s: cannot open it, menu stays English\n", path);
            return result;
        }
        std::string data;
        char buf[16384];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) data.append(buf, n);
        std::fclose(f);
        if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
        if (!Reader(data).Menu(loaded, result.code)) {
            std::fprintf(stderr, "[tlg] language pack %s: not valid JSON, menu stays English\n", path);
            return Pack{};
        }
        size_t dropped = 0;
        for (auto it = loaded.begin(); it != loaded.end();) {
            if (Placeholders(it->first) != Placeholders(it->second)) {
                it = loaded.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        std::fprintf(stderr, "[tlg] language pack %s: %zu menu texts, %zu dropped (placeholders)\n", path,
                     loaded.size(), dropped);
        return result;
    }();
    return pack;
}

inline const std::unordered_map<std::string, std::string>& Texts() { return Loaded().texts; }

// True when the pack's menu texts hold characters past the Latin, Greek and Cyrillic blocks of
// the built-in font (DejaVu Sans): CJK, kana, Hangul, ... (UTF-8 lead bytes 0xE2 0xBA and up).
inline bool NeedsExtraFont() {
    for (const auto& [en, text] : Texts()) {
        for (size_t i = 0; i + 1 < text.size(); ++i) {
            const auto lead = static_cast<unsigned char>(text[i]);
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (lead >= 0xE3 || (lead == 0xE2 && next >= 0xBA)) return true;
        }
    }
    return false;
}

// A system font for the pack's script, merged into the menu font: the file and the face index in a
// .ttc collection. TLG_LANG_FONT=<file> picks one by hand (face 0). Japanese packs get a Japanese
// font first, the others a Chinese one first (the same characters are drawn differently).
struct ExtraFont {
    std::string path;
    int face = 0;
};

inline std::vector<ExtraFont> ExtraFontCandidates() {
    std::vector<ExtraFont> out;
    if (const char* own = std::getenv("TLG_LANG_FONT"); own && *own) out.push_back({own, 0});
    const std::string& code = Loaded().code;
    // No pack in another script: Japanese first, for the Japanese title in the menu's heading.
    const bool japanese = code.rfind("ja", 0) == 0 || !NeedsExtraFont();
    const bool korean = code.rfind("ko", 0) == 0;
    // Traditional Chinese (zh_TW, zh_HK): a Traditional font first, so the characters take the Taiwan forms.
    const bool traditional = code == "zh_TW" || code == "zh_HK";
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    const std::string fonts = std::string(windir && *windir ? windir : "C:\\Windows") + "\\Fonts\\";
    const std::vector<ExtraFont> ja = {{fonts + "YuGothM.ttc", 0}, {fonts + "meiryo.ttc", 0}, {fonts + "msgothic.ttc", 0}};
    const std::vector<ExtraFont> zh = traditional
        ? std::vector<ExtraFont>{{fonts + "msjh.ttc", 0}, {fonts + "mingliu.ttc", 0}, {fonts + "msyh.ttc", 0}, {fonts + "simsun.ttc", 0}}
        : std::vector<ExtraFont>{{fonts + "msyh.ttc", 0}, {fonts + "simsun.ttc", 0}};
    const std::vector<ExtraFont> ko = {{fonts + "malgun.ttf", 0}};
#else
    // Noto Sans CJK collections: face 0 Japanese, 1 Korean, 2 Simplified, 3 Traditional Chinese.
    const char* noto[] = {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/noto-cjk/NotoSansCJKsc-Regular.otf"};
    std::vector<ExtraFont> ja, zh, ko;
    for (const char* path : noto) {
        ja.push_back({path, 0});
        const bool single = std::string_view(path).ends_with(".otf"); // one face only
        zh.push_back({path, single ? 0 : traditional ? 3 : 2});
        ko.push_back({path, 1});
    }
    for (const char* path : {"/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
                             "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"}) {
        zh.push_back({path, 0});
        ja.push_back({path, 0});
    }
#endif
    const auto& first = japanese ? ja : korean ? ko : zh;
    out.insert(out.end(), first.begin(), first.end());
    for (const auto* rest : {&ja, &zh, &ko}) {
        if (rest != &first) out.insert(out.end(), rest->begin(), rest->end());
    }
    return out;
}

// The menu text in the pack's language. `en` is a string literal (or other text that lives as long
// as the game); the result is too.
inline const char* Tr(const char* en) {
    const auto& texts = Texts();
    if (texts.empty() || !en) return en;
    static std::mutex lock;
    static std::unordered_map<const char*, const char*> by_pointer; // literals: one lookup each
    std::lock_guard guard(lock);
    if (auto hit = by_pointer.find(en); hit != by_pointer.end()) return hit->second;
    const auto it = texts.find(en);
    const char* result = it == texts.end() ? en : it->second.c_str();
    by_pointer.emplace(en, result);
    return result;
}

} // namespace TlgLang
