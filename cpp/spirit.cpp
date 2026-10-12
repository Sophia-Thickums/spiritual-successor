// spirit — the Spiritual Successor: put EVERY GPU in a machine to work.
//
// C++17 port of spirit.py — a drop-in behavioural match (same subcommands, same
// flags, same human-readable output shape, same JSON structure). Standard library
// only; no third-party dependencies. Builds with `make`.
//
// Faithful to the Python original, including:
//   * the sysfs PCIe-link walk (chain_for / binding_link / link_gbps): the MINIMUM
//     negotiated width/speed along the chain from the root complex;
//   * probe() card DB; assign_roles()/score(); the WORKLOADS table and plan();
//   * run(): env-pins one card with DRI_PRIME and MESA_VK_DEVICE_SELECT=vid:did!
//     (the trailing `!` is EXCLUSIVE);
//   * play(): spawns a mount namespace hiding the other cards' /dev/dri nodes,
//     starts encoders on the spares, launches the game, kills encoders on exit;
//   * fg() frame-gen feasibility math; doctor().
//
// Original: Copyright 2026 Sophia Marie DeClue. MIT.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <map>
#include <optional>
#include <poll.h>
#include <pwd.h>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using std::string;
using std::vector;

extern char **environ;

// ★ THE ONE VERSION STRING — identical to the Python `VERSION` and the Rust `VERSION`. `--version`
// prints it as `spirit 1.0.0`; it is the value a bug report should quote back.
static const char *VERSION = "1.0.0";

// ─────────────────────────────────────────────────────────────────────────────
// tiny ordered JSON DOM (key order preserved; int/float distinguished)
// ─────────────────────────────────────────────────────────────────────────────

struct J {
    enum class T { NUL, BOOL, INT, FLT, STR, ARR, OBJ };
    T t = T::NUL;
    bool b = false;
    long long i = 0;
    double f = 0;
    string s;
    vector<J> arr;
    vector<std::pair<string, J>> obj;

    static J nul() { return J(); }
    static J boolean(bool v) { J j; j.t = T::BOOL; j.b = v; return j; }
    static J integer(long long v) { J j; j.t = T::INT; j.i = v; return j; }
    static J flt(double v) { J j; j.t = T::FLT; j.f = v; return j; }
    static J str(const string &v) { J j; j.t = T::STR; j.s = v; return j; }
    static J array() { J j; j.t = T::ARR; return j; }
    static J object() { J j; j.t = T::OBJ; return j; }
    J &put(const string &k, J v) { obj.emplace_back(k, std::move(v)); return *this; }
    J &push(J v) { arr.push_back(std::move(v)); return *this; }
};

static const J *jget(const J &o, const string &k) {
    if (o.t != J::T::OBJ) return nullptr;
    for (auto &kv : o.obj) if (kv.first == k) return &kv.second;
    return nullptr;
}
static string sfield(const J &o, const string &k) {
    const J *p = jget(o, k);
    if (!p || p->t != J::T::STR) return "";
    return p->s;
}
// Python renders a None with "%s" as the literal "None"; mimic that for %s sites.
static string s_or_none(const J &o, const string &k) {
    const J *p = jget(o, k);
    if (!p || p->t == J::T::NUL) return "None";
    if (p->t == J::T::STR) return p->s;
    return "";
}
static std::optional<long long> ifield(const J &o, const string &k) {
    const J *p = jget(o, k);
    if (!p) return std::nullopt;
    if (p->t == J::T::INT) return p->i;
    if (p->t == J::T::FLT) return (long long)p->f;
    return std::nullopt;
}
static std::optional<double> ffield(const J &o, const string &k) {
    const J *p = jget(o, k);
    if (!p) return std::nullopt;
    if (p->t == J::T::FLT) return p->f;
    if (p->t == J::T::INT) return (double)p->i;
    return std::nullopt;
}
static bool bfield(const J &o, const string &k) {
    const J *p = jget(o, k);
    return p && p->t == J::T::BOOL && p->b;
}

// ── JSON parser ──────────────────────────────────────────────────────────────
struct JsonParser {
    const string &s;
    size_t p = 0;
    explicit JsonParser(const string &str) : s(str) {}
    [[noreturn]] void fail(const char *m) { throw std::runtime_error(m); }
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    }
    void expect(const char *w) {
        size_t n = strlen(w);
        if (s.compare(p, n, w) != 0) fail("bad literal");
        p += n;
    }
    J parse() {
        ws();
        J v = value();
        ws();
        if (p != s.size()) fail("trailing data");
        return v;
    }
    J value() {
        ws();
        if (p >= s.size()) fail("unexpected eof");
        char c = s[p];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return parse_string();
        if (c == 't') { expect("true"); return J::boolean(true); }
        if (c == 'f') { expect("false"); return J::boolean(false); }
        if (c == 'n') { expect("null"); return J::nul(); }
        return number();
    }
    J parse_string() {
        if (s[p] != '"') fail("expected string");
        p++;
        string out;
        while (p < s.size()) {
            unsigned char c = (unsigned char)s[p++];
            if (c == '"') return J::str(out);
            if (c == '\\') {
                if (p >= s.size()) fail("bad escape");
                char e = s[p++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned int cp = hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < s.size() &&
                            s[p] == '\\' && s[p + 1] == 'u') {
                            p += 2;
                            unsigned int lo = hex4();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(out, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                out.push_back((char)c);
            }
        }
        fail("unterminated string");
    }
    unsigned int hex4() {
        if (p + 4 > s.size()) fail("bad \\u");
        unsigned int v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else fail("bad hex");
        }
        return v;
    }
    static void utf8(string &out, unsigned int cp) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    J number() {
        size_t start = p;
        if (p < s.size() && (s[p] == '-' || s[p] == '+')) p++;
        bool isflt = false;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++;
        if (p < s.size() && s[p] == '.') { isflt = true; p++; while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++; }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            isflt = true; p++;
            if (p < s.size() && (s[p] == '-' || s[p] == '+')) p++;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++;
        }
        if (p == start) fail("bad number");
        string tok = s.substr(start, p - start);
        if (isflt) return J::flt(strtod(tok.c_str(), nullptr));
        errno = 0;
        long long v = strtoll(tok.c_str(), nullptr, 10);
        return J::integer(v);
    }
    J object() {
        p++; // {
        J o = J::object();
        ws();
        if (p < s.size() && s[p] == '}') { p++; return o; }
        while (true) {
            ws();
            string k = parse_string().s;
            ws();
            if (p >= s.size() || s[p] != ':') fail("expected :");
            p++;
            ws();
            o.obj.emplace_back(k, value());
            ws();
            if (p >= s.size()) fail("unterminated object");
            if (s[p] == ',') { p++; continue; }
            if (s[p] == '}') { p++; break; }
            fail("expected , or }");
        }
        return o;
    }
    J array() {
        p++; // [
        J a = J::array();
        ws();
        if (p < s.size() && s[p] == ']') { p++; return a; }
        while (true) {
            ws();
            a.arr.push_back(value());
            ws();
            if (p >= s.size()) fail("unterminated array");
            if (s[p] == ',') { p++; continue; }
            if (s[p] == ']') { p++; break; }
            fail("expected , or ]");
        }
        return a;
    }
};

// ── Python-compatible float repr and json.dumps(indent=2) ─────────────────────
static string py_float_repr(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    string s(buf, r.ptr);
    if (s.find_first_of(".eE") == string::npos && s.find("inf") == string::npos &&
        s.find("nan") == string::npos)
        s += ".0";
    return s;
}

static void json_escape(const string &in, string &out) {
    out.push_back('"');
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"') { out += "\\\""; i++; }
        else if (c == '\\') { out += "\\\\"; i++; }
        else if (c == '\n') { out += "\\n"; i++; }
        else if (c == '\r') { out += "\\r"; i++; }
        else if (c == '\t') { out += "\\t"; i++; }
        else if (c == '\b') { out += "\\b"; i++; }
        else if (c == '\f') { out += "\\f"; i++; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; i++; }
        else if (c < 0x80) { out.push_back((char)c); i++; }
        else {
            unsigned int cp = 0; int len = 1;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
            else { cp = c; len = 1; }
            for (int k = 1; k < len && i + k < in.size(); k++)
                cp = (cp << 6) | ((unsigned char)in[i + k] & 0x3F);
            i += len;
            char b[16];
            if (cp <= 0xFFFF) { snprintf(b, sizeof(b), "\\u%04x", cp); out += b; }
            else {
                unsigned int v = cp - 0x10000;
                snprintf(b, sizeof(b), "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
                out += b;
            }
        }
    }
    out.push_back('"');
}

static void json_dump(const J &v, int level, string &out) {
    switch (v.t) {
        case J::T::NUL: out += "null"; break;
        case J::T::BOOL: out += v.b ? "true" : "false"; break;
        case J::T::INT: out += std::to_string(v.i); break;
        case J::T::FLT: out += py_float_repr(v.f); break;
        case J::T::STR: json_escape(v.s, out); break;
        case J::T::ARR: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            string ind((size_t)(level + 1) * 2, ' ');
            for (size_t k = 0; k < v.arr.size(); k++) {
                out += ind;
                json_dump(v.arr[k], level + 1, out);
                if (k + 1 < v.arr.size()) out += ",";
                out += "\n";
            }
            out += string((size_t)level * 2, ' ');
            out += "]";
            break;
        }
        case J::T::OBJ: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            string ind((size_t)(level + 1) * 2, ' ');
            for (size_t k = 0; k < v.obj.size(); k++) {
                out += ind;
                json_escape(v.obj[k].first, out);
                out += ": ";
                json_dump(v.obj[k].second, level + 1, out);
                if (k + 1 < v.obj.size()) out += ",";
                out += "\n";
            }
            out += string((size_t)level * 2, ' ');
            out += "}";
            break;
        }
    }
}
static string json_dumps_indent2(const J &v) {
    string out;
    json_dump(v, 0, out);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// small utils
// ─────────────────────────────────────────────────────────────────────────────
static string trim(const string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
static string join(const vector<string> &v, const string &sep) {
    string out;
    for (size_t i = 0; i < v.size(); i++) { if (i) out += sep; out += v[i]; }
    return out;
}
static bool starts_with(const string &s, const string &p) { return s.rfind(p, 0) == 0; }

// ── UTF-8 helpers (★ C1). Python's str slicing and %-width padding count CODE POINTS, not bytes.
// Slicing the port's std::string by byte cut a multi-byte card name mid-character (invalid UTF-8)
// and printf %-Ns padded by byte, so a non-ASCII name misaligned the columns. Both now count
// code points, exactly as the Python does.
static size_t utf8_cplen(const string &s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) len = 1;
        i += len; n++;
    }
    return n;
}
static string utf8_cpslice(const string &s, size_t cps) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < cps) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) len = 1;
        i += len; n++;
    }
    return s.substr(0, i);
}
// Python's "%-Nw" % s: pad on the RIGHT to N code points.
static string pad_display(const string &s, size_t width) {
    string r = s;
    size_t n = utf8_cplen(s);
    while (n < width) { r.push_back(' '); n++; }
    return r;
}

// Python repr() for a plain string: single-quoted unless it contains a single quote.
static string py_repr(const string &s) {
    bool has_sq = s.find('\'') != string::npos;
    bool has_dq = s.find('"') != string::npos;
    char q = has_sq && !has_dq ? '"' : '\'';
    string out;
    out.push_back(q);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == q) { out.push_back('\\'); out.push_back(q); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(c);
    }
    out.push_back(q);
    return out;
}

// Python repr() of a list of strings, e.g. ['renderD999'] — used where the Python prints sorted(want).
static string py_list_repr(const vector<string> &v) {
    string out = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) out += ", ";
        out += py_repr(v[i]);
    }
    out += "]";
    return out;
}

// os.path.expanduser("~")
static string home_dir() {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return pw->pw_dir;
    return "";
}

// DRI_PRIME wants udev's ID_PATH_TAG: "pci-0000_03_00_0" — a BDF's dots and colons become UNDERSCORES.
// The ':'/'.' spelling matches NO device and Mesa silently ignores it, falling back to the default card
// (MEASURED 2026-10-11, both ways on this box: glxinfo under `DRI_PRIME=pci-0000:12:00.0` reported the
// default card; under `pci-0000_12_00_0` it reported the pinned one). The tool wrote the ignored form
// for its whole life, in `run`, `play` and `enable` — a fault all three builds shared, so no
// differential suite could see it.
static string dri_prime(const string &bdf) {
    string s = bdf;
    for (char &c : s) if (c == ':' || c == '.') c = '_';
    return "pci-" + s;
}

// glob.glob(<dir>/<prefix>*) — a single trailing "*", hidden names not matched by a non-'.' prefix.
static vector<string> glob_prefix(const string &dir, const string &prefix) {
    vector<string> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    dirent *e;
    while ((e = readdir(d))) {
        string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (starts_with(n, prefix)) out.push_back(dir + "/" + n);
    }
    closedir(d);
    return out;
}


// ─────────────────────────────────────────────────────────────────────────────
// sysfs readers
// ─────────────────────────────────────────────────────────────────────────────
static std::optional<string> read_trim(const string &path) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return std::nullopt;
    string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    return trim(data);
}
static std::optional<long long> read_int(const string &p) {
    auto v = read_trim(p);
    if (!v || v->empty()) return std::nullopt;
    try {
        size_t idx = 0;
        long long x = std::stoll(*v, &idx, 10);
        if (idx != v->size()) return std::nullopt; // Python int() demands full consumption
        return x;
    } catch (...) { return std::nullopt; }
}
static std::optional<double> read_speed(const string &p) {
    auto v = read_trim(p);
    if (!v || v->empty()) return std::nullopt;
    size_t i = 0;
    while (i < v->size() && !isspace((unsigned char)(*v)[i])) i++;
    string tok = v->substr(0, i);
    try {
        size_t idx = 0;
        double d = std::stod(tok, &idx);
        if (idx != tok.size()) return std::nullopt;
        return d;
    } catch (...) { return std::nullopt; }
}

// ── the true host link: minimum negotiated width/speed along the sysfs chain ──
static vector<string> chain_for(const string &bdf) {
    string path = "/sys/bus/pci/devices/" + bdf;
    char buf[PATH_MAX];
    string real;
    if (realpath(path.c_str(), buf)) real = buf; else real = path;
    vector<string> segs;
    size_t start = 0;
    while (start <= real.size()) {
        size_t pos = real.find('/', start);
        string tok = (pos == string::npos) ? real.substr(start) : real.substr(start, pos - start);
        if (starts_with(tok, "0000:")) segs.push_back(tok);
        if (pos == string::npos) break;
        start = pos + 1;
    }
    if (segs.empty()) segs.push_back(bdf);
    return segs;
}

struct Link {
    std::optional<string> mseg;
    std::optional<long long> mw;
    std::optional<double> ms;
    std::optional<long long> xw;
    std::optional<double> xs;
};
static Link binding_link(const string &bdf) {
    Link L;
    for (auto &seg : chain_for(bdf)) {
        string base = "/sys/bus/pci/devices/" + seg;
        auto w = read_int(base + "/current_link_width");
        if (w && (!L.mw || *w < *L.mw)) { L.mw = w; L.mseg = seg; }
        auto s = read_speed(base + "/current_link_speed");
        if (s && (!L.ms || *s < *L.ms)) L.ms = s;
        auto xwi = read_int(base + "/max_link_width");
        if (xwi && (!L.xw || *xwi < *L.xw)) L.xw = xwi;
        auto xsi = read_speed(base + "/max_link_speed");
        if (xsi && (!L.xs || *xsi < *L.xs)) L.xs = xsi;
    }
    return L;
}

static double py_round2(double x) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", x);
    return strtod(buf, nullptr);
}

static const std::map<double, double> SPEED_GBPS = {
    {2.5, 0.25}, {5.0, 0.50}, {8.0, 1.00}, {16.0, 1.97}, {32.0, 4.00}};
static const std::map<double, string> GEN_NAME = {
    {2.5, "Gen1"}, {5.0, "Gen2"}, {8.0, "Gen3"}, {16.0, "Gen4"}, {32.0, "Gen5"}};
static const double EFF = 0.85;
static const double PLAN_HEADROOM = 0.60;

static std::optional<double> link_gbps(std::optional<long long> width, std::optional<double> speed) {
    if (!width || !speed) return std::nullopt;
    auto it = SPEED_GBPS.find(*speed);
    if (it == SPEED_GBPS.end()) return std::nullopt;
    return py_round2(it->second * (double)(*width) * EFF);
}
static std::optional<string> gen_name(std::optional<double> xs) {
    if (!xs) return std::nullopt;
    auto it = GEN_NAME.find(*xs);
    if (it != GEN_NAME.end()) return it->second;
    char b[32];
    snprintf(b, sizeof(b), "%.1f GT/s", *xs);
    return string(b);
}

// ── process helpers ─────────────────────────────────────────────────────────
static std::optional<string> which(const string &cmd) {
    const char *p = getenv("PATH");
    if (!p) return std::nullopt;
    string path = p;
    size_t start = 0;
    while (start <= path.size()) {
        size_t pos = path.find(':', start);
        string dir = (pos == string::npos) ? path.substr(start) : path.substr(start, pos - start);
        if (!dir.empty()) {
            string full = dir + "/" + cmd;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0)
                return full;
        }
        if (pos == string::npos) break;
        start = pos + 1;
    }
    return std::nullopt;
}

static vector<string> current_env() {
    vector<string> e;
    for (char **p = environ; *p; ++p) e.push_back(*p);
    return e;
}
static void set_env(vector<string> &e, const string &k, const string &v) {
    string prefix = k + "=";
    for (auto &s : e) {
        if (starts_with(s, prefix)) { s = prefix + v; return; }
    }
    e.push_back(prefix + v);
}
static vector<char *> to_cargv(const vector<string> &v) {
    vector<char *> a;
    a.reserve(v.size() + 1);
    for (auto &s : v) a.push_back(const_cast<char *>(s.c_str()));
    a.push_back(nullptr);
    return a;
}

// run to completion, capture stdout (stderr -> /dev/null). env optional.
static std::optional<string> run_capture(const vector<string> &argv, const vector<string> *envp) {
    int fds[2];
    if (pipe(fds) != 0) return std::nullopt;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return std::nullopt; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 2); close(dn); }
        auto a = to_cargv(argv);
        if (envp) {
            auto e = to_cargv(*envp);
            execvpe(a[0], a.data(), e.data());
        } else {
            execvp(a[0], a.data());
        }
        _exit(127);
    }
    close(fds[1]);
    string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, n);
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    return out;
}

// Capture BOTH stdout and stderr, like subprocess.run(capture_output=True, text=True).
// ok=false means the child could not be exec'd or the timeout expired (Python raises).
struct ProcResult {
    bool ok = false;
    int rc = 0;
    string out, err, reason;
};
static ProcResult run_capture_both(const vector<string> &argv, const vector<string> *envp,
                                   int timeout_s) {
    ProcResult R;
    int po[2], pe[2];
    if (pipe(po) != 0) { R.reason = "pipe failed"; return R; }
    if (pipe(pe) != 0) { close(po[0]); close(po[1]); R.reason = "pipe failed"; return R; }
    pid_t pid = fork();
    if (pid < 0) {
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        R.reason = "fork failed"; return R;
    }
    if (pid == 0) {
        close(po[0]); close(pe[0]);
        dup2(po[1], 1); dup2(pe[1], 2);
        close(po[1]); close(pe[1]);
        auto a = to_cargv(argv);
        if (envp) { auto e = to_cargv(*envp); execvpe(a[0], a.data(), e.data()); }
        else execvp(a[0], a.data());
        _exit(127);
    }
    close(po[1]); close(pe[1]);
    struct pollfd fds[2];
    fds[0].fd = po[0]; fds[0].events = POLLIN;
    fds[1].fd = pe[0]; fds[1].events = POLLIN;
    bool open_o = true, open_e = true;
    int remaining = timeout_s; // seconds; coarse deadline
    while (open_o || open_e) {
        int pr = poll(fds, 2, remaining > 0 ? remaining * 1000 : 0);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { // timeout
            kill(pid, SIGKILL);
            R.ok = false;
            R.reason = "timed out";
            break;
        }
        if (open_o && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            ssize_t n = read(po[0], buf, sizeof(buf));
            if (n > 0) R.out.append(buf, n);
            else if (n == 0) { open_o = false; close(po[0]); }
        }
        if (open_e && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            ssize_t n = read(pe[0], buf, sizeof(buf));
            if (n > 0) R.err.append(buf, n);
            else if (n == 0) { open_e = false; close(pe[0]); }
        }
        if (remaining == 0) remaining = 0;
    }
    if (open_o) close(po[0]);
    if (open_e) close(pe[0]);
    int status;
    waitpid(pid, &status, 0);
    if (R.reason == "timed out") return R;
    if (WIFEXITED(status)) { R.rc = WEXITSTATUS(status); R.ok = true; }
    else if (WIFSIGNALED(status)) { R.rc = -WTERMSIG(status); R.ok = true; }
    else R.ok = false;
    // exec failure surfaces as exit 127 with no captured output on a normal path
    return R;
}

// spawn and wait; returns exit code (negative for signal), like subprocess.call.
// ★ ALSO REPORT AN exec FAILURE (fix 2026-10-11, outside review). Python's subprocess.call raises
// FileNotFoundError when the program cannot be found, and play() turns that into a clean rc=127 line
// plus THE TAIL. A bare fork/execvp cannot tell an exec failure from a program that exited 127, so the
// child reports the errno over a close-on-exec pipe.
struct SpawnResultEx { int rc = 127; bool exec_failed = false; int err = 0; string prog; };
static SpawnResultEx spawn_wait_ex(const vector<string> &argv, const vector<string> &env) {
    SpawnResultEx R;
    int pfd[2];
    if (pipe(pfd) != 0) return R;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return R; }
    if (pid == 0) {
        close(pfd[0]);
        fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
        auto a = to_cargv(argv);
        auto e = to_cargv(env);
        execvpe(a[0], a.data(), e.data());
        int err = errno;
        ssize_t w = write(pfd[1], &err, sizeof(err));
        (void)w;
        _exit(127);
    }
    close(pfd[1]);
    int err = 0;
    ssize_t n = read(pfd[0], &err, sizeof(err));
    close(pfd[0]);
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) R.rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) R.rc = -WTERMSIG(status);
    if (n == (ssize_t)sizeof(err)) {
        R.exec_failed = true;
        R.err = err;
        R.prog = argv.empty() ? "" : argv[0];
    }
    return R;
}

