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
static string rstrip_char(const string &s, char c) {
    size_t b = s.size();
    while (b > 0 && s[b - 1] == c) b--;
    return s.substr(0, b);
}
static string join(const vector<string> &v, const string &sep) {
    string out;
    for (size_t i = 0; i < v.size(); i++) { if (i) out += sep; out += v[i]; }
    return out;
}
static bool starts_with(const string &s, const string &p) { return s.rfind(p, 0) == 0; }

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

// os.path.expanduser("~")
static string home_dir() {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return pw->pw_dir;
    return "";
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

// spawn and wait; returns exit code (negative for signal), like subprocess.call
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
        struct stat st;
        if (stat(fake, &st) == 0) {
            FILE *f = fopen(fake, "r");
            if (f) {
                string data;
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
                fclose(f);
                try {
                    J db = JsonParser(data).parse();
                    if (!jget(db, "gpufan")) db.put("gpufan", J::str("probe/0.1"));
                    return db;
                } catch (...) {
                    // fall through to a real probe on parse failure
                }
            }
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
    out.put("gpufan", J::str("probe/0.1"));
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
            string nm = sfield(g, "name");
            if (nm.size() > 30) nm = nm.substr(0, 30);
            printf("%-6s %-13s %-30s %-9s %-9s %-8s %-6s %s\n",
                   sfield(g, "card").c_str(), sfield(g, "bdf").c_str(), nm.c_str(),
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
        string nm = sfield(r, "name");
        if (nm.size() > 26) nm = nm.substr(0, 26);
        auto vt = ifield(r, "vram_total_mib");
        string vr = (vt && *vt) ? (std::to_string(*vt) + "M") : "-";
        vector<string> rl;
        const J *rls = jget(r, "roles");
        if (rls) for (auto &x : rls->arr) rl.push_back(x.s);
        auto gm = ffield(r, "link_gbps_max");
        string linkcol = gm ? py_float_repr(*gm) : "-";
        printf("%-6s %-26s %-9s %-8s %s\n", sfield(r, "card").c_str(), nm.c_str(), linkcol.c_str(),
               vr.c_str(), join(rl, ", ").c_str());
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
        printf("  VRAM asked: %d MiB (measured on this box: a light load costs ~800 MiB, a GPU-bound"
               " 4K load ~8,200 MiB — ~10x)\n\n",
               (int)*vram_mib);
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
static const std::map<string, double> HANDOFF_GBPS = {{"card0", 0.76}, {"card2", 0.25}, {"card3", 2.91}};
static const std::map<string, double> FRAME_MIB = {{"1080p", 8.29}, {"1440p", 14.75}, {"4k", 33.18}};

static int fg_cmd(const J &db, const string &res_in, const string &mode) {
    string r, hz;
    size_t at = res_in.rfind('@');
    if (at != string::npos) { r = res_in.substr(0, at); hz = res_in.substr(at + 1); }
    else {
        if (res_in.size() >= 2) { r = res_in.substr(0, res_in.size() - 2); hz = res_in.substr(res_in.size() - 2); }
        else { r = ""; hz = res_in; }
    }
    r = rstrip_char(r, '@');
    string res = FRAME_MIB.count(r) ? r : "1440p";
    int hznum;
    try { hznum = std::stoi(hz); } catch (...) { hznum = 60; }
    double payload = FRAME_MIB.at(res);
    double budget = 1000.0 / hznum;
    bool one_way = (mode == "present-on-secondary");

    printf("spirit fg — can a card carry the frame-gen/upscale pass for %s@%d?\n\n", res.c_str(), hznum);
    printf("  frame payload: %.2f MiB    frame budget: %.2f ms\n\n", payload, budget);
    printf("  %-8s %-12s %-14s %-12s %s\n", "card", "handoff", "one-way ms", "round-trip ms", "verdict");
    printf("  %s\n", string(84, '-').c_str());

    const J *ga = gpus_arr(db);
    if (ga) for (auto &g : ga->arr) {
        string c = sfield(g, "card");
        auto it = HANDOFF_GBPS.find(c);
        if (it == HANDOFF_GBPS.end()) {
            printf("  %-8s %-12s %-14s %-12s %s\n", c.c_str(), "n/a", "-", "-",
                   "not a card pair here / not measured");
            continue;
        }
        double rate = it->second;
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
        snprintf(hb, sizeof(hb), "%.2f GB/s", rate);
        printf("  %-8s %-12s %-14.2f %-12.2f %s (%s)\n", c.c_str(), hb, ow, rt, verdict.c_str(),
               arith.c_str());
    }
    printf("\n  ★ ARCHITECTURE RULE, from measurement: put the DISPLAY on the card doing the frame-gen, so a\n");
    printf("    frame crosses ONE WAY. A round trip doubles the cost and is the first thing to fail.\n");
    printf("  ★ The handoff table above is MEASURED ON THE DEV BOX and is a property of its links — on any\n");
    printf("    other machine re-measure it before trusting a verdict. RADV lands the import in a\n");
    printf("    HOST-VISIBLE aperture heap, so these are aperture rates, not peer-VRAM rates.\n");
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
    // shutil.copy2(LSFG_CONF, LSFG_CONF + ".bak-spirit") then append the block
    FILE *src = fopen(conf.c_str(), "rb");
    string bak = conf + ".bak-spirit";
    FILE *dst = src ? fopen(bak.c_str(), "wb") : nullptr;
    if (src && dst) {
        char buf[8192];
        size_t n;
        bool okc = true;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
            if (fwrite(buf, 1, n, dst) != n) { okc = false; break; }
        fclose(src); fclose(dst);
        if (okc) {
            FILE *af = fopen(conf.c_str(), "a");
            if (af) {
                size_t len = strlen(mb);
                bool wok = (fwrite(mb, 1, len, af) == len);
                fclose(af);
                if (wok)
                    return {name, "added profile " + py_repr(name) + " (backup: " + bak + ")"};
            }
        }
    } else {
        if (src) fclose(src);
        if (dst) fclose(dst);
    }
    return {std::nullopt, "could not write the profile (" + string(strerror(errno)) + ")"};
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
static std::optional<int> isolation_count(const string &vid, const string &did) {
    if (!which("vulkaninfo")) return std::nullopt;
    vector<string> env = current_env();
    set_env(env, "MESA_VK_DEVICE_SELECT", vid + ":" + did + "!");
    auto out = run_capture({"vulkaninfo", "--summary"}, &env);
    if (!out) return std::nullopt;
    int cnt = 0;
    size_t pos = 0;
    while ((pos = out->find("deviceName", pos)) != string::npos) { cnt++; pos += 10; }
    return cnt;
}

static int doctor(const J &db, bool as_json) {
    vector<J> checks;
    const J *ga = gpus_arr(db);
    size_t ngpu = ga ? ga->arr.size() : 0;

    // A. probe reads a link for every card
    {
        vector<string> miss;
        if (ga) for (auto &g : ga->arr) {
            const J *L = jget(g, "link");
            auto v = L ? ffield(*L, "gbps_max") : std::nullopt;
            if (!v) miss.push_back(sfield(g, "card"));
        }
        J c = J::object();
        c.put("check", J::str("probe-links"));
        c.put("verdict", J::str(miss.empty() ? "PASS" : "FAIL"));
        c.put("detail", J::str(miss.empty() ? "a host link for every card"
                                            : ("missing links: " + join(miss, ","))));
        checks.push_back(std::move(c));
    }

    // B. routing isolation
    if (!which("vulkaninfo")) {
        J c = J::object();
        c.put("check", J::str("routing-isolation"));
        c.put("verdict", J::str("UNKNOWN"));
        c.put("detail", J::str("vulkaninfo not installed — cannot test"));
        checks.push_back(std::move(c));
    } else if (ga) {
        for (auto &g : ga->arr) {
            string vendor = sfield(g, "vendor"), device = sfield(g, "device");
            string vid = vendor.size() > 2 ? vendor.substr(2) : string();
            string did = device.size() > 2 ? device.substr(2) : string();
            auto n = isolation_count(vid, did);
            string v, d;
            if (!n) { v = "UNKNOWN"; d = "could not run a test child"; }
            else if (*n == 1) { v = "PASS"; d = "a child sees exactly 1 device"; }
            else { v = "FAIL"; d = "a child sees " + std::to_string(*n) + " devices (expected 1)"; }
            J c = J::object();
            c.put("check", J::str("routing-isolation"));
            c.put("card", J::str(sfield(g, "card")));
            c.put("verdict", J::str(v));
            c.put("detail", J::str(d));
            checks.push_back(std::move(c));
        }
    }

    // C. telemetry readable
    {
        int n_ok = 0;
        if (ga) for (auto &g : ga->arr) {
            if (read_int("/sys/class/drm/" + sfield(g, "card") + "/device/mem_info_vram_used"))
                n_ok++;
        }
        J c = J::object();
        c.put("check", J::str("telemetry"));
        c.put("verdict", J::str((size_t)n_ok == ngpu ? "PASS" : "FAIL"));
        char b[128];
        snprintf(b, sizeof(b), "VRAM counters readable on %d/%d cards", n_ok, (int)ngpu);
        c.put("detail", J::str(b));
        checks.push_back(std::move(c));
    }

    // D. role plan
    {
        J rp = assign_roles(db, "game");
        int placed = 0;
        vector<string> unexplained;
        for (auto &r : rp.arr) {
            const J *rls = jget(r, "roles");
            bool idle = rls && rls->arr.size() == 1 && rls->arr[0].t == J::T::STR && rls->arr[0].s == "idle";
            const J *why = jget(r, "why");
            bool why_empty = !why || why->arr.empty();
            if (!idle) placed++;
            if (idle && why_empty) unexplained.push_back(sfield(r, "card"));
        }
        J c = J::object();
        c.put("check", J::str("role-plan"));
        c.put("verdict", J::str((placed && unexplained.empty()) ? "PASS" : "FAIL"));
        char b[192];
        snprintf(b, sizeof(b), "places %d/%d cards", placed, (int)rp.arr.size());
        string det = b;
        if (!unexplained.empty()) det += "; unexplained idle: " + join(unexplained, ",");
        c.put("detail", J::str(det));
        checks.push_back(std::move(c));
    }

    // E. UPSCALING — the path the tool advertises actually exists on this machine.
    {
        auto gs = gamescope_state();
        J c = J::object();
        c.put("check", J::str("upscaling"));
        c.put("verdict", J::str(gs.first == "READY" ? "PASS" : "UNKNOWN"));
        c.put("detail", J::str(gs.first + " — " + gs.second));
        checks.push_back(std::move(c));
    }

    // F. FRAME GENERATION — reported honestly; UNKNOWN is never green, and absent is not a
    //    failure of THIS machine (it needs software the user must own). Only a broken install
    //    is a FAIL.
    {
        auto ls = lsfg_state();
        string lv = (ls.first == "READY") ? "PASS" : (ls.first == "NOT-WORKING" ? "FAIL" : "UNKNOWN");
        J c = J::object();
        c.put("check", J::str("frame-generation"));
        c.put("verdict", J::str(lv));
        c.put("detail", J::str(ls.first + " — " + ls.second));
        checks.push_back(std::move(c));
    }

    bool bad = false;
    for (auto &c : checks) if (sfield(c, "verdict") == "FAIL") bad = true;
    string overall = bad ? "PROBLEMS FOUND" : "READY";

    if (as_json) {
        J o = J::object();
        o.put("spirit", J::str("doctor/0.1"));
        o.put("overall", J::str(overall));
        J arr = J::array();
        for (auto &c : checks) arr.push(c);
        o.put("checks", std::move(arr));
        printf("%s\n", json_dumps_indent2(o).c_str());
        return bad ? 1 : 0;
    }

    printf("spirit doctor — verifying this machine's multi-GPU readiness\n\n");
    for (auto &c : checks) {
        const J *cp = jget(c, "card");
        string card;
        if (cp && cp->t == J::T::STR) {
            char b[32];
            snprintf(b, sizeof(b), "%-6s ", cp->s.c_str());
            card = b;
        }
        printf("  [%-7s] %s%s\n", sfield(c, "verdict").c_str(), card.c_str(),
               sfield(c, "detail").c_str());
    }
    printf("\n  OVERALL: %s%s\n", overall.c_str(),
           bad ? " — see FAIL lines above" : "");
    return bad ? 1 : 0;
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
    string pci = "pci-" + bdf;
    set_env(env, "DRI_PRIME", pci);
    string vendor = sfield(*g, "vendor"), device = sfield(*g, "device");
    string mvs = (vendor.size() > 2 ? vendor.substr(2) : string()) + ":" +
                 (device.size() > 2 ? device.substr(2) : string()) + "!";
    set_env(env, "MESA_VK_DEVICE_SELECT", mvs);
    if (role == "compute") set_env(env, "GGML_VK_VISIBLE_DEVICES", bdf);

    const J *L = jget(*g, "link");
    auto gm = L ? ffield(*L, "gbps_max") : std::nullopt;
    string nm = sfield(*g, "name");
    if (nm.size() > 34) nm = nm.substr(0, 34);
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
        printf("     Removing this notice is the one thing the licence forbids. Put it back.\n\n");
    }
    return 0;
}

// A SYNTHETIC load: ffmpeg's OWN generated test pattern (testsrc2), piped to /dev/null. The encoder
// really runs — that is why the counters move — but the input is a test card and the output is
// discarded. A LOAD proves a card can be DRIVEN; a JOB produces something. --work is the job.
static pid_t start_test_load(const string &node) {
    vector<string> env = current_env();
    set_env(env, "LIBVA_DRIVER_NAME", "radeonsi");
    vector<string> argv = {"ffmpeg", "-hide_banner", "-loglevel", "error",
                           "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=60",
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
    size_t dot = b.rfind('.');
    return (dot == string::npos) ? b : b.substr(0, dot);
}

// Duration in seconds, or -1. -1 is returned, never a guess: a split whose extent is guessed drops
// the tail silently.
static double media_duration(const string &src) {
    string cmd = "ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 '" + src +
                 "' 2>/dev/null";
    FILE *f = popen(cmd.c_str(), "r");
    if (!f) return -1.0;
    char buf[128] = {0};
    if (!fgets(buf, sizeof(buf), f)) { pclose(f); return -1.0; }
    pclose(f);
    try { return std::stod(string(buf)); } catch (...) { return -1.0; }
}

// A REAL job on a spare card, to a real file — the WHOLE input, or one SEGMENT of it.
// seg >= 0 SPLITS one job across cards: each card gets a slice and the pieces have to add back up.
// Without it every card transcodes the same whole file, which is REPLICATION, a weaker claim.
static std::pair<string, pid_t> start_work(const string &node, const string &src,
                                           const string &outdir, const string &stem,
                                           long long seg, double ss, double dur) {
    vector<string> env = current_env();
    set_env(env, "LIBVA_DRIVER_NAME", "radeonsi");
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

// The card's short name, as the role table prints it.
static string gpu_name(const J &g) {
    string n = sfield(g, "name");
    if (n.empty()) n = sfield(g, "card");
    for (const char *cut : {"[", "(", "  "}) {
        size_t i = n.find(cut);
        if (i != string::npos && i > 0) n = n.substr(0, i);
    }
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (n.size() > 22) n = n.substr(0, 22);
    return n;
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

// [build] the game argv with the OTHER cards' /dev/dri nodes hidden behind a mount namespace.
// KEEP THE NAMESPACE INSIDE ANY OUTER WRAPPER: `unshare -r` maps our uid to root, so the
// ROOT-OWNED /tmp/.X11-unix presents as the overflow uid inside — a nested gamescope then dies
// at xwayland/sockets.c:83 "not owned by root or us". gamescope goes OUTSIDE this function; only
// the game is inside. Build here, call from the caller.
static vector<string> hidden_argv(const vector<string> &cmd, const J &main, const J &db) {
    if (is_wine_target(cmd)) return cmd;
    vector<string> hide;
    const J *ga = gpus_arr(db);
    if (ga) for (auto &g : ga->arr) {
        if (sfield(g, "card") == sfield(main, "card")) continue;
        for (const string &n : {sfield(g, "card"), sfield(g, "render_node")})
            if (!n.empty()) hide.push_back(n);
    }
    if (!hide.empty() && which("unshare")) {
        string script =
            "hide=\"$1\"; shift; for n in $hide; do "
            "[ -e \"/dev/dri/$n\" ] && mount --bind /dev/null \"/dev/dri/$n\" 2>/dev/null || true; "
            "done; exec \"$@\"";
        vector<string> argv = {"unshare", "-rm", "sh", "-c", script, "sh", join(hide, " ")};
        for (auto &c : cmd) argv.push_back(c);
        return argv;
    }
    return cmd;
}

static int play_cmd(const J &db, const vector<string> &cmd_in, bool do_offload,
                    std::optional<double> scale, std::optional<int> framegen, bool overlay,
                    string work, string work_outdir, bool split, string only_spares) {
    if (cmd_in.empty()) {
        fprintf(stderr, "usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] "
                        "[--work FILE [--work-outdir DIR] [--split]] [--spare NODE,...] -- <game command...>\n");
        return 1;
    }
    vector<string> cmd = cmd_in;
    const J *main = main_card(db);
    if (!main) { fprintf(stderr, "no GPUs detected\n"); return 1; }
    vector<const J *> spares;
    const J *ga = gpus_arr(db);
    if (ga) for (auto &g : ga->arr)
        if (sfield(g, "card") != sfield(*main, "card")) spares.push_back(&g);
    if (!only_spares.empty()) {
        // ★ --spare NODE[,NODE...] — the human names the cards that work. Everything else is left
        // alone entirely, not merely idle. A real session runs for hours unattended, and an old card
        // on a marginal link is the human's risk to take, not a default we assume for them.
        vector<string> want;
        for (size_t i = 0, j = 0; i <= only_spares.size(); i++)
            if (i == only_spares.size() || only_spares[i] == ',' || only_spares[i] == ' ') {
                if (i > j) want.push_back(only_spares.substr(j, i - j));
                j = i + 1;
            }
        vector<const J *> kept;
        for (auto *g : spares)
            for (const string &w : want)
                if (base_of(sfield(*g, "render_node")) == w) kept.push_back(g);
        if (kept.empty()) {
            printf("  spare: REFUSED — none of those render nodes is a spare card here. The game would\n"
                   "         run with no cards working beside it.\n");
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

    printf("spirit play — the game on the strongest card, every other card working the tail\n\n");
    const J *L = jget(*main, "link");
    auto xw = L ? ifield(*L, "max_width") : std::nullopt;
    string mgen = L ? s_or_none(*L, "max_gen") : "None";
    auto gm = L ? ffield(*L, "gbps_max") : std::nullopt;
    string mname = sfield(*main, "name");
    if (mname.size() > 30) mname = mname.substr(0, 30);
    printf("  MAIN  (renders)  %-6s %-30s x%s %s  %s GB/s\n", sfield(*main, "card").c_str(),
           mname.c_str(), xw ? std::to_string(*xw).c_str() : "None", mgen.c_str(),
           gm ? py_float_repr(*gm).c_str() : "None");
    for (auto *g : spares) {
        string sn = sfield(*g, "name");
        if (sn.size() > 30) sn = sn.substr(0, 30);
        string rn = sfield(*g, "render_node");
        if (rn.empty()) rn = "?";
        printf("  SPARE (offload)  %-6s %-30s node /dev/dri/%s\n", sfield(*g, "card").c_str(),
               sn.c_str(), rn.c_str());
    }

    vector<string> env = current_env();
    string pci = "pci-" + sfield(*main, "bdf");
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

    if (is_wine_target(cmd)) {
        printf("  wall: Proton/Wine detected — skipping the mount namespace (Wine refuses to run as\n");
        printf("        root, and `unshare -r` makes us root); the exclusive pin still applies.\n");
    }

    vector<pid_t> procs;
    vector<std::pair<string, string>> job_outputs;   // (node, out path)
    double job_expect = -1.0;
    if (do_offload && which("ffmpeg")) {
        vector<string> targets;
        for (auto *g : spares) {
            string n = sfield(*g, "render_node");
            if (!n.empty()) targets.push_back(n);
        }
        if (!work.empty()) {
            // A REAL JOB. Real input, real output, on cards that are NOT the one running the game.
            struct stat st;
            if (stat(work.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                printf("  work: REFUSED — %s is not a file. A job needs a real input; the synthetic\n"
                       "        test load is what runs when you do not give one.\n", work.c_str());
                return 1;
            }
            if (work_outdir.empty()) work_outdir = ".";
            mkdir(work_outdir.c_str(), 0755);
            string stem = stem_of(work);
            if (split && targets.size() > 1) {
                double total = media_duration(work);
                if (total <= 0.0) {
                    printf("  split: REFUSED — could not read the duration of %s, and a split whose\n"
                           "         extent is guessed drops the tail.\n", stem.c_str());
                    return 1;
                }
                size_t n = targets.size();
                double span = total / (double)n;
                printf("  ★ SPLIT JOB: %s is %.1fs — divided into %zu slices across %zu cards, one each.\n",
                       (stem + ".mp4").c_str(), total, n, n);
                for (size_t i = 0; i < n; i++) {
                    double ss = (double)i * span;
                    double dur = (i < n - 1) ? span : (total - ss);
                    auto r = start_work(targets[i], work, work_outdir, stem, (long long)i, ss, dur);
                    if (r.second > 0) procs.push_back(r.second);
                    job_outputs.push_back({targets[i], r.first});
                    printf("      %-12s slice %zu  [%6.1fs .. %6.1fs] -> %s\n", targets[i].c_str(), i + 1,
                           ss, ss + dur, base_of(r.first).c_str());
                }
                job_expect = total;
            } else {
                if (split)
                    printf("  split: only one spare card has a render node — replicating instead, and\n"
                           "         saying so rather than calling it a split.\n");
                for (const string &n : targets) {
                    auto r = start_work(n, work, work_outdir, stem, -1, 0.0, 0.0);
                    if (r.second > 0) procs.push_back(r.second);
                    job_outputs.push_back({n, r.first});
                }
                if (!procs.empty()) {
                    printf("  ★ JOB: transcoding %s on %d spare card(s) — real input, real output. "
                           "(WHOLE file on each card = REPLICATION, not a split; add --split to divide "
                           "one job between them.)\n", (stem + ".mp4").c_str(), (int)procs.size());
                    for (auto &jo : job_outputs)
                        printf("      %s -> %s\n", jo.first.c_str(), jo.second.c_str());
                }
            }
        } else {
            for (const string &n : targets) {
                pid_t p = start_test_load(n);
                if (p > 0) procs.push_back(p);
            }
            if (!procs.empty()) {
                printf("  cards: %d spare card(s) running a SYNTHETIC TEST LOAD\n", (int)procs.size());
                printf("         (ffmpeg's own testsrc2 test pattern -> /dev/null). This proves the\n");
                printf("         cards can be DRIVEN; it is not a job and produces nothing.\n");
                printf("         For real work: spirit play --work <file> [--work-outdir DIR] -- <game>\n");
            }
        }
    }
    printf("\n  launching…\n\n");
    fflush(stdout);

    // Build the hidden game argv FIRST, then wrap it in gamescope — gamescope stays OUTSIDE the
    // mount namespace (see hidden_argv).
    vector<string> inner = hidden_argv(cmd, *main, db);
    bool scale_on = scale && *scale != 0.0;
    if (overlay && !scale_on) {
        auto gs = gamescope_state();
        if (gs.first != "READY" || !native_res(db)) {
            printf("  overlay: REFUSED - gamescope not ready (%s)\n", gs.second.c_str());
            return 1;
        }
    }
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
            printf("  overlay: mangoapp inside gamescope - on-screen FPS is the COMPOSITOR's present rate\n");
            fflush(stdout);   // same buffering reason as the Python
        }
        gs.push_back("--");
        for (auto &c : inner) gs.push_back(c);
        inner = gs;
    }
    time_t t_game = time(nullptr);
    int rc = spawn_wait(inner, env, false);
    // A TEST LOAD IS A LOAD AND DIES WITH THE GAME. A JOB IS NOT: killing a half-finished transcode
    // leaves a truncated file and calls it done. Jobs are WAITED FOR, then each output is checked
    // for size and duration ON DISK — the artifact, never the child's exit code.
    if (!work.empty()) {
        for (pid_t p : procs) { int st; waitpid(p, &st, 0); }
    } else {
        for (pid_t p : procs) kill(p, SIGTERM);
    }
    if (!work.empty()) {
        printf("\n  game exited (rc=%d) — the job ran to completion (not stopped with the game)\n", rc);
        printf("  ── JOB OUTPUT ────────────────────────────────────────────────────────\n");
        double got = 0.0;
        int written = 0;
        for (auto &jo : job_outputs) {
            struct stat st;
            if (stat(jo.second.c_str(), &st) == 0) {
                double d = media_duration(jo.second);
                if (d > 0) { got += d; written++; }
                printf("  %-14s %s  (%.1f MB, %.1fs)\n", jo.first.c_str(), stem_of(jo.second).c_str(),
                       (double)st.st_size / 1048576.0, d > 0 ? d : 0.0);
            } else {
                printf("  %-14s MISSING — %s was not written. The job failed; do not report it as done.\n",
                       jo.first.c_str(), jo.second.c_str());
            }
        }
        if (job_expect > 0.0) {
            printf("  ── SPLIT CHECK ────────────────────────────────────────────────────────\n");
            printf("  slices written      : %d of %d\n", written, (int)job_outputs.size());
            printf("  slice durations sum : %.1fs\n", got);
            printf("  original duration   : %.1fs\n", job_expect);
            double diff = got - job_expect; if (diff < 0) diff = -diff;
            double tol = job_expect * 0.01; if (tol < 1.0) tol = 1.0;
            if (diff <= tol) printf("  ⇒ COMPLETE — the pieces reconstruct the whole input (within 1%%).\n");
            else printf("  ⇒ INCOMPLETE — %.1fs of %.1fs. The split LOST footage; do not report it as ok.\n",
                        job_expect - got, job_expect);
        }
    } else {
        printf("\n  game exited (rc=%d) — test load stopped\n", rc);
    }
    double elapsed = difftime(time(nullptr), t_game);

    // ★ THE TAIL — what the OTHER cards actually did. A feature you cannot see is indistinguishable
    // from one that is broken, and the loads are silent by design (ffmpeg's output goes to /dev/null).
    // So say it plainly, per card, and say "alive for" rather than claiming work was done.
    if (!procs.empty() || !job_outputs.empty()) {
        printf("\n  ── THE TAIL — what the other cards did ───────────────────────────────\n");
        if (!job_outputs.empty()) {
            for (auto &jo : job_outputs) {
                struct stat st;
                bool ok = stat(jo.second.c_str(), &st) == 0;
                string nm = "?";
                for (auto *g : spares) if (base_of(sfield(*g, "render_node")) == jo.first) nm = gpu_name(*g);
                printf("  %-12s %-22s REAL JOB  %s\n", jo.first.c_str(), nm.c_str(),
                       ok ? jo.second.c_str() : "FAILED — no file");
            }
        } else {
            for (auto *g : spares) {
                string node = base_of(sfield(*g, "render_node"));
                if (node.empty()) continue;
                printf("  %-12s %-22s SYNTHETIC TEST LOAD — alive for %.0f s of the session,\n",
                       node.c_str(), gpu_name(*g).c_str(), elapsed);
                printf("  %-12s %-22s produced nothing (a job needs --work FILE)\n", "", "");
            }
        }
    }
    return rc == 0 ? 0 : rc;
}

// ─────────────────────────────────────────────────────────────────────────────
// help / arg parsing
// ─────────────────────────────────────────────────────────────────────────────
static void print_help() {
    printf("usage: spirit [-h] {probe,roles,plan,doctor,origin,boost,fg} ...\n\n");
    printf("Spiritual Successor — put every GPU to work\n");
    printf("commands: probe · roles · plan · doctor · origin · fg · boost · run · play  (`run` and `play` take a command after `--`)\n\n");
    printf("positional arguments:\n");
    printf("  {probe,roles,plan,doctor,boost,fg}\n\n");
    printf("options:\n");
    printf("  -h, --help            show this help message and exit\n");
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
    return "usage: spirit [-h] {probe,roles,plan,doctor,origin,boost,fg} ...";
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
// argparse raises "unrecognized arguments" on the TOP-LEVEL parser.
static int unrec_error(const string &msg) {
    fprintf(stderr, "usage: spirit [-h] {probe,roles,plan,doctor,boost,fg} ...\n");
    fprintf(stderr, "spirit: error: %s\n", msg.c_str());
    return 2;
}

// Python int() semantics for argparse `type=int`: full-string base-10 parse.
static bool parse_py_int(const string &s, long long &out) {
    string t = trim(s);
    if (t.empty()) return false;
    size_t idx = 0;
    try {
        out = std::stoll(t, &idx, 10);
    } catch (...) { return false; }
    return idx == t.size();
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

int main(int argc, char **argv) {
    vector<string> args(argv + 1, argv + argc);

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
                        only_spares);
    }

    if (args.empty()) { print_help(); return 1; }
    if (args[0] == "-h" || args[0] == "--help") { print_help(); return 0; }

    string cmd = args[0];
    if (cmd != "probe" && cmd != "roles" && cmd != "plan" && cmd != "doctor" && cmd != "origin" &&
        cmd != "boost" && cmd != "fg") {
        fprintf(stderr, "usage: spirit [-h] {probe,roles,plan,doctor,origin,boost,fg} ...\n");
        fprintf(stderr,
                "spirit: error: argument cmd: invalid choice: '%s' (choose from 'probe', 'roles', 'plan', 'doctor', 'origin', 'boost', 'fg')\n",
                cmd.c_str());
        return 2;
    }

    // ★ THE MARK, ON EVERY RUN. Skipped only for --json (a machine is reading) and for `origin`
    // itself, which prints it properly. A tampered build cannot silence the warning inside it.
    {
        bool is_json = false;
        for (const string &a : args) if (a == "--json") is_json = true;
        if (!is_json && cmd != "origin") origin_cmd(false);
    }

    // global -h / --help inside a subcommand: argparse prints that subcommand's help
    for (size_t k = 1; k < args.size(); k++)
        if (args[k] == "-h" || args[k] == "--help") { print_sub_help(cmd); return 0; }

    J db = probe();

    if (cmd == "probe") {
        bool json = false;
        for (size_t k = 1; k < args.size(); k++) {
            if (args[k] == "--json") json = true;
            else return unrec_error("unrecognized arguments: " + args[k]);
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
            return unrec_error("unrecognized arguments: " + args[k]);
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
            return unrec_error("unrecognized arguments: " + args[k]);
        }
        return plan_cmd(db, workload, vram);
    }
    if (cmd == "doctor") {
        for (size_t k = 1; k < args.size(); k++)
            return unrec_error("unrecognized arguments: " + args[k]);
        return doctor(db, false);
    }
    if (cmd == "origin") {
        bool check = false;
        for (size_t k = 1; k < args.size(); k++) {
            if (args[k] == "--check") { check = true; continue; }
            return unrec_error("unrecognized arguments: " + args[k]);
        }
        return origin_cmd(check);
    }
    if (cmd == "boost") {
        for (size_t k = 1; k < args.size(); k++)
            return unrec_error("unrecognized arguments: " + args[k]);
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
            return unrec_error("unrecognized arguments: " + args[k]);
        }
        return fg_cmd(db, res, mode);
    }

    print_help();
    return 1;
}
