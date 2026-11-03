// JSON mínimo: lo justo para los mensajes del Quest y los archivos de ajustes de VRmin Host
#pragma once
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    bool has(const std::string &k) const { return t == Obj && o.count(k) > 0 && o.at(k).t != Null; }
    const Json &operator[](const std::string &k) const {
        static const Json none;
        if (t != Obj) return none;
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    std::string str(const std::string &def = "") const { return t == Str ? s : def; }
    double num(double def = 0) const { return t == Num ? n : t == Bool ? (b ? 1 : 0) : def; }
    bool boolean(bool def = false) const { return t == Bool ? b : t == Num ? n != 0 : def; }

    static Json parse(const std::string &text, bool *ok = nullptr) {
        size_t i = 0;
        Json j;
        bool good = parseValue(text, i, j);
        skip(text, i);
        if (ok) *ok = good;
        return good ? j : Json();
    }

    static std::string esc(const std::string &v) {
        std::string out = "\"";
        for (unsigned char c : v) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
            }
        }
        return out + "\"";
    }
    std::string dump() const {
        switch (t) {
        case Null: return "null";
        case Bool: return b ? "true" : "false";
        case Num: {
            char buf[64];
            if (n == (long long)n && n > -1e15 && n < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)n);
            else snprintf(buf, sizeof buf, "%.6g", n);
            return buf;
        }
        case Str: return esc(s);
        case Arr: {
            std::string r = "[";
            for (size_t k = 0; k < a.size(); k++) { if (k) r += ","; r += a[k].dump(); }
            return r + "]";
        }
        case Obj: {
            std::string r = "{";
            bool first = true;
            for (auto &kv : o) { if (!first) r += ","; first = false; r += esc(kv.first) + ":" + kv.second.dump(); }
            return r + "}";
        }
        }
        return "null";
    }

    // Para armar objetos
    static Json object() { Json j; j.t = Obj; return j; }
    static Json array() { Json j; j.t = Arr; return j; }
    Json(const char *v) : t(Str), s(v) {}
    Json(const std::string &v) : t(Str), s(v) {}
    Json(bool v) : t(Bool), b(v) {}
    Json(int v) : t(Num), n(v) {}
    Json(long long v) : t(Num), n((double)v) {}
    Json(unsigned v) : t(Num), n(v) {}
    Json(double v) : t(Num), n(v) {}
    Json() {}
    Json &set(const std::string &k, const Json &v) { t = Obj; o[k] = v; return *this; }
    Json &push(const Json &v) { t = Arr; a.push_back(v); return *this; }

private:
    static void skip(const std::string &x, size_t &i) { while (i < x.size() && (x[i] == ' ' || x[i] == '\n' || x[i] == '\r' || x[i] == '\t')) i++; }
    static bool parseString(const std::string &x, size_t &i, std::string &out) {
        if (i >= x.size() || x[i] != '"') return false;
        i++;
        while (i < x.size() && x[i] != '"') {
            char c = x[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= x.size()) return false;
            char e = x[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > x.size()) return false;
                unsigned cp = (unsigned)strtoul(x.substr(i, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= x.size() && x[i] == '\\' && x[i + 1] == 'u') {
                    unsigned lo = (unsigned)strtoul(x.substr(i + 2, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                }
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                break;
            }
            default: out += e;
            }
        }
        if (i >= x.size()) return false;
        i++;
        return true;
    }
    static bool parseValue(const std::string &x, size_t &i, Json &j) {
        skip(x, i);
        if (i >= x.size()) return false;
        char c = x[i];
        if (c == '{') {
            j.t = Obj; i++; skip(x, i);
            if (i < x.size() && x[i] == '}') { i++; return true; }
            while (true) {
                skip(x, i);
                std::string k;
                if (!parseString(x, i, k)) return false;
                skip(x, i);
                if (i >= x.size() || x[i] != ':') return false;
                i++;
                Json v;
                if (!parseValue(x, i, v)) return false;
                j.o[k] = v;
                skip(x, i);
                if (i < x.size() && x[i] == ',') { i++; continue; }
                if (i < x.size() && x[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (c == '[') {
            j.t = Arr; i++; skip(x, i);
            if (i < x.size() && x[i] == ']') { i++; return true; }
            while (true) {
                Json v;
                if (!parseValue(x, i, v)) return false;
                j.a.push_back(v);
                skip(x, i);
                if (i < x.size() && x[i] == ',') { i++; continue; }
                if (i < x.size() && x[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (c == '"') { j.t = Str; return parseString(x, i, j.s); }
        if (x.compare(i, 4, "true") == 0) { j.t = Bool; j.b = true; i += 4; return true; }
        if (x.compare(i, 5, "false") == 0) { j.t = Bool; j.b = false; i += 5; return true; }
        if (x.compare(i, 4, "null") == 0) { j.t = Null; i += 4; return true; }
        char *end = nullptr;
        j.n = strtod(x.c_str() + i, &end);
        if (end == x.c_str() + i) return false;
        j.t = Num;
        i = end - x.c_str();
        return true;
    }
};