static int spawn_wait(const vector<string> &argv, const vector<string> &env, bool devnull) {
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        if (devnull) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); close(dn); }
        }
        auto a = to_cargv(argv);
        auto e = to_cargv(env);
        execvpe(a[0], a.data(), e.data());
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return 0;
}

// spawn without waiting (Popen); returns pid, -1 on failure.
static pid_t spawn_detached(const vector<string> &argv, const vector<string> &env) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); close(dn); }
        auto a = to_cargv(argv);
        auto e = to_cargv(env);
        execvpe(a[0], a.data(), e.data());
        _exit(127);
    }
    return pid;
}

static std::optional<string> lspci_name(const string &bdf) {
    if (!which("lspci")) return std::nullopt;
    auto out = run_capture({"lspci", "-mm", "-s", bdf}, nullptr);
    if (!out) return std::nullopt;
    // split on '"'
    vector<string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = out->find('"', start);
        if (pos == string::npos) { parts.push_back(out->substr(start)); break; }
        parts.push_back(out->substr(start, pos - start));
        start = pos + 1;
    }
    if (parts.size() >= 8) return trim(parts[5] + " " + parts[7]);
    return std::nullopt;
}

static std::pair<int, int> connectors(const string &card) {
    int online = 0, total = 0;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return {0, 0};
    vector<string> names;
    dirent *e;
    string pre = card + "-";
    while ((e = readdir(d))) {
        string n = e->d_name;
        if (starts_with(n, pre)) names.push_back(n);
    }
    closedir(d);
    for (auto &n : names) {
        string st = "/sys/class/drm/" + n + "/status";
        struct stat sb;
        if (stat(st.c_str(), &sb) != 0) continue;
        auto v = read_trim(st);
        total++;
        if (v && *v == "connected") online++;
    }
    return {online, total};
}

static std::optional<string> card_render_node(const string &card) {
    string dir = "/sys/class/drm/" + card + "/device/drm";
    DIR *d = opendir(dir.c_str());
    if (!d) return std::nullopt;
    vector<string> names;
    dirent *e;
    while ((e = readdir(d))) {
        string n = e->d_name;
        if (starts_with(n, "renderD")) names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    if (names.empty()) return std::nullopt;
    return names[0];
}

// ─────────────────────────────────────────────────────────────────────────────
// probe
// ─────────────────────────────────────────────────────────────────────────────
static J probe() {
    const char *fake = getenv("SPIRIT_DEVICES");
    if (fake && *fake) {
        // ★ A FIXTURE THAT CANNOT BE READ MUST NOT SILENTLY BECOME THE REAL MACHINE (fix 2026-10-11,
        // outside review). This used to fall through to the live probe whenever the path did not
        // exist, or crash with a traceback when the JSON was malformed — so a typo in the variable
        // name for `spirit probe` meant you were reading your actual hardware and thought you were
        // reading a fixture. That is the ambient-state trap this whole project has a law about.
        struct stat st;
        if (stat(fake, &st) != 0) {
            fprintf(stderr, "SPIRIT_DEVICES is set to %s, which does not exist. Refusing to "
                            "silently read the real machine instead.\n", py_repr(fake).c_str());
            exit(2);
        }
        FILE *f = fopen(fake, "r");
        if (!f) {
            int e = errno;
            fprintf(stderr, "SPIRIT_DEVICES is set to %s, which is not readable JSON (%s). "
                            "Refusing to silently read the real machine instead.\n",
                    py_repr(fake).c_str(), strerror(e));
            exit(2);
        }
        string data;
        { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n); }
        fclose(f);
        try {
            J db = JsonParser(data).parse();
            if (!jget(db, "gpufan")) db.put("gpufan", J::str("probe/1.0.0"));
            return db;
        } catch (const std::exception &exc) {
            // The parse-error text is the ONE part of this message each language spells its own way.
            fprintf(stderr, "SPIRIT_DEVICES is set to %s, which is not readable JSON (%s). "
                            "Refusing to silently read the real machine instead.\n",
                    py_repr(fake).c_str(), exc.what());
            exit(2);
        }
    }

    vector<J> gpus;
    // enumerate /sys/class/drm/card* matching ^card[0-9]+$
    vector<std::pair<long long, string>> cards;
    DIR *d = opendir("/sys/class/drm");
    if (d) {
        dirent *e;
        while ((e = readdir(d))) {
            string n = e->d_name;
            if (n.size() >= 5 && starts_with(n, "card")) {
                bool digits = true;
                for (size_t k = 4; k < n.size(); k++)
                    if (!isdigit((unsigned char)n[k])) { digits = false; break; }
                if (digits && n.size() > 4) cards.emplace_back(std::stoll(n.substr(4)), n);
            }
        }
        closedir(d);
    }
    std::sort(cards.begin(), cards.end());

    for (auto &cp : cards) {
        string card = cp.second;
        string base = "/sys/class/drm/" + card + "/device";
        struct stat st;
        if (stat(base.c_str(), &st) != 0) continue;

        char rbuf[PATH_MAX];
        string bdf;
        if (realpath(base.c_str(), rbuf)) {
            string r = rbuf;
            size_t sl = r.find_last_of('/');
            bdf = (sl == string::npos) ? r : r.substr(sl + 1);
        } else {
            bdf = "";
        }
        string vendor = read_trim(base + "/vendor").value_or("");
        string device = read_trim(base + "/device").value_or("");
        string uev = read_trim(base + "/uevent").value_or("");
        string driver;
        {
            string cur;
            for (char c : uev) {
                if (c == '\n') {
                    if (starts_with(cur, "DRIVER=")) { driver = cur.substr(7); break; }
                    cur.clear();
                } else cur.push_back(c);
            }
            if (driver.empty() && starts_with(cur, "DRIVER=")) driver = cur.substr(7);
        }
        Link L = binding_link(bdf);
        auto own_w = read_int(base + "/current_link_width");
        auto total = read_int(base + "/mem_info_vram_total");
        auto used = read_int(base + "/mem_info_vram_used");
        string cn = card;
        auto conn = connectors(cn);
        int online = conn.first, nconn = conn.second;
        bool integrated = total.has_value() && *total < 2LL * 1024 * 1024 * 1024;

        std::optional<string> nm = lspci_name(bdf);
        string name = nm ? *nm
                         : ((vendor.size() > 2 ? vendor.substr(2) : string()) + ":" +
                            (device.size() > 2 ? device.substr(2) : string()));

        J link = J::object();
        link.put("binding_element", L.mseg ? J::str(*L.mseg) : J::nul());
        link.put("max_width", L.xw ? J::integer(*L.xw) : J::nul());
        auto mg = gen_name(L.xs);
        link.put("max_gen", mg ? J::str(*mg) : J::nul());
        auto gmax = link_gbps(L.xw, L.xs);
        link.put("gbps_max", gmax ? J::flt(*gmax) : J::nul());
        link.put("current_width", L.mw ? J::integer(*L.mw) : J::nul());
        auto cg = gen_name(L.ms);
        link.put("current_gen", cg ? J::str(*cg) : J::nul());
        auto gcur = link_gbps(L.mw, L.ms);
        link.put("gbps_current", gcur ? J::flt(*gcur) : J::nul());
        bool idle_deg = (L.mw && L.xw && *L.mw < *L.xw) || (L.ms && L.xs && *L.ms < *L.xs);
        link.put("idle_degraded", J::boolean(idle_deg));
        link.put("own_reports_width", own_w ? J::integer(*own_w) : J::nul());
        bool capped = (own_w && L.xw && *own_w > *L.xw);
        link.put("capped_by_topology", J::boolean(capped));

        J g = J::object();
        g.put("card", J::str(cn));
        g.put("bdf", J::str(bdf));
        g.put("name", J::str(name));
        g.put("driver", J::str(driver));
        g.put("vendor", J::str(vendor));
        g.put("device", J::str(device));
        g.put("integrated", J::boolean(integrated));
        auto rn = card_render_node(cn);
        g.put("render_node", rn ? J::str(*rn) : J::nul());
        auto vt = (total && *total) ? std::optional<long long>(std::llround((double)*total / 1048576.0))
                                     : std::nullopt;
        g.put("vram_total_mib", vt ? J::integer(*vt) : J::nul());
        auto vu = (used && *used) ? std::optional<long long>(std::llround((double)*used / 1048576.0))
                                   : std::nullopt;
        g.put("vram_used_mib", vu ? J::integer(*vu) : J::nul());
        g.put("display_outputs", J::integer(online));
        g.put("connectors_total", J::integer(nconn));
        g.put("has_output", J::boolean(online > 0));
        g.put("link", std::move(link));
        gpus.push_back(std::move(g));
    }

    std::stable_sort(gpus.begin(), gpus.end(), [](const J &a, const J &b) {
        double ga = 0, gb = 0;
        const J *la = jget(a, "link"), *lb = jget(b, "link");
        if (la) { auto v = ffield(*la, "gbps_max"); if (v) ga = *v; }
        if (lb) { auto v = ffield(*lb, "gbps_max"); if (v) gb = *v; }
        return ga > gb;
    });

    J out = J::object();
    out.put("gpufan", J::str("probe/1.0.0"));
    J arr = J::array();
    for (auto &g : gpus) arr.push(std::move(g));
    out.put("gpus", std::move(arr));
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// scoring & roles
// ─────────────────────────────────────────────────────────────────────────────
static const J *gpus_arr(const J &db) { return jget(db, "gpus"); }

static double score(const J &g) {
    double gm = 0;
    const J *L = jget(g, "link");
    if (L) { auto v = ffield(*L, "gbps_max"); if (v) gm = *v; }
    double vram = (double)ifield(g, "vram_total_mib").value_or(0);
    double base = gm + vram / 4096.0;
    return bfield(g, "integrated") ? base - 5 : base;
}

static J assign_roles(const J &db, const string &profile) {
    (void)profile;
    const J *ga = gpus_arr(db);
    vector<const J *> g;
    if (ga) for (auto &x : ga->arr) g.push_back(&x);

    vector<const J *> ranked = g;
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const J *a, const J *b) { return score(*a) > score(*b); });

    std::map<string, vector<string>> used;
    std::map<string, vector<string>> reasons;

    auto add = [&](const J *c, const string &role, const string &why) {
        used[sfield(*c, "card")].push_back(role);
        reasons[sfield(*c, "card")].push_back(why);
    };
    auto has_role = [&](const J *c, const string &role) {
        auto it = used.find(sfield(*c, "card"));
        if (it == used.end()) return false;
        return std::find(it->second.begin(), it->second.end(), role) != it->second.end();
    };
    auto pick = [&](const string &role, const std::function<bool(const J &)> &pred,
                    const string &why, const vector<string> &exclude = {}) -> const J * {
        for (auto *c : ranked) {
            string cd = sfield(*c, "card");
            if (std::find(exclude.begin(), exclude.end(), cd) != exclude.end()) continue;
            if (has_role(c, role)) continue;
            if (pred(*c)) { add(c, role, why); return c; }
        }
        return nullptr;
    };

    // 1) render
    std::optional<const J *> render;
    {
        vector<const J *> monitor;
        for (auto *c : ranked) if (bfield(*c, "has_output")) monitor.push_back(c);
        if (!monitor.empty()) {
            const J *best = monitor[0];
            for (auto *c : monitor) if (score(*c) > score(*best)) best = c;
            render = best;
        } else if (!ranked.empty()) {
            render = ranked[0];
        }
    }
    string rd = render ? sfield(**render, "card") : string();

    if (render) {
        string why = bfield(**render, "has_output")
            ? "FEEDS THE MONITOR — the main GPU by rule (rendering anywhere else costs a per-frame cross-GPU present)"
            : "strongest card by score (NO monitor detected — headless rig)";
        add(*render, "render", why);
    }

    // 2) display
    std::optional<const J *> display = render;
    string dd = rd;
    if (display) {
        string why = bfield(**display, "has_output") ? "owns the connected panel"
                                                     : "assumed display (no panel detected)";
        add(*display, "display", why);
    }

    // 3) frame-gen + upscale
    {
        const J *fgc = nullptr;
        for (auto *c : ranked)
            if (bfield(*c, "has_output") && sfield(*c, "card") != dd) { fgc = c; break; }
        if (fgc) {
            add(fgc, "framegen", "second panel owner: interpolated frames are generated on the display GPU");
            add(fgc, "upscale", "same card spatial-upscales the presented frame");
        } else {
            const J *cand = nullptr;
            for (auto *c : ranked) {
                auto ct = ifield(*c, "connectors_total");
                if (ct && *ct && sfield(*c, "card") != dd) { cand = c; break; }
            }
            if (cand) {
                char b[192];
                snprintf(b, sizeof(b),
                         "SPARE framegen/upscale target — plug a display into one of its %d connectors to activate",
                         (int)ifield(*cand, "connectors_total").value_or(0));
                reasons[sfield(*cand, "card")].push_back(b);
            }
        }
    }

    // 4) encode
    pick("encode",
         [&](const J &c) { return sfield(c, "driver") == "amdgpu" && sfield(c, "card") != rd; },
         "VCN/VCE video encode (AMD has no concurrent-session limit) — records/streams off the render card");

    // 5) compute — largest VRAM off the render card
    {
        const J *comp = nullptr;
        for (auto *c : ranked) {
            if (sfield(*c, "card") == rd) continue;
            if (!comp) { comp = c; continue; }
            long long a = ifield(*c, "vram_total_mib").value_or(0);
            long long b = ifield(*comp, "vram_total_mib").value_or(0);
            if (a > b) comp = c;
        }
        if (comp)
            add(comp, "compute",
                "largest VRAM off the render card: process-level compute (agents, LLM, Blender)");
    }

    // 6) analysis — non-render card carrying the fewest roles so far
    {
        const J *ana = nullptr;
        for (auto *c : ranked) {
            if (sfield(*c, "card") == rd) continue;
            auto cnt = [&](const J *x) {
                auto it = used.find(sfield(*x, "card"));
                return it == used.end() ? (size_t)0 : it->second.size();
            };
            if (!ana) { ana = c; continue; }
            if (cnt(c) < cnt(ana)) ana = c;
        }
        if (ana)
            add(ana, "analysis",
                "batch lane: video analysis, proxies, preview bakes (deadline-free)");
    }

    // 7) desktop/compositor
    pick("desktop",
         [&](const J &c) { return bfield(c, "integrated") && bfield(c, "has_output"); },
         "drive the desktop/compositor so the gaming card keeps its VRAM for frames");

    // honest advice
    if (render && bfield(**render, "has_output")) {
        const J *stronger = nullptr;
        for (auto *c : ranked) {
            if (sfield(*c, "card") == rd) continue;
            if (bfield(*c, "has_output")) continue;
            if (score(*c) > score(**render)) { stronger = c; break; }
        }
        if (stronger) {
            string nm = sfield(*stronger, "name");
            if (nm.size() > 26) nm = nm.substr(0, 26);
            reasons[rd].push_back(
                "NOTE: " + nm +
                " scores higher but feeds no monitor — plug your display into it to make IT "
                "the main GPU (we keep the monitor card as main until you do)");
        }
    }

    // 8) compute-pool
    for (auto *c : ranked) {
        if (used.count(sfield(*c, "card"))) continue;
        long long vram = ifield(*c, "vram_total_mib").value_or(0);
        if (vram >= 2048)
            add(c, "compute-pool",
                "spare card: joins the system compute pool (agents, renders, transcodes) — "
                "no in-game framegraph role, but the box uses it");
        else
            reasons[sfield(*c, "card")].push_back(
                "too little VRAM for a useful role (integrated; shares system RAM). "
                "Plug the panel into the motherboard to make it the desktop card.");
    }

    // plan
    J plan = J::array();
    if (ga) {
        for (auto &c : ga->arr) {
            string cd = sfield(c, "card");
            J o = J::object();
            o.put("card", J::str(cd));
            o.put("name", J::str(sfield(c, "name")));
            o.put("bdf", J::str(sfield(c, "bdf")));
            const J *L = jget(c, "link");
            auto gm = L ? ffield(*L, "gbps_max") : std::nullopt;
            o.put("link_gbps_max", gm ? J::flt(*gm) : J::nul());
            auto vt = ifield(c, "vram_total_mib");
            o.put("vram_total_mib", vt ? J::integer(*vt) : J::nul());
            J roles = J::array();
            auto iu = used.find(cd);
            if (iu != used.end() && !iu->second.empty())
                for (auto &r : iu->second) roles.push(J::str(r));
            else
                roles.push(J::str("idle"));
            o.put("roles", std::move(roles));
            J why = J::array();
            auto ir = reasons.find(cd);
            if (ir != reasons.end() && !ir->second.empty())
                for (auto &w : ir->second) why.push(J::str(w));
            else
                why.push(J::str("idle — no role on this link beats doing nothing; not faked"));
            o.put("why", std::move(why));
            plan.push(std::move(o));
        }
    }
    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────
// printing
// ─────────────────────────────────────────────────────────────────────────────
static string gbps_or_dash(const J &c) {
    const J *L = jget(c, "link");
    if (L) { auto v = ffield(*L, "gbps_max"); if (v) return py_float_repr(*v); }
    return "-";
}

static void print_table(const J &db) {
    printf("spirit probe — the TRUE host link of each GPU (min negotiated width along the sysfs chain)\n\n");
    printf("%-6s %-13s %-30s %-9s %-9s %-8s %-6s %s\n", "card", "bdf", "name", "link", "GB/s est",
           "VRAM", "out", "note");
    printf("%s\n", string(118, '-').c_str());
    const J *ga = gpus_arr(db);
    if (ga) {
        for (auto &g : ga->arr) {
            const J *L = jget(g, "link");
            auto vt = ifield(g, "vram_total_mib");
            string vr = (vt && *vt) ? (std::to_string(*vt) + " MiB") : "-";
            string note;
            bool integ = bfield(g, "integrated");
            bool capped = L ? bfield(*L, "capped_by_topology") : false;
            auto xw = L ? ifield(*L, "max_width") : std::nullopt;
            auto ownw = L ? ifield(*L, "own_reports_width") : std::nullopt;
            if (integ) {
                note = "INTEGRATED — shares system RAM; 'link' is the CPU root port, not a card link";
            } else if (capped) {
                note = "card says x" + std::to_string(ownw.value_or(0)) + ", HOST gives x" +
                       std::to_string(xw.value_or(0));
            } else if ((xw.value_or(99)) < 4) {
                note = "NARROW — batch/offline card";
            }
            bool idle_deg = L ? bfield(*L, "idle_degraded") : false;
            if (idle_deg) {
                auto cw = L ? ifield(*L, "current_width") : std::nullopt;
                string cg = L ? s_or_none(*L, "current_gen") : "None";
                note += "  [idle now: x" + std::to_string(cw.value_or(0)) + " " + cg + "]";
            }
            string linkstr;
            if (xw) {
                string gn = L ? s_or_none(*L, "max_gen") : "None";
                linkstr = "x" + std::to_string(*xw) + " " + gn;
            } else linkstr = "-";
            string nm = utf8_cpslice(sfield(g, "name"), 30);
            printf("%-6s %-13s %s %-9s %-9s %-8s %-6s %s\n",
                   sfield(g, "card").c_str(), sfield(g, "bdf").c_str(), pad_display(nm, 30).c_str(),
                   linkstr.c_str(), gbps_or_dash(g).c_str(), vr.c_str(),
                   bfield(g, "has_output") ? "yes" : "no", note.c_str());
        }
    }
    printf("\nPlanning law: a secondary may take work only if it crosses the link ONCE or at\n");
    printf("FRAME rate toward the display. Per-frame PIXEL offload is dead on links < ~25 GB/s.\n");
}

static void print_roles(const J &db, const string &profile) {
    J roles = assign_roles(db, profile);
    int n = (int)roles.arr.size();
    int used_cards = 0;
    for (auto &r : roles.arr) {
        const J *rls = jget(r, "roles");
        bool is_idle = rls && rls->arr.size() == 1 && rls->arr[0].t == J::T::STR &&
                       rls->arr[0].s == "idle";
        if (!is_idle) used_cards++;
    }
    printf("spirit roles — profile: %s — %d GPU(s) detected, %d with a job\n\n", profile.c_str(), n,
           used_cards);
    printf("%-6s %-26s %-9s %-8s %s\n", "card", "name", "link", "VRAM", "role(s)");
    printf("%s\n", string(104, '-').c_str());
    for (auto &r : roles.arr) {
        string nm = utf8_cpslice(sfield(r, "name"), 26);
        auto vt = ifield(r, "vram_total_mib");
        string vr = (vt && *vt) ? (std::to_string(*vt) + "M") : "-";
        vector<string> rl;
        const J *rls = jget(r, "roles");
        if (rls) for (auto &x : rls->arr) rl.push_back(x.s);
        auto gm = ffield(r, "link_gbps_max");
        string linkcol = gm ? py_float_repr(*gm) : "-";
        printf("%-6s %s %-9s %-8s %s\n", sfield(r, "card").c_str(), pad_display(nm, 26).c_str(),
               linkcol.c_str(), vr.c_str(), join(rl, ", ").c_str());
        const J *why = jget(r, "why");
        if (why) for (auto &w : why->arr) printf("        └─ %s\n", w.s.c_str());
    }
    printf("\nEvery card is either working or explicitly idle-with-a-reason. That is the whole point:\n");
    printf("the Spiritual Successor uses ALL the silicon, not just the fastest card.\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// plan
// ─────────────────────────────────────────────────────────────────────────────
struct WL {
    bool has_res;
    long long w, h, bpp;
    long long fps;
    long long hidden, tps;
    bool live;
    string why;
};
static const std::map<string, WL> WORKLOADS = {
    {"1440p60-framegen", {true, 2560, 1440, 4, 60, 0, 0, false,
                          "frame-gen with the display on the secondary: base frames cross once, one way"}},
    {"4k60-framegen", {true, 3840, 2160, 4, 60, 0, 0, false,
                       "same at 4K; needs ~8 GB/s sustained"}},
    {"1440p-perframe-split", {true, 2560, 1440, 4, 60, 0, 0, true,
                              "AFR/SFR — needs the frame back EVERY frame: expect infeasible"}},
    {"llm-13b", {false, 0, 0, 2, 0, 4096, 40, false,
                 "process-level compute; only token/KV traffic crosses, ~KB/token"}},
};

static int plan_cmd(const J &db, const string &workload, std::optional<long long> vram_mib) {
    auto it = WORKLOADS.find(workload);
    if (it == WORKLOADS.end()) {
        printf("unknown workload. known: 1440p60-framegen, 4k60-framegen, 1440p-perframe-split, llm-13b\n");
        return 1;
    }
    const WL &w = it->second;
    long long S;
    double per_sec;
    if (w.has_res) { S = w.w * w.h * w.bpp; per_sec = (double)S * w.fps; }
    else { S = w.hidden * w.bpp; per_sec = (double)S * w.tps; }

    printf("plan — %s\n  %s\n\n", workload.c_str(), w.why.c_str());
    printf("  payload: %.2f MB %s\n", S / 1e6, w.has_res ? "per frame" : "per token");
    printf("  sustained one-way: %.2f GB/s\n\n", per_sec / 1e9);
    if (vram_mib && *vram_mib) {
        // ★ THESE ARE FIGURES WE ACTUALLY MEASURED (fix 2026-10-11, final review). The previous line
        // quoted ~800 MiB and ~8,200 MiB, and neither appears anywhere in this project's log. What the
        // log does have is below, each from a named run.
        printf("  VRAM asked: %d MiB — for scale, measured here: a 4K benchmark peaks at 1,514 MiB,\n",
               (int)*vram_mib);
        printf("  an 8K benchmark at 6,898 MiB, and a real open-world game at 10,692 MiB.\n\n");
    }
    printf("  %-8s %-11s %-10s %-9s %-8s %s\n", "card", "link GB/s", "capacity", "verdict", "VRAM",
           "arithmetic");
    printf("  %s\n", string(104, '-').c_str());

    const J *ga = gpus_arr(db);
    if (ga) for (auto &g : ga->arr) {
        const J *L = jget(g, "link");
        auto B = L ? ffield(*L, "gbps_max") : std::nullopt;
        if (!B || *B == 0) continue;
        double cap = *B * PLAN_HEADROOM;
        double load = per_sec / 1e9;
        string verdict, arith;
        char bb[256];
        if (w.live) {
            verdict = "INFEASIBLE";
            snprintf(bb, sizeof(bb),
                     "live: result must return every frame (%.1f%% of link at 100%%, latency on the critical path)",
                     load / *B * 100);
            arith = bb;
        } else if (load <= cap) {
            verdict = (load <= cap * 0.7) ? "ok" : "marginal";
            snprintf(bb, sizeof(bb), "%.1f%% of link (cap %.1f%%)", load / *B * 100,
                     PLAN_HEADROOM * 100);
            arith = bb;
        } else {
            verdict = "INFEASIBLE";
            snprintf(bb, sizeof(bb), "%.1f%% of link > %.1f%% cap", load / *B * 100,
                     PLAN_HEADROOM * 100);
            arith = bb;
        }
        if (!bfield(g, "has_output") && workload.find("framegen") != string::npos) {
            verdict = "INFEASIBLE";
            auto ct = ifield(g, "connectors_total");
            if (ct && *ct) {
                snprintf(bb, sizeof(bb),
                         "no panel attached — has %d connector(s); plug a display in to use as the frame-gen target",
                         (int)*ct);
                arith = bb;
            } else arith = "no display outputs at all — cannot be a frame-gen target";
        }
        auto vram = ifield(g, "vram_total_mib");
        string vcol = (vram && *vram) ? (std::to_string(*vram) + "M") : "-";
        if (vram_mib && *vram_mib && vram && *vram) {
            if (*vram_mib > *vram) {
                verdict = "INFEASIBLE";
                snprintf(bb, sizeof(bb), "needs %d MiB VRAM; this card has %d MiB", (int)*vram_mib,
                         (int)*vram);
                arith = bb;
            } else {
                vcol = std::to_string(*vram) + "M ok";
            }
        }
        char cb[64];
        snprintf(cb, sizeof(cb), "%.2f GB/s", cap);
        printf("  %-8s %-11s %-10s %-9s %-8s %s\n", sfield(g, "card").c_str(),
               py_float_repr(*B).c_str(), cb, verdict.c_str(), vcol.c_str(), arith.c_str());
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// fg
// ─────────────────────────────────────────────────────────────────────────────
static const std::map<string, double> FRAME_MIB = {{"1080p", 8.29}, {"1440p", 14.75}, {"4k", 33.18}};

static int fg_cmd(const J &db, const string &res_in, const string &mode) {
    // ★ PARSE THE RESOLUTION AND THE REFRESH AS TWO FIELDS (fix 2026-10-11, outside review). The old
    // parser split the last two characters off the end, so the two examples the help text itself
    // advertises were silently wrong: `4k120` became `4k1` at 20 Hz and fell back to 1440p (`1080p144`
    // likewise), and `1440p@0` divided by zero. Both of those examples are now correct, and a refresh
    // of zero falls back rather than crashing.
    string res;
    int hznum;
    std::regex rx("^\\s*(1080p|1440p|4k)\\s*@?\\s*(\\d+)?\\s*$", std::regex::icase);
    std::smatch m;
    if (std::regex_match(res_in, m, rx)) {
        res = m[1].str();
        for (char &c : res) c = (char)tolower((unsigned char)c);
        hznum = m[2].matched ? std::stoi(m[2].str()) : 60;
    } else {
        if (!res_in.empty())
            printf("  fg: could not read %s as <1080p|1440p|4k>[@][Hz]; using 1440p60.\n",
                   py_repr(res_in).c_str());
        else
            printf("  fg: no resolution given; using 1440p60.\n");
        res = "1440p";
        hznum = 60;
    }
    if (hznum <= 0) hznum = 60;
    double payload = FRAME_MIB.at(res);
    double budget = 1000.0 / hznum;
    bool one_way = (mode == "present-on-secondary");

    printf("spirit fg — can a card carry the frame-gen/upscale pass for %s@%d?\n\n", res.c_str(), hznum);
    printf("  frame payload: %.2f MiB    frame budget: %.2f ms\n\n", payload, budget);
    printf("  %-8s %-12s %-14s %-12s %s\n", "card", "link bound", "one-way ms", "round-trip ms", "verdict");
    printf("  %s\n", string(84, '-').c_str());

    const J *ga = gpus_arr(db);
    if (ga) for (auto &g : ga->arr) {
        string c = sfield(g, "card");
        // ★ THIS TOOL CANNOT KNOW YOUR MACHINE'S HANDOFF RATE (fix 2026-10-11, found by the final
        // outside read). This table used to be keyed by card NAME — card0/card2/card3 — and those names
        // are not portable: a stranger's card0 inherited another card's measured rate and was handed a
        // FEASIBLE/INFEASIBLE verdict about hardware we have never seen. The rate is a property of the
        // LINK and the driver, so what is computed here is an UPPER BOUND from the card's OWN measured
        // host link — and the gap between that bound and reality is stated below, not hidden.
        const J *L = jget(g, "link");
        auto link = L ? ffield(*L, "gbps_max") : std::nullopt;
        if (!link || *link == 0.0) {
            printf("  %-8s %-12s %-14s %-12s %s\n", c.c_str(), "n/a", "-", "-",
                   "no host link could be read for this card");
            continue;
        }
        double rate = *link;
        double ow = payload / 1024.0 / rate * 1000.0;
        double rt = ow * 2;
        string verdict, arith;
        char bb[128];
        if (one_way) {
            bool ok = ow <= budget;
            verdict = ok ? "FEASIBLE one-way" : "INFEASIBLE one-way";
            snprintf(bb, sizeof(bb), "%.1f ms vs %.1f ms budget", ow, budget);
        } else {
            bool ok = rt <= budget;
            verdict = ok ? "FEASIBLE round-trip" : "INFEASIBLE round-trip";
            snprintf(bb, sizeof(bb), "%.1f ms vs %.1f ms budget", rt, budget);
        }
        arith = bb;
        char hb[64];
        snprintf(hb, sizeof(hb), "<=%.2f GB/s", rate);
        printf("  %-8s %-12s %-14.2f %-12.2f %s (%s)\n", c.c_str(), hb, ow, rt, verdict.c_str(),
               arith.c_str());
    }
    printf("\n  ★ ARCHITECTURE RULE, from measurement: put the DISPLAY on the card doing the frame-gen, so a\n");
    printf("    frame crosses ONE WAY. A round trip doubles the cost and is the first thing to fail.\n");
    printf("  ★ THE VERDICT ABOVE IS AN UPPER BOUND, NOT A MEASUREMENT. It assumes a frame crosses at the\n");
    printf("    link's own rate. It does not: on the dev box the real handoff ran at roughly a tenth to a\n");
    printf("    third of the link rate, because RADV lands the import in a HOST-VISIBLE aperture heap. So a\n");
    printf("    card marked INFEASIBLE is definitely out, and a card marked FEASIBLE may still be out —\n");
    printf("    measure it on your own machine before trusting the last column.\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// boost: what extra frames can THIS machine give you, right now
// ─────────────────────────────────────────────────────────────────────────────
static const string LSFG_LAYER =
    "/usr/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json";
static const string LSFG_APPID = "993090";
static string lsfg_conf_path() { return home_dir() + "/.config/lsfg-vk/conf.toml"; }
static string steam_manifest_path() {
    return home_dir() + "/.local/share/Steam/steamapps/appmanifest_" + LSFG_APPID + ".acf";
}
static string lossless_dir() {
    return home_dir() + "/.local/share/Steam/steamapps/common/Lossless Scaling";
}

// ★ THE WITNESS — our own Vulkan layer, installed system-wide, INERT unless SPIRIT_PRESENT_COUNT
// names a file. It counts vkQueuePresentKHR calls from BELOW lsfg-vk, so it sees the images frame
// generation added. Nothing else can: the game's own counter counts frames IT rendered, an overlay
// sits above the layer, and the compositor's scan-out counter reports the display refresh.
static const string COUNTER_LAYER =
    "/usr/share/vulkan/implicit_layer.d/VkLayer_SPIRIT_present_counter.json";
static const string COUNTER_LIB = "/usr/local/lib/libspirit_present_counter.so";

// (state, detail) for the present counter. READY / PARTIAL / ABSENT — never a silent pass.
static std::pair<string, string> counter_state() {
    struct stat sb;
    if (stat(COUNTER_LAYER.c_str(), &sb) != 0)
        return {"ABSENT",
                "the present counter layer is not installed, so frame-gen output can only "
                "be ASSERTED, not measured"};
    if (stat(COUNTER_LIB.c_str(), &sb) != 0)
        return {"PARTIAL", "manifest is present but " + COUNTER_LIB + " is missing"};
    return {"READY", "present counter installed — frame-gen output is measurable"};
}

// The counter file is a fixed-width decimal the layer rewrites on every present.
static long long read_counter(const string &path) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return 0;
    string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    string raw = trim(data);
    if (raw.empty()) return 0;
    try {
        size_t idx = 0;
        long long v = std::stoll(raw, &idx, 10);
        if (idx != raw.size()) return 0;
        return v;
    } catch (...) { return 0; }
}

// tempfile.gettempdir(): TMPDIR/TEMP/TMP if it names an existing directory, else /tmp.
static string temp_dir() {
    for (const char *v : {"TMPDIR", "TEMP", "TMP"}) {
        const char *p = getenv(v);
        if (p && *p) {
            struct stat sb;
            if (stat(p, &sb) == 0 && S_ISDIR(sb.st_mode)) return p;
        }
    }
    return "/tmp";
}

static const J *main_card(const J &db); // defined below (play section)

// (state, detail). READY / ABSENT / PARTIAL / UNKNOWN — never a silent pass.
static std::pair<string, string> gamescope_state() {
    if (!which("gamescope")) return {"ABSENT", "gamescope is not installed"};
    ProcResult r = run_capture_both({"gamescope", "--help"}, nullptr, 15);
    if (!r.ok) return {"UNKNOWN", "gamescope --help failed (" + r.reason + ")"};
    // ★ gamescope prints its ENTIRE help to STDERR, not stdout. Read BOTH streams.
    string out = r.out + "\n" + r.err;
    for (char &c : out) c = (char)tolower((unsigned char)c);
    if (out.find("fsr") != string::npos)
        return {"READY", "gamescope present; FSR 1.0 upscaling available"};
    return {"PARTIAL", "gamescope present but no fsr filter is listed in --help"};
}

// (state, detail) for lsfg-vk — checked against the surfaces that OWN the answer.
// ★ NOT `lsfg-vk-cli validate`: that only validates its own config file, and reports a clean
// "valid" while the runtime cannot initialise at all. The manifest distinguishes a working
// install from a broken one.
static std::pair<string, string> lsfg_state() {
    size_t sl = LSFG_LAYER.find_last_of('/');
    string dir = LSFG_LAYER.substr(0, sl);
    string base = LSFG_LAYER.substr(sl + 1);
    vector<string> stood = glob_prefix(dir, base + ".disabled");
    std::sort(stood.begin(), stood.end());
    struct stat sb;
    bool layer_exists = (stat(LSFG_LAYER.c_str(), &sb) == 0);
    if (!stood.empty() && !layer_exists)
        return {"STOOD-DOWN", "installed but disabled at the loader; enable with: sudo mv " +
                                  stood.back() + " " + LSFG_LAYER};
    if (!layer_exists) return {"ABSENT", "lsfg-vk is not installed"};
    string ld = lossless_dir();
    if (stat(ld.c_str(), &sb) != 0)
        return {"NEEDS-GAME", "Lossless Scaling is not in the Steam library"};
    if (stat((ld + "/Lossless.dll").c_str(), &sb) != 0)
        return {"NEEDS-GAME", "no Lossless.dll in the Lossless Scaling install"};
    string man;
    if (FILE *f = fopen(steam_manifest_path().c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) man.append(buf, n);
        fclose(f);
    }
    // ★ the manifest spells it "BetaKey"; a lowercase literal never matches. Compare folded.
    string low = man;
    for (char &c : low) c = (char)tolower((unsigned char)c);
    if (low.find("betakey") == string::npos || low.find("\"lsfg-vk\"") == string::npos)
        return {"NEEDS-BRANCH",
                "Lossless Scaling is on Steam's DEFAULT branch; switch it to the \"lsfg-vk\" beta "
                "(Steam > Lossless Scaling > Properties > Betas). Without it the frame-gen shaders "
                "are absent and the layer fails with \"The specified shader DLL does not exist\""};
    if (!which("lsfg-vk-cli")) return {"PARTIAL", "layer present but lsfg-vk-cli is missing"};
    return {"INSTALLED", "layer + the lsfg-vk branch are present (path not exercised until a game runs)"};
}

// The preferred mode of a connected output on the monitor card, or nullopt. Rootless sysfs read.
static std::optional<std::pair<long long, long long>> native_res(const J &db) {
    const J *main = main_card(db);
    if (!main) return std::nullopt;
    string card = sfield(*main, "card");
    vector<string> paths = glob_prefix("/sys/class/drm", card + "-");
    // keep only "<entry>/modes" that exist
    vector<string> modes;
    for (auto &p : paths) {
        struct stat sb;
        string mp = p + "/modes";
        if (stat(mp.c_str(), &sb) == 0) modes.push_back(mp);
    }
    std::sort(modes.begin(), modes.end());
    std::optional<std::pair<long long, long long>> best;
    for (auto &path : modes) {
        FILE *f = fopen(path.c_str(), "r");
        if (!f) continue;
        string data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
        size_t nl = data.find('\n');
        string first = trim(nl == string::npos ? data : data.substr(0, nl));
        // re.match(r"^(\d+)x(\d+)$", first)
        size_t i = 0;
        while (i < first.size() && isdigit((unsigned char)first[i])) i++;
        if (i == 0) continue;
        if (i >= first.size() || first[i] != 'x') continue;
        size_t j = i + 1;
        while (j < first.size() && isdigit((unsigned char)first[j])) j++;
        if (j == i + 1) continue;
        if (j != first.size()) continue;
        long long w = std::stoll(first.substr(0, i));
        long long h = std::stoll(first.substr(i + 1, j - i - 1));
        if (w < 1024) continue;
        if (!best || w * h > best->first * best->second) best = std::make_pair(w, h);
    }
    return best;
}

// Render resolution for a scale factor, forced even. ROUND (ties-to-even, like Python round()),
// do not truncate: 1440 * 0.7 is 1007.9999999999999 in binary float.
static std::pair<long long, long long> render_res(const std::pair<long long, long long> &native,
                                                  double factor) {
    long long w = (long long)nearbyint((double)native.first * factor);
    long long h = (long long)nearbyint((double)native.second * factor);
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    return {w / 2 * 2, h / 2 * 2};
}

// Make sure a profile exists for this multiplier. Returns (profile_name_or_none, note).
static std::pair<std::optional<string>, string> ensure_lsfg_profile(int multiplier) {
    string name = "spirit " + std::to_string(multiplier) + "x";
    string conf = lsfg_conf_path();
    FILE *rf = fopen(conf.c_str(), "r");
    if (!rf)
        return {std::nullopt,
                "no lsfg-vk config at " + conf + " — create a profile in lsfg-vk-ui first"};
    string text;
    { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), rf)) > 0) text.append(buf, n); }
    fclose(rf);
    if (text.find("name = \"" + name + "\"") != string::npos)
        return {name, "profile " + py_repr(name) + " already present"};
    char mb[512];
    snprintf(mb, sizeof(mb),
             "\n[[profile]]\nactive_in = []\nflow_scale = 1.0\nmultiplier = %d\nname = \"%s\"\n"
             "override_present_mode = true\npacing_mode = \"vsync\"\nperformance_mode = false\n"
             "preserve_swapchain_image_count = false\n",
             multiplier, name.c_str());
    // ★ KEEP THE USER'S ORIGINAL, ONCE (fix 2026-10-11, from the outside review). This copied
    // unconditionally, so a SECOND multiplier overwrote the backup with the already-modified file —
    // and the user's original lsfg-vk config was then gone for good. A backup that is rewritten on
    // every use is not a backup; it is a copy of the last thing we did.
    string bak = conf + ".bak-spirit";
    // ★ NO RUNTIME ERROR TEXT (fix 2026-10-11, outside review). Same class as the compatdata and
    // work-outdir branches: each language spells its own error differently, so interpolating it
    // makes this a line the three builds can never agree on.
    string write_fail = "could not write the profile — check that " + conf + " is writable";
    struct stat bst;
    bool need_backup = (stat(bak.c_str(), &bst) != 0);   // shutil.copy2 only if not exists
    FILE *src = need_backup ? fopen(conf.c_str(), "rb") : nullptr;
    FILE *dst = src ? fopen(bak.c_str(), "wb") : nullptr;
    if (need_backup && (!src || !dst)) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        return {std::nullopt, write_fail};
    }
    if (src && dst) {
        char buf[8192];
        size_t n;
        bool okc = true;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
            if (fwrite(buf, 1, n, dst) != n) { okc = false; break; }
        fclose(src); fclose(dst);
        if (!okc)
            return {std::nullopt, write_fail};
    }
    FILE *af = fopen(conf.c_str(), "a");
    if (af) {
        size_t len = strlen(mb);
        bool wok = (fwrite(mb, 1, len, af) == len);
        fclose(af);
        if (wok)
            return {name, "added profile " + py_repr(name) + " (backup: " + bak + ")"};
    }
    return {std::nullopt, write_fail};
}

static int boost_cmd(const J &db) {
    auto gs = gamescope_state();
    auto ls = lsfg_state();
    auto native = native_res(db);
    const string &gs_state = gs.first, &gs_detail = gs.second;
    const string &ls_state = ls.first, &ls_detail = ls.second;

    printf("spirit boost — what extra frames this machine can give you, right now\n\n");
    printf("  UPSCALING — render below native and scale up. Fewer pixels rendered = real FPS.\n");
    printf("    gamescope : %-11s %s\n", gs_state.c_str(), gs_detail.c_str());
    if (native) {
        printf("    display   : %lldx%lld native, on the monitor card\n", native->first,
               native->second);
        double fr[4] = {0.9, 0.8, 0.7, 0.6};
        for (double f : fr) {
            auto r = render_res(*native, f);
            printf("      --scale %.1f  ->  render %lldx%lld  (%.0f%% of the pixels)\n", f,
                   r.first, r.second, 100.0 * f * f);
        }
    } else {
        printf("    display   : no connected output's preferred mode could be read\n");
    }
    printf("\n");
    printf("  FRAME GENERATION — interpolated frames. The big multiplier, and the one that needs\n");
    printf("  software we do not ship: lsfg-vk, which requires owning Lossless Scaling.\n");
    printf("    lsfg-vk   : %-11s %s\n", ls_state.c_str(), ls_detail.c_str());
    auto cs = counter_state();
    printf("    witness   : %-11s %s\n", cs.first.c_str(), cs.second.c_str());
    printf("                (our own present counter, below the frame-gen layer — the only\n");
    printf("                 instrument that can see an INSERTED frame; without it a frame-gen\n");
    printf("                 claim is an assertion, not a measurement)\n");
    printf("\n");
    if (gs_state == "READY")
        printf("  ⇒ Upscaling is available NOW:   spirit play --scale 0.7 -- <your game>\n");
    else
        printf("  ⇒ Upscaling is not ready: %s\n", gs_detail.c_str());
    if (ls_state == "INSTALLED")
        printf("  ⇒ Frame generation is available: spirit play --framegen 2 -- <your game>\n");
    else
        printf("  ⇒ Frame generation is not ready: %s\n", ls_detail.c_str());
    printf("\n  ★ Neither one is a miracle, and this tool will not pretend otherwise. Upscaling adds\n");
    printf("    frames by rendering fewer pixels. Frame generation adds frames by inventing them,\n");
    printf("    and it can look wrong on fast motion — that is the technology, not this tool.\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// doctor
// ─────────────────────────────────────────────────────────────────────────────
// What a child sees with the exclusive device-select applied to ONE card (.
struct IsoResult {
    int rc = 0;
    int count = 0;
    vector<std::pair<long long, long long>> ids;   // [(vendor, device), ...]
};

static std::optional<IsoResult> pinned_isolation(const string &vid, const string &did) {
    if (!which("vulkaninfo")) return std::nullopt;
    vector<string> env = current_env();
    set_env(env, "MESA_VK_DEVICE_SELECT", vid + ":" + did + "!");
    ProcResult r = run_capture_both({"vulkaninfo", "--summary"}, &env, 30);
    if (!r.ok) return std::nullopt;
    IsoResult res;
    res.rc = r.rc;
    const string &out = r.out;
    size_t pos = 0;
    while ((pos = out.find("deviceName", pos)) != string::npos) { res.count++; pos += 10; }
    vector<long long> vids, dids;
    std::regex rv("vendorID\\s*=\\s*(0x[0-9a-fA-F]+)");
    std::regex rd("deviceID\\s*=\\s*(0x[0-9a-fA-F]+)");
    for (std::sregex_iterator it(out.begin(), out.end(), rv), e; it != e; ++it)
        vids.push_back(strtoll((*it)[1].str().c_str(), nullptr, 16));
    for (std::sregex_iterator it(out.begin(), out.end(), rd), e; it != e; ++it)
        dids.push_back(strtoll((*it)[1].str().c_str(), nullptr, 16));
    size_t n = vids.size() < dids.size() ? vids.size() : dids.size();
    for (size_t i = 0; i < n; i++) res.ids.push_back({vids[i], dids[i]});
    return res;
}

// (vendor, device) as ints from a probe record, or nullopt if it cannot be parsed.
static std::optional<std::pair<long long, long long>> card_ids(const J &g) {
    string v = sfield(g, "vendor"), d = sfield(g, "device");
    if (v.empty() || d.empty()) return std::nullopt;
    char *ev = nullptr, *ed = nullptr;
    long long vi = strtoll(v.c_str(), &ev, 16);
    long long di = strtoll(d.c_str(), &ed, 16);
    if (ev == v.c_str() || ed == d.c_str()) return std::nullopt;
    return std::make_pair(vi, di);
}

// ★ READ INDEPENDENTLY OF THE ROLE ENGINE (helper for doctor's display-card check). It reads sysfs
// itself so the check can DISAGREE with what the role engine decided, or it is a mirror rather than a
// check.
static string base_of(const string &path); // defined below (play section)
static vector<string> connector_names(const string &card) {
    vector<string> names;
    for (const string &entry : glob_prefix("/sys/class/drm", card + "-")) {
        string base = base_of(entry);
        string name = base.size() > card.size() ? base.substr(card.size() + 1) : string();
        if (!name.empty() && read_trim(entry + "/status")) names.push_back(name);
    }
    return names;
}

// ★ READ `enabled`, NOT `status` (. `status` says what is PLUGGED IN — the same predicate
// the role engine used to pick the main card, so it could never disagree. `enabled` says what is
// DRIVING a display, which can: a second card with an enabled output is a cross-GPU present.
static vector<string> enabled_connectors(const string &card) {
    vector<string> out;
    for (const string &name : connector_names(card)) {
        auto e = read_trim("/sys/class/drm/" + card + "-" + name + "/enabled");
        if (e && *e == "enabled") out.push_back(name);
    }
    return out;
}

static string py_str_int(std::optional<long long> v) {
    return v ? std::to_string(*v) : string("None");
}

static int doctor(const J &db, bool as_json) {
    // ★ FOUR VERDICTS, NOT THREE (redesigned 2026-10-11 from outside design review). The old rule was
    // "any non-PASS is suspicious", which made a working mixed-vendor machine report PROBLEMS FOUND:
    // the telemetry check read a sysfs file only one vendor exposes, and an integrated GPU has no such
    // file to read. The states are now defined by what the tool actually depends on:
    //
    //     PASS     observed directly, by something that could have come out the other way
    //     FAIL     observed to be wrong, or something the tool relies on is broken
    //     N/A      this check does not apply to this card by design, and nothing depends on it
    //     UNKNOWN  the tool does depend on it, and doctor could not determine it
    //
    // Any FAIL -> PROBLEMS FOUND (exit 1). No FAIL but a CORE UNKNOWN (the device pin, or the main
    // card's link) -> NOT VERIFIED (exit 3). Only non-core UNKNOWNs left -> WORKS, NOT FULLY VERIFIED
    // (exit 3). Only PASS and N/A -> ALL CHECKS PASSED (exit 0), with the N/A count printed so nobody
    // reads N/A as a pass. A bare green verdict is never printed over an UNKNOWN.
    //
    // ★ EXIT 3, NOT 2 (. argparse usage errors and the SPIRIT_DEVICES refusals both exit
    // 2, so 2 could not distinguish "not fully verified" from "you typed it wrong".
    vector<J> checks;
    const J *ga = gpus_arr(db);
    const J *main = main_card(db);          // nullptr when this machine has no GPUs at all (1a.15)

    if (!ga || ga->arr.empty()) {
        // ★ NO CARDS IS NOT A CRASH (. With nothing to test, the honest verdict is a FAIL.
        J c = J::object();
        c.put("check", J::str("gpus"));
        c.put("verdict", J::str("FAIL"));
        c.put("core", J::boolean(true));
        c.put("detail", J::str("no GPUs were found on this machine — there is nothing to render on "
                               "and nothing to test"));
        checks.push_back(std::move(c));
    } else {
        // A. LINKS. The MAIN card's link is CORE (1d.22): the tool cannot vouch for the launch if it
        //    cannot even read the card it will render on.
        for (auto &g : ga->arr) {
            const J *L = jget(g, "link");
            auto gb = L ? ffield(*L, "gbps_max") : std::nullopt;
            bool core = main && sfield(g, "card") == sfield(*main, "card");
            J c = J::object();
            c.put("check", J::str("link"));
            c.put("card", J::str(sfield(g, "card")));
            if (gb) {
                auto mw = L ? ifield(*L, "max_width") : std::nullopt;
                string mg = L ? s_or_none(*L, "max_gen") : "None";
                c.put("verdict", J::str("PASS"));
                c.put("core", J::boolean(core));
                c.put("detail", J::str("x" + py_str_int(mw) + " " + mg +
                                       " — the minimum negotiated width along the chain"));
            } else if (bfield(g, "integrated")) {
                c.put("verdict", J::str("N/A"));
                c.put("core", J::boolean(core));
                c.put("detail", J::str("integrated; there is no card link to measure"));
            } else {
                c.put("verdict", J::str("UNKNOWN"));
                c.put("core", J::boolean(core));
                c.put("detail", J::str("no PCIe link attributes were readable along the chain"));
            }
            checks.push_back(std::move(c));
        }

        // B. ROUTING ISOLATION. The load-bearing pin, so CORE, and it MATCHES THE PINNED ID (1a.2,
        //    1a.10, 1b.18): a count alone proves the pin CHANGED something, not that it selected the
        //    right card. A nonzero rc (a loader failure), a count of 0 (the pin matched nothing) and a
        //    count above 1 (still sees more than one) are kept as SEPARATE states.
        if (!which("vulkaninfo")) {
            J c = J::object();
            c.put("check", J::str("routing-isolation"));
            c.put("verdict", J::str("UNKNOWN"));
            c.put("core", J::boolean(true));
            c.put("detail", J::str("vulkaninfo (vulkan-tools) is not installed — the device pin, "
                                   "which the whole launch depends on, cannot be tested at all"));
            checks.push_back(std::move(c));
        } else {
            vector<string> cards;
            vector<std::optional<IsoResult>> res;
            for (auto &g : ga->arr) {
                string vendor = sfield(g, "vendor"), device = sfield(g, "device");
                string vid = vendor.size() > 2 ? vendor.substr(2) : string();
                string did = device.size() > 2 ? device.substr(2) : string();
                cards.push_back(sfield(g, "card"));
                res.push_back(pinned_isolation(vid, did));
            }
            vector<string> untestable, rcbad, zero, multi;
            for (size_t i = 0; i < res.size(); i++) {
                if (!res[i]) { untestable.push_back(cards[i]); continue; }
                if (res[i]->rc != 0) { rcbad.push_back(cards[i]); continue; }
                if (res[i]->count == 0) { zero.push_back(cards[i]); continue; }
                if (res[i]->count > 1) { multi.push_back(cards[i]); continue; }
            }
            vector<string> mism;
            for (size_t i = 0; i < ga->arr.size(); i++) {
                const J &g = ga->arr[i];
                const std::optional<IsoResult> &v = res[i];
                if (v && v->rc == 0 && v->count == 1) {
                    auto want = card_ids(g);
                    bool match = want && v->ids.size() == 1 &&
                                 v->ids[0].first == want->first && v->ids[0].second == want->second;
                    if (!match) mism.push_back(sfield(g, "card"));
                }
            }
            J c = J::object();
            c.put("check", J::str("routing-isolation"));
            if (!untestable.empty()) {
                c.put("verdict", J::str("UNKNOWN"));
                c.put("core", J::boolean(true));
                c.put("detail", J::str("a test child could not be run for: " + join(untestable, ", ")));
            } else if (!rcbad.empty()) {
                c.put("verdict", J::str("UNKNOWN"));
                c.put("core", J::boolean(true));
                c.put("detail", J::str("vulkaninfo returned nonzero (a loader failure, not 'sees more "
                                       "than one device') for: " + join(rcbad, ", ")));
            } else if (!zero.empty()) {
                c.put("verdict", J::str("FAIL"));
                c.put("core", J::boolean(true));
                c.put("detail", J::str("the pin matched nothing — a child saw 0 devices for: "
                                       + join(zero, ", ")));
            } else if (!multi.empty()) {
                c.put("verdict", J::str("FAIL"));
                c.put("core", J::boolean(true));
                c.put("detail", J::str("a child still sees more than one device for: " + join(multi, ", ")));
            } else if (!mism.empty()) {
                c.put("verdict", J::str("FAIL"));
                c.put("core", J::boolean(true));
                c.put("detail", J::str("a child saw exactly 1 device, but it is not the card pinned for: "
                                       + join(mism, ", ")));
            } else if (ga->arr.size() == 1) {
                c.put("verdict", J::str("UNKNOWN"));
                c.put("core", J::boolean(true));
                c.put("detail", J::str("only one GPU is present, so a child sees 1 device with or "
                                       "without the pin — the count cannot show the pin selected anything"));
            } else {
                c.put("verdict", J::str("PASS"));
                c.put("core", J::boolean(true));
                c.put("detail", J::str("a child saw exactly 1 device AND its vendor:device id matches the "
                                       "card pinned — per card. (A count alone would prove only that the pin "
                                       "CHANGED something, not which card; the id match is what ties the "
                                       "surviving device to the pinned one.)"));
            }
            checks.push_back(std::move(c));
        }

        // B2. IDENTICAL CARDS. Not a gap in doctor — a design defect worth failing loudly, because the
        //     pin genuinely cannot separate them and the user would never guess.
        {
            vector<std::pair<std::pair<string, string>, vector<string>>> seen;
            for (auto &g : ga->arr) {
                std::pair<string, string> key{sfield(g, "vendor"), sfield(g, "device")};
                bool found = false;
                for (auto &kv : seen)
                    if (kv.first == key) { kv.second.push_back(sfield(g, "card")); found = true; break; }
                if (!found) seen.push_back({key, {sfield(g, "card")}});
            }
            J c = J::object();
            c.put("check", J::str("individual-pin"));
            bool dup = false;
            for (auto &kv : seen) {
                if (kv.second.size() > 1) {
                    dup = true;
                    string v = kv.first.first, d = kv.first.second;
                    c.put("verdict", J::str("FAIL"));
                    c.put("detail", J::str(join(kv.second, ", ") + " share " +
                                           (v.size() > 2 ? v.substr(2) : string()) + ":" +
                                           (d.size() > 2 ? d.substr(2) : string()) +
                                           " — the pin selects the model, not the card, so it cannot "
                                           "tell them apart"));
                    break;
                }
            }
            if (!dup) {
                c.put("verdict", J::str("PASS"));
                c.put("detail", J::str("every card has its own vendor:device id, so a pin can name one"));
            }
            checks.push_back(std::move(c));
        }

        // C. TELEMETRY. ★ NOTHING IN THIS TOOL READS A CARD'S LIVE LOAD ( blocking item 4).
        //    By the four verdicts' own definition that makes it N/A for EVERY kind of card — a check on
        //    a dependency that does not exist. Kept as N/A, per card, so it is printed and counted.
        for (auto &g : ga->arr) {
            J c = J::object();
            c.put("check", J::str("telemetry"));
            c.put("card", J::str(sfield(g, "card")));
            c.put("verdict", J::str("N/A"));
            c.put("detail", J::str("this tool reads a card's TOTAL VRAM (probe) but never its live load, "
                                   "so no memory counter is required here"));
            checks.push_back(std::move(c));
        }

        // D. DISPLAY CARD. ★ READS `enabled`, NOT `status` ( a blocking finding). A second
        //    card with an enabled output is a cross-GPU present, the real risk. Read independently of
        //    the engine, as before.
        {
            vector<std::pair<string, vector<string>>> enabled_cards;
            for (auto &g : ga->arr) {
                string card = sfield(g, "card");
                vector<string> en = enabled_connectors(card);
                if (!en.empty()) enabled_cards.push_back({card, en});
            }
            J c = J::object();
            c.put("check", J::str("display-card"));
            if (enabled_cards.empty()) {
                c.put("verdict", J::str("N/A"));
                c.put("detail", J::str("no connector has an active CRTC — no display is being driven"));
            } else {
                vector<string> names;
                for (auto &kv : enabled_cards) names.push_back(kv.first);
                std::sort(names.begin(), names.end());
                bool main_lit = false;
                for (auto &kv : enabled_cards)
                    if (main && kv.first == sfield(*main, "card")) main_lit = true;
                if (!main_lit) {
                    c.put("verdict", J::str("FAIL"));
                    c.put("detail", J::str("rendering was assigned to " +
                                           (main ? sfield(*main, "card") : string("?")) +
                                           ", but " + join(names, ", ") + " is driving a display"));
                } else if (enabled_cards.size() > 1) {
                    vector<string> others;
                    for (auto &kv : enabled_cards)
                        if (!main || kv.first != sfield(*main, "card")) others.push_back(kv.first);
                    std::sort(others.begin(), others.end());
                    c.put("verdict", J::str("FAIL"));
                    c.put("detail", J::str("more than one card is driving a display (" + join(names, ", ") +
                                           "): the game window can land on " + join(others, ", ") +
                                           "'s monitor, a cross-GPU present"));
                } else {
                    c.put("verdict", J::str("PASS"));
                    c.put("detail", J::str("the card picked to render (" +
                                           (main ? sfield(*main, "card") : string("?")) +
                                           ") is the one driving a display"));
                }
            }
            checks.push_back(std::move(c));
        }

        // E. THE SECOND WALL. Now doctor says what it found, which is the least it can do.
        {
            std::optional<bool> wall;
            if (which("unshare")) {
                vector<string> argv = {"unshare", "-Urm", "true"};
                ProcResult r = run_capture_both(argv, nullptr, 10);
                if (r.ok) wall = (r.rc == 0);
            }
            J c = J::object();
            c.put("check", J::str("namespace-wall"));
            if (wall && *wall) {
                c.put("verdict", J::str("PASS"));
                c.put("detail", J::str("unprivileged user namespaces are available; the game can be "
                                       "separated from the other cards' device nodes"));
            } else if (wall && !*wall) {
                c.put("verdict", J::str("UNKNOWN"));
                c.put("detail", J::str("unprivileged user namespaces are NOT available here, so only the pin "
                                       "separates the game from the other cards"));
            } else {
                c.put("verdict", J::str("UNKNOWN"));
                c.put("detail", J::str("unshare is not installed, so only the pin separates the game from the "
                                       "other cards"));
            }
            checks.push_back(std::move(c));
        }

        // F. UPSCALING — a path that needs OPTIONAL software. ABSENT (and present but without FSR) is
        //    DETERMINED, and nothing depends on it unless the user asks for --scale, so N/A, not UNKNOWN
        //    ( blocking item 3).
        {
            auto gs = gamescope_state();
            J c = J::object();
            c.put("check", J::str("upscaling"));
            string gv = "UNKNOWN";
            if (gs.first == "READY") gv = "PASS";
            else if (gs.first == "ABSENT" || gs.first == "PARTIAL") gv = "N/A";
            c.put("verdict", J::str(gv));
            c.put("detail", J::str(gs.first + " — " + gs.second));
            checks.push_back(std::move(c));
        }

        // G. FRAME GENERATION — same rule (blocking item 3): ABSENT / disabled / game-not-in-library
        //    are DETERMINED and optional, so N/A. Only a broken install (the shaders absent) is a FAIL.
        //    PARTIAL now AGREES with `play` (blocking item 5, ): the missing piece is
        //    `lsfg-vk-cli`, which this tool never calls, so both report it PASS.
        {
            auto ls = lsfg_state();
            string lv = "UNKNOWN";
            if (ls.first == "INSTALLED") lv = "PASS";
            else if (ls.first == "PARTIAL") lv = "PASS";
            else if (ls.first == "NEEDS-BRANCH") lv = "FAIL";
            else if (ls.first == "STOOD-DOWN" || ls.first == "ABSENT" || ls.first == "NEEDS-GAME") lv = "N/A";
            J c = J::object();
            c.put("check", J::str("frame-generation"));
            c.put("verdict", J::str(lv));
            c.put("detail", J::str(ls.first + " — " + ls.second));
            checks.push_back(std::move(c));
        }
    }

    // ── the verdict rules, stated once ──────────────────────────────────────────────────────────────
    int nf = 0, nu = 0, ncu = 0, nna = 0;
    for (auto &c : checks) {
        string v = sfield(c, "verdict");
        if (v == "FAIL") nf++;
        else if (v == "UNKNOWN") { nu++; if (bfield(c, "core")) ncu++; }
        else if (v == "N/A") nna++;
    }
    string overall;
    int code;
    if (nf) { overall = "PROBLEMS FOUND"; code = 1; }
    else if (ncu) {
        char b[64]; snprintf(b, sizeof(b), "%d", ncu);
        overall = string("NOT VERIFIED — a core check could not be determined (") + b + ")";
        code = 3;
    }
    else if (nu) {
        overall = "WORKS, NOT FULLY VERIFIED (" + std::to_string(nu) + " item" + (nu == 1 ? "" : "s") + ")";
        code = 3;
    }
    else { overall = "ALL CHECKS PASSED"; code = 0; }

    if (as_json) {
        J o = J::object();
        o.put("spirit", J::str("doctor/1.0.0"));
        o.put("overall", J::str(overall));
        o.put("fails", J::integer(nf));
        o.put("unknown", J::integer(nu));
        o.put("core_unknown", J::integer(ncu));
        o.put("not_applicable", J::integer(nna));
        J arr = J::array();
        for (auto &c : checks) arr.push(c);
        o.put("checks", std::move(arr));
        printf("%s\n", json_dumps_indent2(o).c_str());
        return code;
    }

    printf("spirit doctor — verifying this machine's multi-GPU readiness\n\n");
    for (auto &c : checks) {
        const J *cp = jget(c, "card");
        string card;
        if (cp && cp->t == J::T::STR) {
            char b[64];
            snprintf(b, sizeof(b), "%-6s ", cp->s.c_str());
            card = b;
        }
        printf("  [%-7s] %s%s\n", sfield(c, "verdict").c_str(), card.c_str(),
               sfield(c, "detail").c_str());
    }
    printf("\n  OVERALL: %s\n", overall.c_str());
    if (nna)
        printf("  (%d check%s did not apply here and %s not counted as passes.)\n",
               nna, nna == 1 ? "" : "s", nna == 1 ? "was" : "were");
    if (nu) {
        printf("  An item above could not be determined. That is not a failure, and it is not a pass —\n");
        printf("  it means the tool cannot vouch for that part on this machine.\n");
        if (ncu) {
            printf("  ★ %d of them %s CORE (the device pin, or the main card's link): without those, this\n",
                   ncu, ncu == 1 ? "is" : "are");
            printf("    build does not claim the launch works — hence NOT VERIFIED above, not WORKS.\n");
        }
    }
    if (nf) printf("  See the FAIL lines above.\n");
    return code;
}

// ─────────────────────────────────────────────────────────────────────────────
// run
// ─────────────────────────────────────────────────────────────────────────────
static int run_cmd(const J &db, const string &role, const string &card, const vector<string> &cmd) {
    const J *g = nullptr;
    const J *ga = gpus_arr(db);
    if (ga) for (auto &x : ga->arr)
        if (sfield(x, "card") == card || sfield(x, "bdf") == card) { g = &x; break; }
    if (!g) { fprintf(stderr, "no such card: %s\n", card.c_str()); return 2; }

    vector<string> env = current_env();
    string bdf = sfield(*g, "bdf");
    string pci = dri_prime(bdf);
    set_env(env, "DRI_PRIME", pci);
    string vendor = sfield(*g, "vendor"), device = sfield(*g, "device");
    string mvs = (vendor.size() > 2 ? vendor.substr(2) : string()) + ":" +
                 (device.size() > 2 ? device.substr(2) : string()) + "!";
    set_env(env, "MESA_VK_DEVICE_SELECT", mvs);
    if (role == "compute") set_env(env, "GGML_VK_VISIBLE_DEVICES", bdf);

    const J *L = jget(*g, "link");
    auto gm = L ? ffield(*L, "gbps_max") : std::nullopt;
    string nm = utf8_cpslice(sfield(*g, "name"), 34);
    printf("# routing role=%s to %s (%s, link %s GB/s)\n", role.c_str(), sfield(*g, "card").c_str(),
           nm.c_str(), gm ? py_float_repr(*gm).c_str() : "None");
    printf("# env: DRI_PRIME=%s  MESA_VK_DEVICE_SELECT=%s\n", pci.c_str(), mvs.c_str());

    if (role == "encode") {
        string rn = sfield(*g, "render_node");
        if (!rn.empty()) {
            set_env(env, "SPIRIT_VAAPI_DEVICE", "/dev/dri/" + rn);
            printf("#      add to ffmpeg:  -vaapi_device /dev/dri/%s\n", rn.c_str());
        }
    }
    if (role == "compute")
        printf("#      ggml: GGML_VK_VISIBLE_DEVICES=%s\n", bdf.c_str());
    fflush(stdout);

    if (cmd.empty()) return 0;
    return spawn_wait(cmd, env, false);
}

// ─────────────────────────────────────────────────────────────────────────────
// play
// ─────────────────────────────────────────────────────────────────────────────
static const J *main_card(const J &db) {
    const J *ga = gpus_arr(db);
    if (!ga || ga->arr.empty()) return nullptr;
    // ★ THE CARD ACTUALLY DRIVING A DISPLAY OUTRANKS A CARD MERELY PLUGGED IN (fix 2026-10-12, from the
    // outside read). `has_output` is built from a connector reading `connected`, and a cable in a port
    // that is not driving (DPMS off, a KVM, a second seat) reads `connected` too. doctor judges the
    // display card by the ACTIVE CRTC; play used `has_output` + score — two definitions of the single
    // most important decision this tool makes. Order: active CRTC, then connected, then top scorer.
    // (A fixture has no sysfs, so enabled_connectors is empty and this falls through as before.)
    vector<const J *> driving;
    for (auto &g : ga->arr)
        if (!enabled_connectors(sfield(g, "card")).empty()) driving.push_back(&g);
    if (!driving.empty()) {
        const J *best = driving[0];
        for (auto *c : driving) if (score(*c) > score(*best)) best = c;
        return best;
    }
    vector<const J *> pool;
    for (auto &g : ga->arr) if (bfield(g, "has_output")) pool.push_back(&g);
    if (pool.empty()) for (auto &g : ga->arr) pool.push_back(&g);
    const J *best = pool[0];
    for (auto *c : pool) if (score(*c) > score(*best)) best = c;
    return best;
}

// ── ORIGIN — the mark. Every run prints it; break it and the tool says so, loudly, forever.
// Hiding a trace in the code is an old tradition, and it works here because this is not a comment —
// it is a CHECK. A comment gets deleted by accident; a check that reads its own notice can only be
// removed ON PURPOSE. The honest limit, stated so nobody thinks it is stronger than it is: a
// determined person can rip the whole check out. What it guarantees is that taking the credit is a
// decision, not an oversight.
static const char *ORIGIN_NOTICE = "Spiritual Successor by Sophia Marie DeClue — github.com/Sophia-Thickums/spiritual-successor";
static const char *ORIGIN_SEAL = "74c06104c43a4c80";
static const char *ORIGIN_TRUE = "Spiritual Successor by Sophia Marie DeClue — github.com/Sophia-Thickums/spiritual-successor";

static string origin_seal() {
    // FNV-1a 64 — ten lines, no dependency, identical bytes to the Python and Rust builds.
    unsigned long long h = 0xcbf29ce484222325ULL;
    const unsigned char *p = (const unsigned char *)ORIGIN_NOTICE;
    for (size_t i = 0; i < strlen(ORIGIN_NOTICE); i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    char b[32]; snprintf(b, sizeof(b), "%016llx", h);
    return string(b);
}

static int origin_cmd(bool check_only) {
    string actual = origin_seal();
    bool intact = (actual == ORIGIN_SEAL);
    if (!check_only) {
        printf("  %s\n", ORIGIN_NOTICE);
        printf("  mark: %s\n", actual.substr(0, 16).c_str());
    }
    if (check_only) return intact ? 0 : 1;
    if (!intact) {
        // NOT SILENCEABLE: prints on stdout, on every run, in the tool's own voice.
        printf("\n  ★★ THIS IS A MODIFIED COPY. The origin mark does not verify.\n");
        printf("     This is a copy of, or derived from: %s\n", ORIGIN_TRUE);
        printf("     Expected mark %s, found %s.\n", string(ORIGIN_SEAL).substr(0, 16).c_str(),
               actual.substr(0, 16).c_str());
        // ★ THIS LINE USED TO OVERSTATE THE LICENCE (corrected 2026-10-11, outside review). It said
        // removing the notice was "the one thing the licence forbids". MIT forbids nothing of the sort:
        // it permits modification freely, and requires only that the copyright notice travel with copies.
        printf("     This banner is not the licence. MIT lets anyone modify this freely — it asks that\n");
        printf("     the copyright notice travel with copies, not that this line be kept.\n\n");
    }
    return 0;
}

// ★ THE ENCODER DRIVER IS NOT ALWAYS radeonsi (fix 2026-10-11, found by the outside review).
// `LIBVA_DRIVER_NAME=radeonsi` was hard-coded at both launch sites, so a spare card that is not AMD
// was handed AMD's VAAPI driver. With a test load it failed SILENTLY, because ffmpeg's own output goes
// to /dev/null. The driver is now chosen from the card's OWN kernel driver, and a card with no VAAPI
// encoder we know of is SKIPPED AND SAID OUT LOUD rather than quietly doing nothing.
// Returns (driver, reason): (driver, "") if this card can encode, ("", why not) otherwise.
static std::pair<string, string> va_driver(const J &g) {
    string d = sfield(g, "driver");
    for (char &c : d) c = (char)tolower((unsigned char)c);
    if (d == "amdgpu") return {"radeonsi", ""};
    if (d == "i915")   return {"iHD", ""};
    if (d == "xe")     return {"iHD", ""};
    return {"", "no VAAPI encoder this tool knows of (kernel driver: " + (d.empty() ? string("unknown") : d) + ")"};
}

// Why this card's link is marginal, or nullopt if it is fine.
// ★ A REAL JOB DOES NOT REACH A MARGINAL CARD UNLESS THE USER NAMES IT (from outside design review,
// 2026-10-11). A benchmark that enumerated every display device reached a card sitting on an external
// dock, the card latched its own thermal alarm, and the machine went down. A user typing `spirit play
// --work film.mp4 -- game` did not ask for that. A card on a narrow link is usable — it is just not a
// default we assume on their behalf. `--spare <node>` includes it.
//
// ★ `capped_by_topology` IS NOT A PROXY FOR "BEHIND AN EXTERNAL BRIDGE". It fires whenever the card's
// OWN bridge reports wider than the host gives — the NORMAL case for every modern AMD card, because
// Vega, Navi and RDNA all contain an internal PCIe switch. Width is read from the HOST link only;
// the threshold is in the ★ block below. This matches Python's `_marginal` byte for byte.
// ★ THE DEFAULT REQUIRES AN x8-OR-WIDER HOST LINK (tightened 2026-10-12). It used to require only
// "wider than x4", which excluded an x1 card but let an x4 card through — and x4 is exactly the shape
// the incident was: an externally-docked card, an M.2/external adapter, or a shared-port card. The rule
// is on the HOST link width, never on the card's own bridge. This matches Python's `_marginal` byte
// for byte.
static std::optional<string> marginal_reason(const J &g) {
    const J *L = jget(g, "link");
    auto w = L ? ifield(*L, "max_width") : std::nullopt;
    if (w && *w < 8) return "x" + std::to_string(*w) + " link at the host — too narrow to use unasked";
    return std::nullopt;
}

// A SYNTHETIC load: ffmpeg's OWN generated test pattern (testsrc2), piped to /dev/null. The encoder
// really runs — that is why the counters move — but the input is a test card and the output is
// discarded. A LOAD proves a card can be DRIVEN; a JOB produces something. --work is the job.
//
// ★ BOUNDED IN TIME, PACED TO REAL TIME (fix 2026-10-11, from the final outside read). The old load ran
// until the game exited — an uninvited, unbounded workload on every spare card, the shape of the
// incident where a card on an external dock latched its thermal alarm and took the machine down. It now
// runs for `cap` seconds of REAL time and stops: `-re` paces the generated input to native frame rate
// (without it ffmpeg would encode the whole clip in a few seconds and exit), and `-t` bounds it.
static pid_t start_test_load(const string &node, const string &va, int cap) {
    vector<string> env = current_env();
    set_env(env, "LIBVA_DRIVER_NAME", va);
    vector<string> argv = {"ffmpeg", "-hide_banner", "-loglevel", "error",
                           "-re", "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=60",
                           "-t", std::to_string(cap),
                           "-vaapi_device", "/dev/dri/" + node,
                           "-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-f", "null", "-"};
    return spawn_detached(argv, env);
}

static string base_of(const string &path) {
    size_t sl = path.rfind('/');
    return (sl == string::npos) ? path : path.substr(sl + 1);
}

static string stem_of(const string &path) {
    size_t sl = path.rfind('/');
    string b = (sl == string::npos) ? path : path.substr(sl + 1);
    // os.path.splitext: a leading dot is NOT an extension separator (".bashrc" -> (".bashrc", "")).
    size_t dot = b.rfind('.');
    if (dot == string::npos || dot == 0) return b.empty() ? "job" : b;
    string s = b.substr(0, dot);
    return s.empty() ? "job" : s;
}

// os.path.abspath: join to cwd when relative, then normalise (collapse "//", ".", "..").
static string abspath(const string &p) {
    string path = p;
    bool abs = !path.empty() && path[0] == '/';
    if (!abs) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) path = string(cwd) + "/" + path;
    }
    vector<string> parts;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == string::npos) j = path.size();
        string comp = path.substr(i, j - i);
        if (comp == "." || comp.empty()) {
            // skip
        } else if (comp == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(comp);
        }
        i = j + 1;
    }
    string out = (path.size() && path[0] == '/') ? "/" : "";
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += "/";
        out += parts[k];
    }
    if (out.empty()) out = ".";
    return out;
}

// os.path.dirname.
static string dir_name(const string &path) {
    size_t sl = path.rfind('/');
    if (sl == string::npos) return "";
    if (sl == 0) return "/";
    return path.substr(0, sl);
}

// Duration in seconds, or -1. -1 is returned, never a guess: a split whose extent is guessed drops
// the tail silently.
//
// ★ NO SHELL. THIS USED TO BE A COMMAND INJECTION (found 2026-10-11 by an outside review; reproduced
// here by construction). It built the string `ffprobe … '<filename>'` and handed it to popen(), so a
// filename containing a single quote closed the quoting and everything after it was executed as a
// separate command. `spirit play --work "x.mkv'; rm -rf ~; echo '"` was a live hazard in published
// source. It now passes an argv vector straight to exec, the way Python's subprocess does, so the
// filename is never seen by a shell. There is no quoting to get wrong because there is no shell.
static double media_duration(const string &src) {
    vector<string> argv = {"ffprobe", "-v", "error", "-show_entries", "format=duration",
                           "-of", "default=nw=1:nk=1", src};
    std::optional<string> out = run_capture(argv, nullptr);
    if (!out) return -1.0;
    try {
        return std::stod(trim(*out));
    } catch (...) {
        return -1.0;
    }
}

// A REAL job on a spare card, to a real file — the WHOLE input, or one SEGMENT of it.
// seg >= 0 SPLITS one job across cards: each card gets a slice and the pieces have to add back up.
// Without it every card transcodes the same whole file, which is REPLICATION, a weaker claim.
static std::pair<string, pid_t> start_work(const string &node, const string &src,
                                           const string &outdir, const string &stem,
                                           long long seg, double ss, double dur, const string &va) {
    vector<string> env = current_env();
    set_env(env, "LIBVA_DRIVER_NAME", va);
    char tag[256];
    if (seg >= 0) snprintf(tag, sizeof(tag), "%s_part%lld", node.c_str(), seg + 1);
    else          snprintf(tag, sizeof(tag), "%s", node.c_str());
    string out = outdir + "/" + stem + "." + tag + ".mp4";
    vector<string> argv = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-y"};
    if (seg >= 0) {
        char ssbuf[64], dbuf[64];
        snprintf(ssbuf, sizeof(ssbuf), "%.3f", ss);
        snprintf(dbuf, sizeof(dbuf), "%.3f", dur);
        argv.push_back("-ss"); argv.push_back(ssbuf);
        argv.push_back("-t");  argv.push_back(dbuf);
    }
    argv.push_back("-i"); argv.push_back(src);
    argv.push_back("-vaapi_device"); argv.push_back("/dev/dri/" + node);
    argv.push_back("-vf"); argv.push_back("format=nv12,hwupload");
    argv.push_back("-c:v"); argv.push_back("h264_vaapi");
    argv.push_back("-b:v"); argv.push_back("8M");
    argv.push_back(out);
    return {out, spawn_detached(argv, env)};
}

// Join the split pieces into ONE file, stream-copy (no re-encode). See Python's `_join_slices` for the
// why: `--split` proved the work was DIVIDED, and this gives the user their video back as ONE file.
// Returns {ok, detail}. `detail` carries NO interpolated runtime error text, so all three builds agree.
static std::pair<bool, string> join_slices(const vector<string> &parts, const string &out_path) {
    string lst = out_path + ".concat.txt";
    FILE *f = fopen(lst.c_str(), "w");
    if (!f) return {false, "the join could not be started (check ffmpeg and the output path)"};
    for (const auto &p : parts) {
        string ap = abspath(p), esc;
        for (char c : ap) { if (c == '\'') esc += "'\\''"; else esc += c; }
        fprintf(f, "file '%s'\n", esc.c_str());
    }
    fclose(f);
    unlink(out_path.c_str());
    ProcResult r = run_capture_both({"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                                     "-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", out_path},
                                    nullptr, 3600);
    unlink(lst.c_str());
    if (!r.ok) return {false, "the join could not be started (check ffmpeg and the output path)"};
    struct stat st;
    if (r.rc != 0 || stat(out_path.c_str(), &st) != 0)
        return {false, "ffmpeg could not join the pieces (rc=" + std::to_string(r.rc) + ")"};
    return {true, ""};
}

// The card's short name, as the role table prints it.
// ★ SLICE ON A CODE-POINT BOUNDARY (C1). Python's _gpu_name takes the first 22 CODE POINTS; the port
// used substr(0, 22) — bytes — so a name whose 22nd byte fell inside a multi-byte character emitted
// invalid UTF-8 and misaligned the columns. utf8_cpslice/utf8_cplen count like Python.
static string gpu_name(const J &g) {
    string n = sfield(g, "name");
    if (n.empty()) n = sfield(g, "card");
    for (const char *cut : {"[", "(", "  "}) {
        size_t i = n.find(cut);
        if (i != string::npos && i > 0) n = n.substr(0, i);
    }
    while (!n.empty() && n.back() == ' ') n.pop_back();
    return utf8_cpslice(n, 22);
}

// True if this is a Proton/Wine launch, where the mount-namespace wall MUST be skipped.
// MEASURED 2026-10-10: `unshare -r` maps our uid to 0 inside, and Wine/Proton REFUSE TO RUN AS
// ROOT — the launch dies with `bin_steam.sh: Error: Cannot run as root user` before the game ever
// starts. Detect it and fall back to the pin alone, and SAY SO rather than silently weakening it.
// NO ENV LOOKUP HERE. An earlier version also checked STEAM_COMPAT_DATA_PATH, which meant a stray
// variable left in a shell silently disabled the wall for NATIVE games too. Decide on the ARGV.
static bool is_wine_target(const vector<string> &cmd) {
    for (const string &a : cmd) {
        size_t sl = a.rfind('/');
        string b = (sl == string::npos) ? a : a.substr(sl + 1);
        for (char &c : b) c = (char)tolower((unsigned char)c);
        if (b.size() >= 4 && b.compare(b.size() - 4, 4, ".exe") == 0) return true;
        if (b.find("proton") != string::npos) return true;
        if (b == "wine") return true;
        if (b.compare(0, 4, "wine") == 0) return true;
    }
    return false;
}

// True if this launches the STEAM CLIENT itself (or a `steam://` URL) rather than the game. `steam`
// is not a Wine target, so the old code wrapped it in the fake-root namespace — and Steam's own
// bin_steam.sh refuses as root. Worse, when the client is ALREADY running, `steam -applaunch` /
// `steam://` hands the request to it and returns at once, so the game starts with none of our
// environment (unpinned) while the tool prints a normal exit. This tool cannot pin a process the
// Steam client starts, so the second wall is skipped for it, exactly as Python's `_is_steam_launcher`.
static bool is_steam_launcher(const vector<string> &cmd) {
    for (const string &a : cmd) {
        if (a.rfind("steam://", 0) == 0) return true;
        size_t sl = a.rfind('/');
        string b = (sl == string::npos) ? a : a.substr(sl + 1);
        for (char &c : b) c = (char)tolower((unsigned char)c);
        if (b == "steam") return true;
    }
    return false;
}

// The running program's own absolute path, for the Steam launch-options recipe. Mirrors spirit.py's
// `os.path.abspath(__file__)` and the Rust build's `/proc/self/exe` readlink: a stranger copying the
// recipe out must get the path to THIS binary, not a guess.
static string self_exe_path() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return string(buf); }
    return "spirit";
}

// True when the pin is actually REACHING processes: this one already carries it, or the user manager
// does (which is what apps started from now on inherit). Never "a file exists" — the file proves only
// that someone wrote it, and a Steam that was already running never picked it up. Mirrors
// spirit.py::_pin_active.
static bool pin_active() {
    const char *own = getenv("MESA_VK_DEVICE_SELECT");
    if (own && *own) return true;
    if (!which("systemctl")) return false;
    ProcResult r = run_capture_both({"systemctl", "--user", "show-environment"}, nullptr, 10);
    if (!r.ok || r.rc != 0) return false;
    return r.out.rfind("MESA_VK_DEVICE_SELECT=", 0) == 0
        || r.out.find("\nMESA_VK_DEVICE_SELECT=") != string::npos;
}

// True if unprivileged user namespaces WORK here, false if they are denied, nullopt if no `unshare`.
// Cached once per process like Python's _USERNS_CACHE. Mirrors spirit.py::_userns_available.
static std::optional<bool> userns_available() {
    static bool done = false;
    static std::optional<bool> cache;
    if (done) return cache;
    done = true;
    if (!which("unshare")) { cache = std::nullopt; return cache; }
    vector<string> argv = {"unshare", "-Urm", "true"};
    ProcResult r = run_capture_both(argv, nullptr, 10);
    cache = r.ok ? std::optional<bool>(r.rc == 0) : std::optional<bool>(false);
    return cache;
}

// The Steam install that has a `steamapps` directory, or nullopt. Never guessed from a path shape.
static std::optional<string> steam_root() {
    for (const string &c : {home_dir() + "/.steam/steam", home_dir() + "/.local/share/Steam"}) {
        struct stat sb;
        if (stat((c + "/steamapps").c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) return c;
    }
    return std::nullopt;
}

// os.makedirs(path, exist_ok=True): create path and every missing parent.
static bool mkdirs(const string &path) {
    if (path.empty()) return false;
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            string sub = path.substr(0, i);
            if (sub.empty()) continue;
            struct stat sb;
            if (stat(sub.c_str(), &sb) == 0) {
                if (!S_ISDIR(sb.st_mode)) return false;
            } else if (mkdir(sub.c_str(), 0777) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

// The Steam appid that owns this launch, resolved through the manifest's `installdir`.
// NOT PARSED OFF THE PATH SPELLING: `appmanifest_<appid>.acf` carries `"installdir" "<folder>"`,
// and that is what is matched. Returns nullopt rather than a guess.
static std::optional<string> appid_for_cmd(const vector<string> &cmd) {
    auto steam = steam_root();
    if (!steam) return std::nullopt;
    string apps = *steam + "/steamapps";
    std::set<string> wanted;
    std::regex rx("steamapps/common/([^/]+)/");
    for (const string &a : cmd) {
        std::smatch m;
        if (std::regex_search(a, m, rx)) wanted.insert(m[1].str());
    }
    if (wanted.empty()) return std::nullopt;
    vector<string> mfs;
    for (string &f : glob_prefix(apps, "appmanifest_"))
        if (f.size() > 4 && f.compare(f.size() - 4, 4, ".acf") == 0) mfs.push_back(f);
    std::sort(mfs.begin(), mfs.end());
    std::regex ird("\"installdir\"\\s*\"([^\"]+)\"");
    for (const string &mf : mfs) {
        FILE *f = fopen(mf.c_str(), "r");
        if (!f) continue;
        string txt;
        { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0) txt.append(buf, n); }
        fclose(f);
        std::smatch im;
        if (std::regex_search(txt, im, ird) && wanted.count(im[1].str())) {
            string b = base_of(mf);
            const size_t pfx = strlen("appmanifest_"), sfx = strlen(".acf");
            return b.substr(pfx, b.size() - pfx - sfx);
        }
    }
    return std::nullopt;
}

// Create `steamapps/compatdata/<appid>/` when it is absent, before a Proton launch. Proton's
// setup_prefix() takes a lock file INSIDE compatdata/<appid>/, so a first-ever launch dies with a
// bare FileNotFoundError it cannot recover from. One makedirs is the whole fix; a no-op otherwise.
static void ensure_compatdata(const vector<string> &cmd) {
    if (!is_wine_target(cmd)) return;
    auto appid = appid_for_cmd(cmd);
    if (!appid) return;
    auto steam = steam_root();
    if (!steam) return;
    string d = *steam + "/steamapps/compatdata/" + *appid;
    struct stat sb;
    if (stat((d + "/pfx").c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) return; // real prefix present
    bool made = !(stat(d.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode));
    if (!mkdirs(d)) {
        // ★ NO strerror() HERE, deliberately: each language spells its own runtime's error
        // differently, so interpolating it makes this the one branch the three builds cannot agree on —
        // and a read-only Steam directory or a full disk reaches it. Same words in every build instead.
        printf("  prefix: could not create %s\n", d.c_str());
        printf("          (Proton needs this directory for its prefix; check that you can write to it)\n");
        return;
    }
    if (made) {
        printf("  prefix: FIRST-EVER LAUNCH of appid %s — created %s\n", appid->c_str(), d.c_str());
        printf("          (Proton's own prefix setup crashes on a missing compatdata dir; it cannot\n");
        printf("           create the directory it needs, because its lock file lives inside it)\n");
    }
}

// [build] the game argv with the OTHER cards' /dev/dri nodes hidden behind a mount namespace.
// KEEP THE NAMESPACE INSIDE ANY OUTER WRAPPER: `unshare -r` maps our uid to root, so the
// ROOT-OWNED /tmp/.X11-unix presents as the overflow uid inside — a nested gamescope then dies
// at xwayland/sockets.c:83 "not owned by root or us". gamescope goes OUTSIDE this function; only
// the game is inside. Build here, call from the caller.
static vector<string> hidden_argv(const vector<string> &cmd, const J &main, const J &db) {
    if (is_wine_target(cmd) || is_steam_launcher(cmd)) return cmd;
    vector<string> hide;
    const J *ga = gpus_arr(db);
    if (ga) for (auto &g : ga->arr) {
        if (sfield(g, "card") == sfield(main, "card")) continue;
        for (const string &n : {sfield(g, "card"), sfield(g, "render_node")})
            if (!n.empty()) hide.push_back(n);
    }
    if (!hide.empty()) {
        auto avail = userns_available();
        if (!avail) {
            // ★ A MISSING `unshare` IS NOT SILENT ANY MORE (§2 item 13). It used to just return the
            // command, so a machine without the binary got no wall and no message.
            printf("  wall: unshare is not installed — only the pin separates the game from the "
                   "other cards' device nodes.\n");
            fflush(stdout);
            return cmd;
        }
        if (!*avail) {
            // ★ THE FALLBACK THAT KEEPS THE GAME LAUNCHING (§1 item 1). On Ubuntu 24.04 or a Debian
            // with `unprivileged_userns_clone=0`, `unshare -r` FAILS. The old code wrapped the launch
            // in it anyway, so the game never started — and the tool reported the session as normal.
            // A wall that cannot be built must never stop a launch: fall back to the pin alone, and
            // SAY SO rather than weakening the wall in silence.
            printf("  wall: the second wall is unavailable here — unprivileged user namespaces are "
                   "denied on this machine, so the game\n");
            printf("        launches WITHOUT the namespace; only the pin separates it from the other "
                   "cards' device nodes.\n");
            fflush(stdout);
            return cmd;
        }
        string script =
            "hide=\"$1\"; shift; for n in $hide; do "
            "if [ -e \"/dev/dri/$n\" ]; then "
            "mount --bind /dev/null \"/dev/dri/$n\" 2>/dev/null || "
            "echo \"  wall: could NOT hide /dev/dri/$n - the game can still see this card\" >&2; "
            "fi; done; exec \"$@\"";
        vector<string> argv = {"unshare", "-rm", "sh", "-c", script, "sh", join(hide, " ")};
        for (auto &c : cmd) argv.push_back(c);
        return argv;
    }
    return cmd;
}

static int play_cmd(const J &db, const vector<string> &cmd_in, bool do_offload,
                    std::optional<double> scale, std::optional<int> framegen, bool overlay,
                    string work, string work_outdir, bool split, string only_spares,
                    std::optional<int> test_load) {
    if (cmd_in.empty()) {
        fprintf(stderr, "usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] "
                        "[--work FILE [--work-outdir DIR] [--split]] [--spare NODE,...] -- <game command...>\n");
        return 1;
    }
    vector<string> cmd = cmd_in;
    const J *main = main_card(db);
    if (!main) {
        // ★ NO CARDS IS NOT A CRASH (fix 2026-10-11, outside review). This used to reach max() on an
        // empty sequence and raise ValueError with a traceback. Python prints this line and returns 1.
        printf("  no GPUs were found, so there is nothing to render on and nothing to work on.\n");
        return 1;
    }
    vector<const J *> spares;
    const J *ga = gpus_arr(db);
    if (ga) for (auto &g : ga->arr)
        if (sfield(g, "card") != sfield(*main, "card")) spares.push_back(&g);
    if (!only_spares.empty()) {
        // ★ --spare NODE[,NODE...] — the human names the cards that work. Everything else is left
        // alone entirely, not merely idle. A real session runs for hours unattended, and an old card
        // on a marginal link is the human's risk to take, not a default we assume for them.
        // ★ DEDUPE and report the nodes we do not know (C6): a repeated node used to start two encoders
        // on one card, and an unknown node used to refuse without saying which one it did not recognise.
        std::set<string> want;
        {
            string s = only_spares;
            for (char &c : s) if (c == ',') c = ' ';
            size_t i = 0;
            while (i <= s.size()) {
                size_t j = s.find(' ', i);
                if (j == string::npos) j = s.size();
                string tok = trim(s.substr(i, j - i));
                if (!tok.empty()) want.insert(tok);
                i = j + 1;
            }
        }
        std::set<string> spare_nodes;
        for (auto *g : spares) spare_nodes.insert(base_of(sfield(*g, "render_node")));
        vector<string> missing;
        for (auto &w : want) if (!spare_nodes.count(w)) missing.push_back(w);
        if (!missing.empty())
            printf("  spare: ignoring unknown render node(s): %s\n", join(missing, ", ").c_str());
        vector<const J *> kept;
        for (auto *g : spares)
            if (want.count(base_of(sfield(*g, "render_node")))) kept.push_back(g);
        if (kept.empty()) {
            vector<string> wl(want.begin(), want.end());
            printf("  spare: REFUSED — none of %s is a spare card here. The game would run with no"
                   " cards working beside it.\n", py_list_repr(wl).c_str());
            return 1;
        }
        string kepts;
        for (auto *g : kept) {
            if (!kepts.empty()) kepts += ", ";
            kepts += base_of(sfield(*g, "render_node"));
        }
        printf("  spare: %d of %d card(s) allowed to work: %s\n", (int)kept.size(), (int)spares.size(),
               kepts.c_str());
        spares = kept;
    }

    printf("spirit play — the game on your monitor card; the other cards stay idle unless you give them work\n\n");
    const J *L = jget(*main, "link");
    auto xw = L ? ifield(*L, "max_width") : std::nullopt;
    string mgen = L ? s_or_none(*L, "max_gen") : "None";
    auto gm = L ? ffield(*L, "gbps_max") : std::nullopt;
    string mname = utf8_cpslice(sfield(*main, "name"), 30);
    printf("  MAIN  (renders)  %-6s %-30s x%s %s  %s GB/s\n", sfield(*main, "card").c_str(),
           pad_display(mname, 30).c_str(), xw ? std::to_string(*xw).c_str() : "None", mgen.c_str(),
           gm ? py_float_repr(*gm).c_str() : "None");
    for (auto *g : spares) {
        string sn = utf8_cpslice(sfield(*g, "name"), 30);
        string rn = sfield(*g, "render_node");
        if (rn.empty()) rn = "?";
        printf("  SPARE (idle unless asked)  %-6s %-30s node /dev/dri/%s\n", sfield(*g, "card").c_str(),
               pad_display(sn, 30).c_str(), rn.c_str());
    }

    vector<string> env = current_env();
    string pci = dri_prime(sfield(*main, "bdf"));
    set_env(env, "DRI_PRIME", pci);
    string vendor = sfield(*main, "vendor"), device = sfield(*main, "device");
    string mvs = (vendor.size() > 2 ? vendor.substr(2) : string()) + ":" +
                 (device.size() > 2 ? device.substr(2) : string()) + "!";
    set_env(env, "MESA_VK_DEVICE_SELECT", mvs);
    printf("\n  pin: MESA_VK_DEVICE_SELECT=%s   DRI_PRIME=%s\n", mvs.c_str(), pci.c_str());

    // UPSCALING: wrap the game in gamescope and scale it up with FSR 1.0.
    if (scale && *scale != 0.0) {
        auto gs = gamescope_state();
        auto native = native_res(db);
        if (gs.first != "READY") {
            printf("  scale: REFUSED — %s\n", gs.second.c_str());
            return 1;
        }
        if (!native) {
            printf("  scale: REFUSED — could not read the native mode of the monitor card's output\n");
            return 1;
        }
        auto r = render_res(*native, *scale);
        printf("  scale: render %lldx%lld -> present %lldx%lld via gamescope FSR 1.0  (%.0f%% of the pixels)\n",
               r.first, r.second, native->first, native->second, 100.0 * (*scale) * (*scale));
    }

    // FRAME GENERATION: drive lsfg-vk if it is here. We never ship or modify it.
    if (framegen && *framegen != 0) {
        auto ls = lsfg_state();
        // ★ "READY" was renamed to "INSTALLED" when this check moved to the Steam manifest.
        if (ls.first != "INSTALLED") {
            printf("  framegen: UNAVAILABLE — %s\n", ls.second.c_str());
            printf("            (launching without it; spirit boost explains the state)\n");
        } else {
            auto prof = ensure_lsfg_profile(*framegen);
            if (prof.first) {
                set_env(env, "LSFGVK_PROFILE", *prof.first);
                if ((scale && *scale != 0.0) || overlay) {
                    // ★ lsfg-vk's own troubleshooting: under gamescope with vsync pacing, set
                    // ENABLE_GAMESCOPE_WSI=0 or frame generation does not engage at all. This applies
                    // whenever gamescope is in play — including --overlay alone, with no upscaling.
                    set_env(env, "ENABLE_GAMESCOPE_WSI", "0");
                }
                printf("  framegen: %dx via lsfg-vk, profile %s — %s\n", *framegen,
                       py_repr(*prof.first).c_str(), prof.second.c_str());
            } else {
                printf("  framegen: UNAVAILABLE — %s\n", prof.second.c_str());
            }
        }
    }

    // ★ ARM THE WITNESS. Only meaningful with frame gen on; harmless otherwise, and we say so
    // plainly if it is missing rather than reporting a frame-gen claim we could not measure.
    string fg_count_path;
    if (framegen && *framegen != 0) {
        auto cst = counter_state();
        if (cst.first == "READY") {
            fg_count_path = temp_dir() + "/spirit-presents-" + std::to_string(getpid()) + ".txt";
            unlink(fg_count_path.c_str());
            set_env(env, "SPIRIT_PRESENT_COUNT", fg_count_path);
            printf("  witness: counting images presented to the compositor (summary at exit)\n");
        } else {
            printf("  witness: UNAVAILABLE — %s\n", cst.second.c_str());
        }
    }

    // ★ THE STEAM PATH IS SAID, NOT SILENT (blocking item 2). `steam`/`steam://` is not the game
    // — it is a REQUEST to the Steam client, which starts the game itself, in its own environment.
    // This tool cannot put the pin there, so it says so and gives the recipe that CAN pin it.
    bool steam_launch = is_steam_launcher(cmd);
    if (steam_launch) {
        // ★ THE STEAM PATH IS SAID, NOT SILENT (blocking item 2). `steam`/`steam://` is not the game
        // — it is a REQUEST to the Steam client, which starts the game itself, in its own environment.
        // If the session-wide pin is on, Steam inherits it and the game IS pinned; otherwise the tool
        // gives the recipe that pins it. Either way it says which case you are in.
        if (pin_active()) {
            printf("  steam: this is a Steam launch, and the session pin is ACTIVE — a game started from\n");
            printf("         now on is pinned with this tool nowhere in the loop, because the pin reaches\n");
            printf("         Steam's games through your session environment.\n");
            printf("         (If Steam was ALREADY RUNNING when you armed it, restart Steam: its\n");
            printf("          environment was fixed at its own start and it did not pick the pin up.)\n");
        } else {
            printf("  steam: this is a Steam launch — it cannot be pinned from here.\n");
            printf("         `steam`/`steam://` hands the request to the running Steam client and returns\n");
            printf("         at once, so the game would start WITHOUT this tool's pin. To pin a Steam game,\n");
            printf("         set its launch options to:\n");
            printf("             \"%s\" play -- %%command%%\n", self_exe_path().c_str());
            printf("         (note the `--` before %%command%%) — OR arm the whole session with\n");
            printf("             spirit enable\n");
            printf("         which covers every launch from now on, Steam included. It dies at reboot\n");
            printf("         unless you add --persist.\n");
        }
    }
    if (is_wine_target(cmd)) {
        printf("  wall: Proton/Wine detected — skipping the mount namespace (Wine refuses to run as\n");
        printf("        root, and `unshare -r` makes us root); the exclusive pin still applies.\n");
        // ★ Before the launch, make sure Proton has somewhere to put its prefix. A first-ever
        // launch otherwise dies inside Proton with a bare FileNotFoundError on a lock file in a
        // directory Proton cannot create. Cheap, idempotent, and it speaks only when it mattered.
        ensure_compatdata(cmd);
    }

    vector<pid_t> procs;
    vector<string> proc_nodes;                        // parallel to procs — which card each load went to
    std::map<string, int> died_early;                 // render node -> rc, loads already gone at game exit
    vector<std::pair<string, string>> job_outputs;   // (node, out path)
    double job_expect = -1.0;
    string no_job_reason;                             // set when --work was asked for but nothing could start
    string no_load_reason;                            // set when --test-load was asked for but nothing could start
    bool load_started = false;                        // True once at least one bounded test load is running

    // ★ REFUSE --overlay BEFORE ANYTHING IS SPAWNED (fix 2026-10-11, outside review). This check used
    // to run AFTER the spare cards had already been put to work, so refusing the overlay left the
    // encoders running with no game to stop them — orphaned processes on a machine the user is about
    // to keep using. A refusal that leaves work running is not a refusal.
    if (do_offload && overlay && !scale) {
        auto gs = gamescope_state();
        if (gs.first != "READY" || !native_res(db)) {
            printf("  overlay: REFUSED — gamescope is not ready (%s)\n", gs.second.c_str());
            return 1;
        }
    }
    // ★ WHICH SPARES CAN ACTUALLY ENCODE, decided from each card's own kernel driver, before anything
    // is launched. A card we cannot drive is left out and NAMED — the alternative is an ffmpeg that
    // dies instantly with its output going to /dev/null, which looks exactly like a card doing work.
    struct Skip { string node, name, why; };
    vector<std::pair<string, string>> encoders;       // (render_node, va_driver)
    vector<Skip> skipped;                             // (node_or_card, name, reason)
    vector<Skip> marginal;                            // (render_node, name, why) — usable only if named
    for (auto *g : spares) {
        string node = sfield(*g, "render_node");
        auto vd = va_driver(*g);
        if (vd.first.empty() || node.empty()) {
            Skip s;
            s.node = node.empty() ? sfield(*g, "card") : base_of(node);
            s.name = gpu_name(*g);
            s.why  = vd.second.empty() ? "no render node" : vd.second;
            skipped.push_back(s);
            continue;
        }
        // ★ A REAL JOB DOES NOT REACH A MARGINAL CARD UNLESS THE USER NAMES IT (from outside design
        // review, 2026-10-11). When the human named the cards with --spare, honour that; otherwise a
        // card on a narrow link or behind an external bridge is left alone and NAMED before launch.
        if (only_spares.empty()) {
            auto mw = marginal_reason(*g);
            if (mw) {
                Skip m;
                m.node = base_of(node);
                m.name = gpu_name(*g);
                m.why  = *mw;
                marginal.push_back(m);
                continue;
            }
        }
        encoders.push_back({base_of(node), vd.first});
    }
    bool want_work = !work.empty();
    bool want_load = test_load.has_value();
    if (do_offload && (want_work || want_load) && which("ffmpeg")) {
        for (auto &s : skipped)
            printf("  spare: SKIPPED %s (%s) — %s\n", s.node.c_str(), s.name.c_str(), s.why.c_str());
        const char *_what = work.empty() ? "load" : "job";
        for (auto &m : marginal) {
            printf("  %s: NOT using %s (%s) — %s\n", _what, m.node.c_str(), m.name.c_str(), m.why.c_str());
            printf("       Add --spare %s to include it anyway.\n", m.node.c_str());
        }
        // ★ NOTHING TO RUN A JOB ON — say it here, before the job block, so the reason is set for the
        // "no job was started" line at the end (A1, fix 2026-10-11).
        if (!work.empty() && encoders.empty()) {
            no_job_reason = "no spare card on this machine can encode (see the SKIPPED lines above)";
            printf("  work: no job started — %s\n", no_job_reason.c_str());
        }
        if (!work.empty()) {
            // A REAL JOB. Real input, real output, on cards that are NOT the one running the game.
            struct stat st;
            if (stat(work.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                // ★ ONE UNIFORM TWO-LINE REFUSAL SHAPE for every refusal in this file (fix 2026-10-11).
                printf("  work: REFUSED — %s is not a file\n", work.c_str());
                printf("          (a job needs a real input; without one the spare cards run a test "
                       "load, which produces nothing)\n");
                return 1;
            }
            // ★ BESIDE THE INPUT, which is what --work-outdir's own help text has always promised.
            // It used to default to the CURRENT DIRECTORY. (Found by the outside review, 2026-10-11.)
            // ★ --work-outdir IS NORMALISED WITH abspath (C7), which collapses `..`; the raw path used
            // to reach mkdirs() and the output path, so `a/../b` and `b` named two different places.
            if (work_outdir.empty()) work_outdir = dir_name(abspath(work));
            else work_outdir = abspath(work_outdir);
            if (work_outdir.empty()) {
                char cw[4096];
                work_outdir = getcwd(cw, sizeof(cw)) ? string(cw) : ".";
            }
            if (!mkdirs(work_outdir)) {
                // ★ NO strerror() HERE, deliberately — see the compatdata branch. Each language spells
                // its own runtime's error differently, so interpolating it makes this a branch the three
                // builds cannot agree on. Same words in every build instead.
                printf("  work: REFUSED — cannot use %s\n", work_outdir.c_str());
                printf("          (the job needs a directory it can write to; check the path)\n");
                return 1;
            }
            string stem = stem_of(work);
            if (split && encoders.size() > 1) {
                double total = media_duration(work);
                if (total <= 0.0) {
                    printf("  split: REFUSED — could not read the duration of %s, and a split whose "
                           "extent is guessed drops the tail.\n", base_of(work).c_str());
                    return 1;
                }
                size_t n = encoders.size();
                double span = total / (double)n;
                printf("  ★ SPLIT JOB: %s is %.1fs — divided into %zu slices across %zu cards, one each.\n",
                       base_of(work).c_str(), total, n, n);
                for (size_t i = 0; i < n; i++) {
                    double ss = (double)i * span;
                    double dur = (i < n - 1) ? span : (total - ss);
                    auto r = start_work(encoders[i].first, work, work_outdir, stem, (long long)i, ss,
                                        dur, encoders[i].second);
                    if (r.second > 0) { procs.push_back(r.second); proc_nodes.push_back(encoders[i].first); }
                    job_outputs.push_back({encoders[i].first, r.first});
                    printf("      %-12s slice %zu  [%6.1fs .. %6.1fs] -> %s\n", encoders[i].first.c_str(),
                           i + 1, ss, ss + dur, base_of(r.first).c_str());
                }
                job_expect = total;
            } else {
                if (split)
                    printf("  split: only one spare card has a render node — replicating instead, and\n"
                           "         saying so rather than calling it a split.\n");
                for (auto &e : encoders) {
                    auto r = start_work(e.first, work, work_outdir, stem, -1, 0.0, 0.0, e.second);
                    if (r.second > 0) { procs.push_back(r.second); proc_nodes.push_back(e.first); }
                    job_outputs.push_back({e.first, r.first});
                }
                if (!procs.empty()) {
                    // ★ THE BASENAME, not stem + ".mp4" (C9): a `clip.mkv` input used to announce
                    // itself as `clip.mkv.mp4`.
                    printf("  ★ JOB: transcoding %s on %d spare card(s) — real input, real output. "
                           "(WHOLE file on each card = REPLICATION, not a split; add --split to divide "
                           "one job between them.)\n", base_of(work).c_str(), (int)procs.size());
                    for (auto &jo : job_outputs)
                        printf("      %s -> %s\n", jo.first.c_str(), jo.second.c_str());
                }
            }
        } else if (want_load) {
            for (auto &e : encoders) {
                pid_t p = start_test_load(e.first, e.second, *test_load);
                if (p > 0) { procs.push_back(p); proc_nodes.push_back(e.first); }
            }
            if (!procs.empty()) {
                load_started = true;
                printf("  TEST LOAD on %d card(s) for %ds wall time, then it stops.\n", (int)procs.size(), *test_load);
                printf("         It writes no file. It only shows these cards can be driven.\n");
                printf("         For real work: spirit play --work <file> [--split] -- <game>\n");
            } else {
                // ★ NO ENCODABLE SPARE IS SAID, NOT SILENT ( blocking item 9).
                no_load_reason = "no spare card on this machine can be driven (see the SKIPPED lines above)";
                printf("  test-load: no load started — %s\n", no_load_reason.c_str());
            }
        }
    } else if (work.empty() && !want_load) {
        // ★ THE DEFAULT, SAID OUT LOUD (from the final outside read). Doing nothing silently would read
        // as the tool being broken; saying nothing runs on the other cards makes the default honest.
        printf("  no job given — your other cards will stay idle.\n");
        printf("          give them a real job:      spirit play --work FILE -- CMD\n");
        printf("          check they can be driven:  spirit play --test-load 60 -- CMD\n");
    } else if (!work.empty() && do_offload) {
        // ★ An absent tool must be SAID, not silently skipped (the README promises exactly this, and
        // the promise was false for ffmpeg). Found 2026-10-11 by an outside review.
        no_job_reason = "ffmpeg is not installed, so no card can be given a job here";
        printf("  work: no job started — %s\n", no_job_reason.c_str());
    } else if (!work.empty()) {
        no_job_reason = "--no-offload was given, so no card was asked to do anything";
        printf("  work: no job started — %s\n", no_job_reason.c_str());
    } else if (want_load && !do_offload) {
        // ★ THE THREE SILENT / FALSE TEST-LOAD CASES ( blocking item 9). Each names why no
        // load started, so a --test-load that began nothing never asserts a load ran.
        no_load_reason = "--no-offload was given, so no card was asked to do anything";
        printf("  test-load: no load started — %s\n", no_load_reason.c_str());
    } else if (want_load) {
        no_load_reason = "ffmpeg is not installed, so no card can be driven here";
        printf("  test-load: no load started — %s\n", no_load_reason.c_str());
    }
    printf("\n  launching…\n\n");
    fflush(stdout);

    // Build the hidden game argv FIRST, then wrap it in gamescope — gamescope stays OUTSIDE the
    // mount namespace (see hidden_argv).
    vector<string> inner = hidden_argv(cmd, *main, db);
    bool scale_on = scale && *scale != 0.0;
    if (scale_on || overlay) {
        auto nat = native_res(db);
        std::pair<long long, long long> rr =
            scale_on ? render_res(*nat, *scale) : *nat;
        // -w/-h = the GAME's render size; -W/-H = the OUTPUT (were swapped: --scale was a no-op)
        vector<string> gs = {"gamescope", "-w", std::to_string(rr.first), "-h",
                             std::to_string(rr.second), "-W", std::to_string(nat->first),
                             "-H", std::to_string(nat->second), "-f"};
        if (scale_on) {
            gs.push_back("-F"); gs.push_back("fsr");
            gs.push_back("--fsr-sharpness"); gs.push_back("2");
        }
        if (overlay) {
            // THE PRESENT-COUNTER. mangoapp runs INSIDE gamescope as a transparent overlay, so its
            // FPS readout is the COMPOSITOR's present rate - the only number frame generation moves.
            // The game's own counter reports frames IT rendered, which frame gen does not change.
            gs.push_back("--mangoapp");
            printf("  overlay: mangoapp inside gamescope — for the eye, not the evidence\n");
            fflush(stdout);   // same buffering reason as the Python
        }
        gs.push_back("--");
        for (auto &c : inner) gs.push_back(c);
        inner = gs;
    }
    struct timespec t_game_ts;
    clock_gettime(CLOCK_MONOTONIC, &t_game_ts);
    SpawnResultEx gr = spawn_wait_ex(inner, env);
    int rc = gr.rc;
    if (gr.exec_failed) {
        // ★ A CLEAN REFUSAL, NOT A TRACEBACK (fix 2026-10-11, outside review). A game command that
        // cannot be found used to raise out of play() as a Python traceback with no THE TAIL at all.
        rc = 127;
        printf("\n  could not run the game: [Errno %d] %s: %s\n", gr.err,
               strerror(gr.err), py_repr(gr.prog).c_str());
        printf("  Nothing was launched. The cards listed above did no work and have been stopped.\n");
        fflush(stdout);
    }
    // A TEST LOAD IS A LOAD AND DIES WITH THE GAME. A JOB IS NOT: killing a half-finished transcode
    // leaves a truncated file and calls it done. Jobs are WAITED FOR, then each output is checked
    // for size and duration ON DISK — the artifact, never the child's exit code.
    if (!work.empty()) {
        for (pid_t p : procs) { int st; waitpid(p, &st, 0); }
    } else {
        // ★ ASK EACH LOAD WHETHER IT IS STILL ALIVE, BEFORE KILLING IT (fix 2026-10-11, from the
        // outside review). THE TAIL used to say every spare was "alive for N s of the session", using
        // the GAME's elapsed time — and nothing ever polled the children. An ffmpeg that died at second
        // zero was therefore reported as having run the whole session. A check that can only agree with
        // you is a mirror, not a check: this one never asked the question, so it could not fail.
        died_early.clear();
        for (size_t i = 0; i < procs.size(); i++) {
            int st = 0;
            pid_t r = waitpid(procs[i], &st, WNOHANG);
            if (r == procs[i]) {
                int code = 0;
                if (WIFEXITED(st)) code = WEXITSTATUS(st);
                else if (WIFSIGNALED(st)) code = -WTERMSIG(st);
                died_early[proc_nodes[i]] = code;
            }
        }
        // ★ SIGTERM, THEN WAIT (fix 2026-10-11, outside review). Python and C++ terminated without
        // waiting, Rust killed outright; three different teardowns for the same act. A caller that
        // assumes the cards are free when play() returns was right only on one of the three. SIGTERM,
        // then wait (5 s), then kill, everywhere.
        for (pid_t p : procs) kill(p, SIGTERM);
        for (pid_t p : procs) {
            bool gone = false;
            for (int k = 0; k < 50; k++) {
                int st = 0;
                if (waitpid(p, &st, WNOHANG) == p) { gone = true; break; }
                struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 100000000L;
                nanosleep(&ts, nullptr);
            }
            if (!gone) kill(p, SIGKILL);
        }
    }
    if (!work.empty() && !procs.empty()) {
        printf("\n  game exited (rc=%d) — the job ran to completion (not stopped with the game)\n", rc);
        printf("  ── JOB OUTPUT ────────────────────────────────────────────────────────\n");
        double got = 0.0;
        int written = 0;
        for (auto &jo : job_outputs) {
            struct stat st;
            if (stat(jo.second.c_str(), &st) == 0) {
                double d = media_duration(jo.second);
                if (d > 0) { got += d; written++; }
                printf("  %-14s %s  (%.1f MB, %.1fs)\n", jo.first.c_str(), base_of(jo.second).c_str(),
                       (double)st.st_size / 1048576.0, d > 0 ? d : 0.0);
            } else {
                printf("  %-14s MISSING — %s was not written. The job failed; do not report it as done.\n",
                       jo.first.c_str(), jo.second.c_str());
            }
        }
        if (job_expect > 0.0) {
            printf("  ── SPLIT CHECK ───────────────────────────────────────────────────────\n");
            printf("  slices written      : %d of %d\n", written, (int)job_outputs.size());
            printf("  slice durations sum : %.1fs\n", got);
            printf("  original duration   : %.1fs\n", job_expect);
            double diff = got - job_expect; if (diff < 0) diff = -diff;
            double tol = job_expect * 0.01; if (tol < 1.0) tol = 1.0;
            if (diff <= tol) {
                printf("  ⇒ COMPLETE — the pieces reconstruct the whole input (within 1%%).\n");
                // ★ ONE FILE BACK (a held-release defect, 2026-10-12). The split above proved the work
                // was DIVIDED; this gives the user their video as ONE file again. The pieces stay on
                // disk as the evidence.
                vector<string> parts;
                for (auto &jo : job_outputs) parts.push_back(jo.second);
                string joined = work_outdir + "/" + stem_of(work) + ".joined.mp4";
                auto jr = join_slices(parts, joined);
                {
                    string hdr = "  ── ONE OUTPUT FILE ";
                    for (int i = 0; i < 52; i++) hdr += "─";
                    printf("%s\n", hdr.c_str());
                }
                if (jr.first) {
                    double jd = media_duration(joined);
                    struct stat jst;
                    long long jsz = (stat(joined.c_str(), &jst) == 0) ? (long long)jst.st_size : 0;
                    double jtol = job_expect * 0.01; if (jtol < 1.0) jtol = 1.0;
                    double jdiff = jd - job_expect; if (jdiff < 0) jdiff = -jdiff;
                    if (jd > 0.0 && jdiff <= jtol) {
                        printf("  %s  (%.1f MB, %.1fs)\n", base_of(joined).c_str(),
                               (double)jsz / 1048576.0, jd);
                        printf("  The whole input as one file, built from the %d slices above.\n",
                               (int)job_outputs.size());
                    } else {
                        printf("  %s was written but runs %.1fs against the input's %.1fs — keep the "
                               "slices; the join may have dropped the tail.\n",
                               base_of(joined).c_str(), jd > 0.0 ? jd : 0.0, job_expect);
                    }
                } else {
                    printf("  join FAILED — %s.\n", jr.second.c_str());
                    printf("  The %d slices above are intact; concatenate them yourself or re-run.\n",
                           (int)job_outputs.size());
                }
            }
            else printf("  ⇒ INCOMPLETE — %.1fs of %.1fs. The split LOST footage; do not report it as ok.\n",
                        job_expect - got, job_expect);
        }
    } else {
        // ★ DO NOT CLAIM A JOB THAT WAS NEVER STARTED (fix 2026-10-11, outside review). With --work
        // given but nothing able to run — --no-offload, no ffmpeg, or no card that can encode — this
        // printed "the job ran to completion" over an empty output block. Nothing had run.
        if (!work.empty()) {
            printf("\n  game exited (rc=%d)\n", rc);
            printf("  ── JOB OUTPUT ────────────────────────────────────────────────────────\n");
            printf("  NO JOB WAS STARTED — %s\n",
                   no_job_reason.empty() ? "nothing was given the work" : no_job_reason.c_str());
            printf("  Nothing was transcoded, so there is no output to check. The game ran normally.\n");
        } else if (want_load) {
            if (load_started)
                printf("\n  game exited (rc=%d) — the bounded test load ended with the session\n", rc);
            else
                // ★ DO NOT SAY "test load stopped" WHEN NONE STARTED ( blocking item 9).
                printf("\n  game exited (rc=%d) — NO TEST LOAD WAS STARTED (%s). The game ran normally.\n",
                       rc, no_load_reason.empty() ? "nothing was given the load" : no_load_reason.c_str());
        } else {
            printf("\n  game exited (rc=%d) — no work was running on the other cards\n", rc);
        }
    }
    // Real-valued, ever-so-slightly-positive wall delta, with the same 1e-6 floor as the Python.
    // The WITNESS panel divides by this; an integer clock would give a division by zero (nan).
    struct timespec t_end_ts;
    clock_gettime(CLOCK_MONOTONIC, &t_end_ts);
    double elapsed = (double)(t_end_ts.tv_sec - t_game_ts.tv_sec) +
                     (double)(t_end_ts.tv_nsec - t_game_ts.tv_nsec) / 1e9;
    if (elapsed < 1e-6) elapsed = 1e-6;

    // ★ A LAUNCHER THAT RETURNS AT ONCE IS SAID, NOT REPORTED AS A NORMAL SESSION (blocking item 2).
    // With the Steam client already running, `steam`/`steam://` hands off and exits in about a second:
    // the game starts unpinned, and the old code printed `game exited (rc=0)` as if the session had
    // been routed. It had not. Detect the fast clean exit and say plainly that the game went its own way.
    if (steam_launch && rc == 0 && elapsed < 3.0) {
        printf("\n  ★ steam returned after %.1fs (rc=0): it handed the launch to the already-running\n", elapsed);
        printf("    Steam client, so the game is starting on its own — WITHOUT this tool's pin. This\n");
        printf("    session did NOT route the game. Pin a Steam game with the launch-options recipe above.\n");
        fflush(stdout);
    }

    // ★ THE TAIL — what the OTHER cards actually did. A feature you cannot see is indistinguishable
    // from one that is broken, and the loads are silent by design (ffmpeg's output goes to /dev/null).
    // So say it plainly, per card, and say "alive for" rather than claiming work was done.
    if (!procs.empty() || !job_outputs.empty() || want_load || (work.empty() && !want_load)) {
        printf("\n  ── THE TAIL — what the other cards did ───────────────────────────────\n");
        if (work.empty() && !want_load) {
            // ★ SAY IDLE, DON'T SAY NOTHING (from the final outside read). With the default now doing
            // nothing, an empty tail would read as a broken tool. An explicit IDLE per card also shows
            // the skipped ones are still being accounted for.
            std::map<string, string> names;
            for (auto *g : spares) names[base_of(sfield(*g, "render_node"))] = gpu_name(*g);
            for (auto *g : spares) {
                string node = base_of(sfield(*g, "render_node"));
                if (node.empty()) continue;
                string nm = names.count(node) ? names[node] : "?";
                bool is_skipped = false;
                for (auto &s : skipped) if (s.node == node) is_skipped = true;
                if (is_skipped) {
                    string why = "not used";
                    for (auto &s : skipped) if (s.node == node) { why = s.why; break; }
                    printf("  %-12s %s SKIPPED — %s\n", node.c_str(), pad_display(nm, 22).c_str(),
                           why.c_str());
                } else {
                    printf("  %-12s %s IDLE  (no job given)\n", node.c_str(), pad_display(nm, 22).c_str());
                }
            }
        } else if (!job_outputs.empty()) {
            for (auto &jo : job_outputs) {
                struct stat st;
                bool ok = stat(jo.second.c_str(), &st) == 0;
                string nm = "?";
                for (auto *g : spares) if (base_of(sfield(*g, "render_node")) == jo.first) nm = gpu_name(*g);
                printf("  %-12s %s REAL JOB  %s\n", jo.first.c_str(), pad_display(nm, 22).c_str(),
                       ok ? jo.second.c_str() : "FAILED — no file");
            }
        } else if (want_load && !load_started) {
            // ★ NOTHING RAN, AND WE SAY SO ( blocking item 9). The old tail had no branch for
            // "a load was asked for and none started", so it printed nothing at all here.
            std::map<string, string> names;
            for (auto *g : spares) names[base_of(sfield(*g, "render_node"))] = gpu_name(*g);
            for (auto *g : spares) {
                string node = base_of(sfield(*g, "render_node"));
                if (node.empty()) continue;
                string nm = names.count(node) ? names[node] : "?";
                bool is_enc = false;
                for (auto &e : encoders) if (e.first == node) is_enc = true;
                if (is_enc) {
                    printf("  %-12s %s NOT DRIVEN — %s\n", node.c_str(), pad_display(nm, 22).c_str(),
                           no_load_reason.empty() ? "no load was started" : no_load_reason.c_str());
                } else {
                    string why = "not used";
                    for (auto &s : skipped) if (s.node == node) { why = s.why; break; }
                    printf("  %-12s %s SKIPPED — %s\n", node.c_str(), pad_display(nm, 22).c_str(),
                           why.c_str());
                }
            }
        } else {
            std::map<string, string> names;
            for (auto *g : spares) names[base_of(sfield(*g, "render_node"))] = gpu_name(*g);
            for (auto *g : spares) {
                string node = base_of(sfield(*g, "render_node"));
                if (node.empty()) continue;
                string nm = names.count(node) ? names[node] : "?";
                bool is_enc = false;
                for (auto &e : encoders) if (e.first == node) is_enc = true;
                if (is_enc) {
                    auto it = died_early.find(node);
                    int cap = test_load ? *test_load : 0;
                    if (it == died_early.end()) {
                        printf("  %-12s %s SYNTHETIC TEST LOAD — still running when the game exited\n",
                               node.c_str(), pad_display(nm, 22).c_str());
                        printf("  %-12s %-22s   (%.0f s of session; bounded to %ds by the wall clock)\n",
                               "", "", elapsed, cap);
                    } else if (it->second == 0) {
                        // ★ NORMAL COMPLETION IS NOT A FAULT ( blocking item 9). The load is
                        // DESIGNED to run for `cap` seconds and then stop; a clean rc 0 means it did
                        // exactly that. It used to be reported as "EXITED EARLY ... it did NOT run".
                        printf("  %-12s %s SYNTHETIC TEST LOAD — COMPLETED its %ds bounded run, as designed\n",
                               node.c_str(), pad_display(nm, 22).c_str(), cap);
                    } else {
                        printf("  %-12s %s SYNTHETIC TEST LOAD — DIED (rc=%d) before its %ds run finished\n",
                               node.c_str(), pad_display(nm, 22).c_str(), it->second, cap);
                    }
                    printf("  %-12s %-22s produced nothing (a job needs --work FILE)\n", "", "");
                } else {
                    string why = "not used";
                    for (auto &s : skipped) if (s.node == node) { why = s.why; break; }
                    printf("  %-12s %s SKIPPED — %s\n", node.c_str(), pad_display(nm, 22).c_str(),
                           why.c_str());
                }
            }
        }
    }

    if (!fg_count_path.empty()) {
        long long n = read_counter(fg_count_path);
        printf("\n  ── WITNESS ─────────────────────────────────────────────────────────\n");
        printf("  images presented to the compositor : %lld\n", n);
        printf("  wall time                          : %.1f s\n", elapsed);
        printf("  presented rate                     : %.1f /s\n", (double)n / elapsed);
        printf("  ★ This is the number frame generation moves, and the ONLY number it moves.\n");
        printf("    The game's own FPS counter cannot see it — that counts frames the GAME\n");
        printf("    rendered, and with frame gen on the game may now be doing LESS work, because it\n");
        printf("    is paying for the interpolation. On one title here that figure fell about 9%%;\n");
        printf("    on another it did not move at all. Either way it cannot see what was added —\n");
        printf("    which is the whole reason this counter exists.\n");
        printf("    Honest limit: an image handed to the compositor is not proof it was scanned\n");
        printf("    out, and no counter can tell you whether the interpolated frames look right.\n");
    }
    return rc == 0 ? 0 : rc;
}

// ─────────────────────────────────────────────────────────────────────────────
// help / arg parsing
// ─────────────────────────────────────────────────────────────────────────────
// ★ `play` is hand-parsed (it has to pass an arbitrary game command through untouched), so argparse
// never sees `play --help` — the flag used to fall through to `rest` and be EXECUTED as the game.
// Printed here, byte-for-byte, so all three builds can share it.
static const char *PLAY_USAGE = R"PLAY(usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] [--work FILE [--work-outdir DIR] [--split]] [--spare NODE,...] -- <game command...>

Play a game on the card your monitor is plugged into. The other cards do nothing unless you ask
for it with --work (a real transcode) or --test-load (a bounded synthetic check).

options:
  -h, --help            show this help message and exit
  --no-offload          run the game alone; no other card does work
  --scale F             render at F of native and let gamescope upscale it (0.3-1.0)
  --framegen N          frame generation at Nx, via lsfg-vk (2, 3 or 4)
  --overlay             show MangoHud over the game, via mangoapp
  --test-load [SECS]    run a bounded synthetic load on the spare cards (default 60s, max 600) — it
                        proves they can be driven and writes no file; nothing runs without this
  --work FILE           transcode FILE on the spare cards while you play (a real job)
  --work-outdir DIR     where a job's output goes (default: beside FILE)
  --split               divide ONE job across the spare cards instead of one job each
  --spare NODE,...      only use these render nodes, e.g. renderD130,renderD131

  A job is not stopped with the game. Every output is checked on disk afterwards: a
  half-finished transcode reported as done is the failure this tool exists not to make.)PLAY";

// ★ `ui` — the local page. ONE Python file, shared by all three builds rather than three
// reimplementations of an HTTP server; this build finds it and hands off. Same lookup order and the
// same failure line in every build, so `spirit ui` behaves identically whoever compiled what.
static const char *UI_USAGE = R"UI(usage: spirit ui [-h] [--port PORT] [--no-open] [--print-url]

Open a local page for this tool: your cards and what each one is for, whether
upscaling and frame generation are available here, and a box to launch your game.

It listens on 127.0.0.1 only, so nothing outside this machine can reach it.

options:
  -h, --help        show this help message and exit
  --port PORT       serve on this port instead of a free one
  --no-open         do not open a browser
  --print-url       print the URL and keep running)UI";

// Beside the running program, then up to four directories above it (a C++ binary in `cpp/` or a Rust
// one in `rust/target/release/`), then where install.sh puts it. Mirrors spirit.py's `_ui_path`.
static std::optional<string> ui_path() {
    vector<string> cands;
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        string self(buf);
        size_t sl = self.find_last_of('/');
        if (sl != string::npos) {
            string d = self.substr(0, sl);
            cands.push_back(d + "/spirit_ui.py");
            for (int i = 0; i < 4; i++) {
                size_t p = d.find_last_of('/');
                if (p == string::npos || p == 0) break;
                d = d.substr(0, p);
                cands.push_back(d + "/spirit_ui.py");
            }
        }
    }
    if (const char *h = getenv("HOME"))
        cands.push_back(string(h) + "/.local/share/spirit/spirit_ui.py");
    struct stat sb;
    for (const string &c : cands)
        if (stat(c.c_str(), &sb) == 0 && S_ISREG(sb.st_mode)) return c;
    return std::nullopt;
}

static int ui_cmd(const vector<string> &args) {
    for (const string &a : args)
        if (a == "-h" || a == "--help") { printf("%s\n", UI_USAGE); return 0; }
    std::optional<string> p = ui_path();
    if (!p) {
        fprintf(stderr, "spirit ui: spirit_ui.py was not found. Run install.sh, or put spirit_ui.py "
                        "beside spirit.py.\n");
        return 1;
    }
    vector<string> av;
    av.push_back("python3");
    av.push_back(*p);
    for (const string &a : args) av.push_back(a);
    return spawn_wait(av, current_env(), false);
}

static void print_help() {
    printf("usage: spirit [-h] [--version] {probe,roles,plan,doctor,origin,boost,fg} ...\n\n");
    printf("Spiritual Successor — put every GPU to work\n");
    printf("commands: probe · roles · plan · doctor · fg · boost · enable · disable · run · play · ui  (`run` and `play` take a command after `--`)\n\n");
    printf("positional arguments:\n");
    printf("  {probe,roles,plan,doctor,origin,boost,fg}\n\n");
    printf("options:\n");
    printf("  -h, --help            show this help message and exit\n");
    printf("  --version             show program's version number and exit\n");
}

// argparse-style subcommand usage lines (byte-for-byte from the Python original).
static string sub_usage(const string &cmd) {
    if (cmd == "probe")  return "usage: spirit probe [-h] [--json]";
    if (cmd == "roles")  return "usage: spirit roles [-h] [--profile PROFILE] [--json]";
    if (cmd == "plan")   return "usage: spirit plan [-h] [--workload WORKLOAD] [--vram-mib VRAM_MIB]";
    if (cmd == "doctor") return "usage: spirit doctor [-h]";
    if (cmd == "origin") return "usage: spirit origin [-h] [--check]";
    if (cmd == "boost")  return "usage: spirit boost [-h]";
    if (cmd == "fg")     return "usage: spirit fg [-h] [--res RES] [--mode {present-on-secondary,round-trip}]";
    return "usage: spirit [-h] [--version] {probe,roles,plan,doctor,origin,boost,fg} ...";
}

static void print_sub_help(const string &cmd) {
    printf("%s\n\n", sub_usage(cmd).c_str());
    printf("options:\n");
    if (cmd == "probe") {
        printf("  -h, --help  show this help message and exit\n");
        printf("  --json\n");
    } else if (cmd == "roles") {
        printf("  -h, --help         show this help message and exit\n");
        printf("  --profile PROFILE\n");
        printf("  --json\n");
    } else if (cmd == "plan") {
        printf("  -h, --help           show this help message and exit\n");
        printf("  --workload WORKLOAD\n");
        printf("  --vram-mib VRAM_MIB  VRAM the workload needs on its card\n");
    } else if (cmd == "doctor") {
        printf("  -h, --help  show this help message and exit\n");
    } else if (cmd == "origin") {
        // ★ argparse help for `origin`, including the --check line (C10): the port used to print only
        // -h/--help, so `spirit origin -h` was a different document from the Python's.
        printf("  -h, --help  show this help message and exit\n");
        printf("  --check     exit 0 if this build is the original, 1 if not\n");
    } else if (cmd == "boost") {
        printf("  -h, --help  show this help message and exit\n");
    } else if (cmd == "fg") {
        printf("  -h, --help            show this help message and exit\n");
        printf("  --res RES             resolution@refresh, e.g. 1440p60 / 4k120 / 1080p144\n");
        printf("  --mode {present-on-secondary,round-trip}\n");
        printf("                        one-way (display on the frame-gen card) or round trip\n");
    }
}

static int arg_error(const string &cmd, const string &msg) {
    fprintf(stderr, "%s\n", sub_usage(cmd).c_str());
    fprintf(stderr, "spirit %s: error: %s\n", cmd.c_str(), msg.c_str());
    return 2;
}
// ★ argparse REPORTS EVERY ARGUMENT IT COULD NOT PLACE, NOT JUST THE FIRST (fix 2026-10-11, outside
// review). `roles --profil edit` yields "unrecognized arguments: --profil edit", because neither token
// was claimed; this build named only `--profil`. Found by a harness case that only became possible once
// the Python reference stopped accepting abbreviations. `flags` are the bare options this subcommand
// knows, `valued` the ones that consume the next token; anything else is reported, in order.
static string unrec_extras(const vector<string> &args, const string &flags, const string &valued) {
    auto in = [](const string &csv, const string &tok) {
        size_t a = 0;
        while (a <= csv.size()) {
            size_t b = csv.find(',', a);
            if (b == string::npos) b = csv.size();
            if (b > a && csv.compare(a, b - a, tok) == 0) return true;
            a = b + 1;
        }
        return false;
    };
    vector<string> out;
    for (size_t k = 1; k < args.size(); k++) {
        if (in(flags, args[k])) continue;
        if (in(valued, args[k])) { k++; continue; }
        out.push_back(args[k]);
    }
    return join(out, " ");
}

// argparse raises "unrecognized arguments" on the TOP-LEVEL parser.
static int unrec_error(const string &msg) {
    fprintf(stderr, "usage: spirit [-h] [--version] {probe,roles,plan,doctor,origin,boost,fg} ...\n");
    fprintf(stderr, "spirit: error: %s\n", msg.c_str());
    return 2;
}

// Python int() semantics for argparse `type=int`: whitespace-trimmed, base-10, signs allowed and a
// single underscore BETWEEN digits accepted (int("1_000") == 1000, int("_1") is a ValueError).
static bool parse_py_int(const string &s, long long &out) {
    string t = trim(s);
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[i] == '+' || t[i] == '-') { neg = (t[i] == '-'); i++; }
    if (i >= t.size()) return false;
    string digits;
    bool prev_digit = false;
    for (; i < t.size(); i++) {
        char c = t[i];
        if (c >= '0' && c <= '9') { digits.push_back(c); prev_digit = true; }
        else if (c == '_') {
            if (!prev_digit || i + 1 >= t.size() || t[i + 1] < '0' || t[i + 1] > '9') return false;
            prev_digit = false;
        } else return false;
    }
    if (digits.empty()) return false;
    size_t idx = 0;
    try {
        long long v = std::stoll((neg ? "-" : "") + digits, &idx, 10);
        if (idx != digits.size() + (neg ? 1 : 0)) return false;
        out = v;
    } catch (...) { return false; }
    return true;
}

// Python float() semantics: whitespace-trimmed, full-string, decimal/exp/inf/nan (not hex).
static bool parse_py_float(const string &s, double &out) {
    string t = trim(s);
    if (t.empty()) return false;
    size_t k = (t[0] == '+' || t[0] == '-') ? 1 : 0;
    if (k + 1 < t.size() && t[k] == '0' && (t[k + 1] == 'x' || t[k + 1] == 'X')) return false;
    if (t.find('(') != string::npos) return false; // strtod "nan(...)" forms
    errno = 0;
    const char *c = t.c_str();
    char *end = nullptr;
    double v = std::strtod(c, &end);
    if (end == c || *end != '\0') return false;
    out = v;
    return true;
}

// ── MAKE IT STAY ON: a session-wide pin, on until you turn it off ─────────────────────
// Mirrors spirit.py's enable/disable. One small file in the user's own config directory; every
// process in the session inherits it, so Steam and the games it starts are pinned with no tool in
// the loop. `probe` reads sysfs, so the pin never blinds the tool's own view of the machine.
static string env_d_file() { return home_dir() + "/.config/environment.d/90-spirit.conf"; }

static void mkdir_p(const string &d) {
    string cur;
    for (size_t i = 0; i < d.size(); i++) {
        cur += d[i];
        if (d[i] == '/' || i + 1 == d.size())
            if (cur != "/") mkdir(cur.c_str(), 0755);
    }
}

static const char *W_SEL = "MESA_VK_DEVICE_SELECT";
static const char *W_PCI = "DRI_PRIME";
static const char *W_FSR = "PROTON_FSR4_UPGRADE";

// The systemd user manager's live environment as text, or "" when there is no manager to ask.
static string manager_env_text() {
    if (!which("systemctl")) return "";
    ProcResult r = run_capture_both({"systemctl", "--user", "show-environment"}, nullptr, 10);
    if (!r.ok || r.rc != 0) return "";
    return r.out;
}

// True when the manager's environment carries exactly K=V on a line of its own.
static bool env_text_has(const string &txt, const string &kv) {
    size_t i = 0;
    while (i <= txt.size()) {
        size_t j = txt.find('\n', i);
        if (j == string::npos) j = txt.size();
        if (txt.compare(i, j - i, kv) == 0) return true;
        if (j >= txt.size()) break;
        i = j + 1;
    }
    return false;
}

static string apply_session(const string &sel, const string &pci) {
    if (!which("systemctl"))
        return "no systemd user manager here — this machine cannot be armed live";
    vector<string> a = {"systemctl", "--user", "set-environment",
                        string(W_SEL) + "=" + sel, string(W_PCI) + "=" + pci,
                        string(W_FSR) + "=1"};
    ProcResult r = run_capture_both(a, nullptr, 10);
    if (!r.ok) return "could not reach the session manager — NOT armed";
    if (r.rc != 0) return "the session manager refused it — NOT armed";
    return "armed for apps started from now on";
}

// Disarm — but ONLY values that are ours. A value this tool did not write is left alone.
static string unapply_session(const string &sel, const string &pci) {
    if (!which("systemctl")) return "no systemd user manager here — nothing to disarm";
    string txt = manager_env_text();
    vector<string> ours;
    if (env_text_has(txt, string(W_SEL) + "=" + sel)) ours.push_back(W_SEL);
    if (env_text_has(txt, string(W_PCI) + "=" + pci)) ours.push_back(W_PCI);
    if (env_text_has(txt, string(W_FSR) + "=1")) ours.push_back(W_FSR);
    if (ours.empty()) return "the session carries no pin this tool set — left everything alone";
    vector<string> a = {"systemctl", "--user", "unset-environment"};
    for (auto &o : ours) a.push_back(o);
    ProcResult r = run_capture_both(a, nullptr, 10);
    if (!r.ok) return "could not reach the session manager — still armed";
    string joined;
    for (size_t k = 0; k < ours.size(); k++) { if (k) joined += ", "; joined += ours[k]; }
    return "disarmed " + joined;
}

static int enable_cmd(const J &db, bool persist) {
    if (getenv("SPIRIT_DEVICES")) {
        // ★ A FIXTURE MUST NOT BECOME A REAL PIN. Same class as the probe's refusal to read the real
        // box on a typo'd fixture path.
        fprintf(stderr, "spirit enable: SPIRIT_DEVICES is set, so the cards come from a FIXTURE — "
                        "refusing to write a pin derived from it into your real session.\n");
        return 2;
    }
    const J *mc = main_card(db);
    if (!mc) {
        fprintf(stderr, "spirit enable: no GPU found on this machine — nothing to pin.\n");
        return 1;
    }
    string vendor = sfield(*mc, "vendor"), device = sfield(*mc, "device");
    string sel = (vendor.size() >= 4 ? vendor.substr(2) : vendor) + ":" +
                 (device.size() >= 4 ? device.substr(2) : device) + "!";
    string pci = dri_prime(sfield(*mc, "bdf"));
    printf("  pin the launch to      : %s (%s)\n", gpu_name(*mc).c_str(), sfield(*mc, "card").c_str());
    printf("  pin                    : %s=%s   %s=%s\n", W_SEL, sel.c_str(), W_PCI, pci.c_str());
    printf("  this session           : %s\n", apply_session(sel, pci).c_str());
    printf("  fps lever              : %s=1 (FSR 4 in-game — raises the BASE rate)\n", W_FSR);
    if (persist) {
        mkdir_p(home_dir() + "/.config/environment.d");
        string tmp = env_d_file() + ".tmp";
        FILE *f = fopen(tmp.c_str(), "w");
        if (f) {
            fprintf(f, "# Spiritual Successor — session-wide GPU pin, written by `spirit enable --persist`.\n");
            fprintf(f, "# Every app started after your next login inherits this, Steam included.\n");
            fprintf(f, "# Remove it with:  spirit disable\n");
            fprintf(f, "%s=%s\n%s=%s\n%s=1\n", W_SEL, sel.c_str(), W_PCI, pci.c_str(), W_FSR);
            if (fclose(f) != 0) f = nullptr;
        }
        if (!f || rename(tmp.c_str(), env_d_file().c_str()) != 0) {
            fprintf(stderr, "spirit enable: could not write %s\n", env_d_file().c_str());
            return 1;
        }
        printf("  future logins          : %s (removed by `spirit disable`)\n", env_d_file().c_str());
    }
    printf("\n");
    if (bfield(*mc, "integrated")) {
        printf("  ★ the card driving your display is the INTEGRATED one, so this pins games to the\n");
        printf("    weaker chip. On a laptop that is usually how the panel is wired, not a mistake —\n");
        printf("    check `spirit roles` before you rely on it.\n");
    }
    printf("  A game you start from now on inherits the pin. A Steam that was ALREADY RUNNING does not\n");
    printf("  — its environment was fixed when it started, so restart Steam, or pin one game with the\n");
    printf("  launch-options recipe. Nothing here survives a reboot unless you pass --persist; that is\n");
    printf("  the default on purpose, so pulling the power always lands you back in a safe state.\n");
    printf("  For frames rather than looks: FSR 4 raises the base rate and frame generation multiplies\n");
    printf("  it — size the lsfg multiplier so base x N sits near your panel's refresh (2x from ~80,\n");
    printf("  3x from ~55). Measured here at 1440p: 2x costs 0.71 ms, 3x 1.18, 4x 1.66 — never the binder.\n");
    return 0;
}

static int disable_cmd(const J &db) {
    const J *mc = main_card(db);
    if (!mc) {
        printf("  this session           : no GPU found — nothing to disarm\n");
    } else {
        string vendor = sfield(*mc, "vendor"), device = sfield(*mc, "device");
        string sel = (vendor.size() >= 4 ? vendor.substr(2) : vendor) + ":" +
                     (device.size() >= 4 ? device.substr(2) : device) + "!";
        string pci = dri_prime(sfield(*mc, "bdf"));
        printf("  this session           : %s\n", unapply_session(sel, pci).c_str());
    }
    struct stat sb;
    if (stat(env_d_file().c_str(), &sb) == 0) {
        if (unlink(env_d_file().c_str()) != 0) {
            fprintf(stderr, "spirit disable: could not remove %s\n", env_d_file().c_str());
            return 1;
        }
        printf("  future logins          : removed %s\n", env_d_file().c_str());
    } else {
        printf("  future logins          : no persisted pin\n");
    }
    printf("\n");
    printf("  `spirit play` still pins whatever it launches. This only disarms the launches the tool\n");
    printf("  was not in the loop for, and only values it set itself.\n");
    return 0;
}

int main(int argc, char **argv) {
    vector<string> args(argv + 1, argv + argc);

    // ★ ONLY OUR OWN FLAGS COUNT, NOT THE GAME'S (fix 2026-10-11, found by the final outside read).
    // These checks used to scan the WHOLE argv, so `spirit play -- ./game --version` printed the version
    // and never launched the game, and any game invoked with a `--json` flag silently lost the banner.
    // A flag only means something to us if it is the first token, or if it appears before the `--` that
    // separates our arguments from the game's.
    vector<string> ours = args;
    for (size_t i = 0; i < args.size(); i++)
        if (args[i] == "--") { ours.assign(args.begin(), args.begin() + i); break; }
    auto in_ours = [&](const string &t) {
        for (const string &a : ours) if (a == t) return true;
        return false;
    };
    bool suppress_mark = in_ours("origin")
                         || (!args.empty() && args[0] == "--version")
                         || (!args.empty() && (args[0] == "probe" || args[0] == "roles") && in_ours("--json"));
    if (!suppress_mark) origin_cmd(false);
    // ★ --version prints THE one version string and nothing else: a single line, exit 0, no origin
    // mark and no help footer. Handled ahead of every other branch so it matches the Python exactly.
    if (!args.empty() && args[0] == "--version") { printf("spirit %s\n", VERSION); return 0; }

    if (!args.empty() && args[0] == "ui") {
        vector<string> rest(args.begin() + 1, args.end());
        return ui_cmd(rest);
    }

    // `enable` / `disable` — parsed by HAND, like play/run/ui, so they stay OUT of the top-level
    // help's subcommand list, which the three builds must print identically.
    if (!args.empty() && (args[0] == "enable" || args[0] == "disable")) {
        bool persist = false, bad = false;
        for (size_t k = 1; k < args.size(); k++) {
            if (args[0] == "enable" && args[k] == "--persist") persist = true;
            else bad = true;
        }
        if (bad) {
            fprintf(stderr, args[0] == "enable" ? "usage: spirit enable [--persist]\n"
                                                : "usage: spirit disable\n");
            return 2;
        }
        if (args[0] == "disable") return disable_cmd(probe());
        return enable_cmd(probe(), persist);
    }

    // `run` parsed by hand (arbitrary command passthrough)
    if (!args.empty() && args[0] == "run") {
        string role, card;
        vector<string> rest;
        size_t i = 1;
        while (i < args.size()) {
            if (args[i] == "--role" && i + 1 < args.size()) { role = args[i + 1]; i += 2; }
            else if (args[i] == "--card" && i + 1 < args.size()) { card = args[i + 1]; i += 2; }
            else if (args[i] == "--") { rest.assign(args.begin() + i + 1, args.end()); break; }
            else { rest.assign(args.begin() + i, args.end()); break; }
        }
        if (role.empty() || card.empty()) {
            fprintf(stderr, "usage: spirit run --role <role> --card <card> [-- <cmd>...]\n");
            return 1;
        }
        return run_cmd(probe(), role, card, rest);
    }

    // `play` parsed by hand
    if (!args.empty() && args[0] == "play") {
        bool offload = true;
        bool overlay = false;
        vector<string> rest;
        size_t i = 1;
        std::optional<double> scale;
        std::optional<int> framegen;
        string work, work_outdir, only_spares;
        bool split = false;
        std::optional<int> test_load;
        while (i < args.size()) {
            if (args[i] == "--no-offload") { offload = false; i += 1; }
            else if (args[i] == "--spare") {
                if (i + 1 >= args.size()) {
                    fprintf(stderr, "spirit play: --spare needs render nodes, e.g. --spare renderD130,renderD131\n");
                    return 2;
                }
                only_spares = args[i + 1]; i += 2;
            }
            else if (args[i] == "--split") { split = true; i += 1; }
            else if (args[i] == "--work") {
                // REAL WORK, not the synthetic test load. The input must exist.
                if (i + 1 >= args.size()) {
                    fprintf(stderr, "spirit play: --work needs a file, e.g. --work capture.mkv\n");
                    return 2;
                }
                work = args[i + 1]; i += 2;
            }
            else if (args[i] == "--work-outdir") {
                if (i + 1 >= args.size()) {
                    fprintf(stderr, "spirit play: --work-outdir needs a directory\n");
                    return 2;
                }
                work_outdir = args[i + 1]; i += 2;
            }
            else if (args[i] == "--scale") {
                double v;
                if (i + 1 >= args.size() || !parse_py_float(args[i + 1], v)) {
                    fprintf(stderr, "spirit play: --scale needs a number, e.g. --scale 0.7\n");
                    return 2;
                }
                scale = v; i += 2;
            }
            else if (args[i] == "--framegen") {
                long long v;
                if (i + 1 >= args.size() || !parse_py_int(args[i + 1], v)) {
                    fprintf(stderr, "spirit play: --framegen needs 2, 3 or 4\n");
                    return 2;
                }
                framegen = (int)v; i += 2;
            }
            else if (args[i] == "--overlay") { overlay = true; i += 1; }
            else if (args[i] == "--test-load" || starts_with(args[i], "--test-load=")) {
                // ★ OPT-IN, AND BOUNDED (from the final outside read). The synthetic load used to be the
                // default and ran until the game exited; it is now something you ask for, for a stated
                // number of seconds, and it stops.
                string raw;
                if (starts_with(args[i], "--test-load=")) { raw = args[i].substr(12); i += 1; }
                else {
                    bool digits = (i + 1 < args.size() && !args[i + 1].empty());
                    for (size_t z = 0; digits && z < args[i + 1].size(); z++)
                        if (args[i + 1][z] < '0' || args[i + 1][z] > '9') digits = false;
                    if (digits) { raw = args[i + 1]; i += 2; }
                    else { raw = "60"; i += 1; }
                }
                long long v;
                if (!parse_py_int(raw, v)) {
                    fprintf(stderr, "spirit play: --test-load needs seconds, e.g. --test-load 30\n");
                    return 2;
                }
                if (v < 1) v = 1;
                if (v > 600) v = 600;
                test_load = (int)v;
            }
            else if (args[i] == "-h" || args[i] == "--help") {
                // ★ Without this, `play --help` fell through to `rest` and was executed as the game.
                printf("%s\n", PLAY_USAGE);
                return 0;
            }
            else if (args[i] == "--") { rest.assign(args.begin() + i + 1, args.end()); break; }
            else { rest.assign(args.begin() + i, args.end()); break; }
        }
        if (scale && !(0.3 <= *scale && *scale <= 1.0)) {
            fprintf(stderr, "spirit play: --scale must be between 0.3 and 1.0\n");
            return 2;
        }
        if (framegen && *framegen != 2 && *framegen != 3 && *framegen != 4) {
            fprintf(stderr, "spirit play: --framegen must be 2, 3 or 4\n");
            return 2;
        }
        return play_cmd(probe(), rest, offload, scale, framegen, overlay, work, work_outdir, split,
                        only_spares, test_load);
    }

    if (args.empty()) { print_help(); return 1; }
    if (args[0] == "-h" || args[0] == "--help") { print_help(); return 0; }

    string cmd = args[0];
    if (cmd != "probe" && cmd != "roles" && cmd != "plan" && cmd != "doctor" && cmd != "origin" &&
        cmd != "boost" && cmd != "fg") {
        fprintf(stderr, "usage: spirit [-h] [--version] {probe,roles,plan,doctor,origin,boost,fg} ...\n");
        fprintf(stderr,
                "spirit: error: argument cmd: invalid choice: '%s' (choose from 'probe', 'roles', 'plan', 'doctor', 'origin', 'boost', 'fg')\n",
                cmd.c_str());
        return 2;
    }

    // (the origin mark is printed at the top of main, exactly as the Python does.)

    // global -h / --help inside a subcommand: argparse prints that subcommand's help
    for (size_t k = 1; k < args.size(); k++)
        if (args[k] == "-h" || args[k] == "--help") { print_sub_help(cmd); return 0; }

    J db = probe();

    if (cmd == "probe") {
        bool json = false;
        for (size_t k = 1; k < args.size(); k++) {
            if (args[k] == "--json") json = true;
            else return unrec_error("unrecognized arguments: " + unrec_extras(args, "--json", ""));
        }
        if (json) printf("%s\n", json_dumps_indent2(db).c_str());
        else print_table(db);
        return 0;
    }
    if (cmd == "roles") {
        bool json = false;
        string profile = "game";
        for (size_t k = 1; k < args.size();) {
            if (args[k] == "--json") { json = true; k++; continue; }
            if (args[k] == "--profile") {
                if (k + 1 >= args.size())
                    return arg_error(cmd, "argument --profile: expected one argument");
                profile = args[k + 1]; k += 2; continue;
            }
            if (starts_with(args[k], "--profile=")) { profile = args[k].substr(10); k++; continue; }
            return unrec_error("unrecognized arguments: " + unrec_extras(args, "--json", "--profile"));
        }
        if (json) printf("%s\n", json_dumps_indent2(assign_roles(db, profile)).c_str());
        else print_roles(db, profile);
        return 0;
    }
    if (cmd == "plan") {
        string workload = "1440p60-framegen";
        std::optional<long long> vram;
        for (size_t k = 1; k < args.size();) {
            if (args[k] == "--workload") {
                if (k + 1 >= args.size())
                    return arg_error(cmd, "argument --workload: expected one argument");
                workload = args[k + 1]; k += 2; continue;
            }
            if (starts_with(args[k], "--workload=")) { workload = args[k].substr(11); k++; continue; }
            if (args[k] == "--vram-mib") {
                if (k + 1 >= args.size())
                    return arg_error(cmd, "argument --vram-mib: expected one argument");
                long long v;
                if (!parse_py_int(args[k + 1], v))
                    return arg_error(cmd, "argument --vram-mib: invalid int value: '" + args[k + 1] + "'");
                vram = v; k += 2; continue;
            }
            if (starts_with(args[k], "--vram-mib=")) {
                string val = args[k].substr(11);
                long long v;
                if (!parse_py_int(val, v))
                    return arg_error(cmd, "argument --vram-mib: invalid int value: '" + val + "'");
                vram = v; k++; continue;
            }
            return unrec_error("unrecognized arguments: " + unrec_extras(args, "", "--workload,--vram-mib"));
        }
        return plan_cmd(db, workload, vram);
    }
    if (cmd == "doctor") {
        for (size_t k = 1; k < args.size(); k++)
            return unrec_error("unrecognized arguments: " + unrec_extras(args, "", ""));
        return doctor(db, false);
    }
    if (cmd == "origin") {
        bool check = false;
        for (size_t k = 1; k < args.size(); k++) {
            if (args[k] == "--check") { check = true; continue; }
            return unrec_error("unrecognized arguments: " + unrec_extras(args, "--check", ""));
        }
        return origin_cmd(check);
    }
    if (cmd == "boost") {
        for (size_t k = 1; k < args.size(); k++)
            return unrec_error("unrecognized arguments: " + unrec_extras(args, "", ""));
        return boost_cmd(db);
    }
    if (cmd == "fg") {
        string res = "1440p60", mode = "present-on-secondary";
        for (size_t k = 1; k < args.size();) {
            if (args[k] == "--res") {
                if (k + 1 >= args.size())
                    return arg_error(cmd, "argument --res: expected one argument");
                res = args[k + 1]; k += 2; continue;
            }
            if (starts_with(args[k], "--res=")) { res = args[k].substr(6); k++; continue; }
            if (args[k] == "--mode" || starts_with(args[k], "--mode=")) {
                string val;
                if (args[k] == "--mode") {
                    if (k + 1 >= args.size())
                        return arg_error(cmd, "argument --mode: expected one argument");
                    val = args[k + 1]; k += 2;
                } else { val = args[k].substr(7); k++; }
                if (val != "present-on-secondary" && val != "round-trip")
                    return arg_error(cmd,
                                     "argument --mode: invalid choice: '" + val +
                                         "' (choose from 'present-on-secondary', 'round-trip')");
                mode = val; continue;
            }
            return unrec_error("unrecognized arguments: " + unrec_extras(args, "", "--res,--mode"));
        }
        return fg_cmd(db, res, mode);
    }

    print_help();
    return 1;
}
