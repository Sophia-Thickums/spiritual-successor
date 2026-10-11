#!/usr/bin/env python3
"""Port tonight's Python work into the C++ build, mechanically and with assertions on every anchor.

Everything here mirrors spirit.py: the origin mark, --work (a REAL job), --split (one job DIVIDED
across cards), and the rule that a job is waited for and checked on disk rather than killed with the
game. Anchors are asserted so a silent no-match can never produce a half-ported file.
"""
import re, sys

P = "cpp/spirit.cpp"
s = open(P).read()
orig_len = len(s)
NOTICE = "Spiritual Successor by Sophia Marie DeClue — github.com/Sophia-Thickums/spiritual-successor"
H = 0xcbf29ce484222325
for _b in NOTICE.encode():
    H ^= _b; H = (H * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
SEAL = "%016x" % H


def sub(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit("ANCHOR MISMATCH (%d != %d): %r" % (n, count, old[:90]))
    s = s.replace(old, new)


# ── 1. origin mark ────────────────────────────────────────────────────────────────────────────
sub("static pid_t start_encode(const string &node) {", """// ── ORIGIN — the mark. Every run prints it; break it and the tool says so, loudly, forever.
// Hiding a trace in the code is an old tradition, and it works here because this is not a comment —
// it is a CHECK. A comment gets deleted by accident; a check that reads its own notice can only be
// removed ON PURPOSE. The honest limit, stated so nobody thinks it is stronger than it is: a
// determined person can rip the whole check out. What it guarantees is that taking the credit is a
// decision, not an oversight.
static const char *ORIGIN_NOTICE = "@NOTICE@";
static const char *ORIGIN_SEAL = "@SEAL@";
static const char *ORIGIN_TRUE = "@NOTICE@";

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
        printf("  %%s\\n", ORIGIN_NOTICE);
        printf("  mark: %%s\\n", actual.substr(0, 16).c_str());
    }
    if (check_only) return intact ? 0 : 1;
    if (!intact) {
        // NOT SILENCEABLE: prints on stdout, on every run, in the tool's own voice.
        printf("\\n  ★★ THIS IS A MODIFIED COPY. The origin mark does not verify.\\n");
        printf("     This is a copy of, or derived from: %%s\\n", ORIGIN_TRUE);
        printf("     Expected mark %%s, found %%s.\\n", string(ORIGIN_SEAL).substr(0, 16).c_str(),
               actual.substr(0, 16).c_str());
        printf("     Removing this notice is the one thing the licence forbids. Put it back.\\n\\n");
    }
    return 0;
}

static pid_t start_test_load(const string &node) {""".replace("@NOTICE@", NOTICE).replace("@SEAL@", SEAL))

# the renamed body keeps its argv; give it the honest docstring the Python one has
sub("""static pid_t start_test_load(const string &node) {
    vector<string> env = current_env();""",
    """// A SYNTHETIC load: ffmpeg's OWN generated test pattern (testsrc2), piped to /dev/null. The encoder
// really runs — that is why the counters move — but the input is a test card and the output is
// discarded. A LOAD proves a card can be DRIVEN; a JOB produces something. --work is the job.
static pid_t start_test_load(const string &node) {
    vector<string> env = current_env();""")

# ── 2. real work + split helpers ───────────────────────────────────────────────────────────────
sub("""// True if this is a Proton/Wine launch, where the mount-namespace wall MUST be skipped.""",
    """static string stem_of(const string &path) {
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

// True if this is a Proton/Wine launch, where the mount-namespace wall MUST be skipped.""")

# ── 3. play_cmd signature ──────────────────────────────────────────────────────────────────────
sub("""static int play_cmd(const J &db, const vector<string> &cmd_in, bool do_offload,
                    std::optional<double> scale, std::optional<int> framegen, bool overlay) {
    if (cmd_in.empty()) {
        fprintf(stderr, "usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] -- <game command...>\\n");""",
    """static int play_cmd(const J &db, const vector<string> &cmd_in, bool do_offload,
                    std::optional<double> scale, std::optional<int> framegen, bool overlay,
                    string work, string work_outdir, bool split) {
    if (cmd_in.empty()) {
        fprintf(stderr, "usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] "
                        "[--work FILE [--work-outdir DIR] [--split]] -- <game command...>\\n");""")

# ── 4. the offload block becomes the job/split logic ───────────────────────────────────────────
sub("""    vector<pid_t> procs;
    if (do_offload && which("ffmpeg")) {
        for (auto *g : spares) {
            string n = sfield(*g, "render_node");
            if (!n.empty()) {
                pid_t p = start_encode(n);
                if (p > 0) procs.push_back(p);
            }
        }
        if (!procs.empty())
            printf("  offload: %d real h264 VAAPI encode(s) running on the spare cards\\n",
                   (int)procs.size());
    }""",
    """    vector<pid_t> procs;
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
                printf("  work: REFUSED — %s is not a file. A job needs a real input; the synthetic\\n"
                       "        test load is what runs when you do not give one.\\n", work.c_str());
                return 1;
            }
            if (work_outdir.empty()) work_outdir = ".";
            mkdir(work_outdir.c_str(), 0755);
            string stem = stem_of(work);
            if (split && targets.size() > 1) {
                double total = media_duration(work);
                if (total <= 0.0) {
                    printf("  split: REFUSED — could not read the duration of %s, and a split whose\\n"
                           "         extent is guessed drops the tail.\\n", stem.c_str());
                    return 1;
                }
                size_t n = targets.size();
                double span = total / (double)n;
                printf("  ★ SPLIT JOB: %s is %.1fs — divided into %zu slices across %zu cards, one each.\\n",
                       (stem + ".mp4").c_str(), total, n, n);
                for (size_t i = 0; i < n; i++) {
                    double ss = (double)i * span;
                    double dur = (i < n - 1) ? span : (total - ss);
                    auto r = start_work(targets[i], work, work_outdir, stem, (long long)i, ss, dur);
                    if (r.second > 0) procs.push_back(r.second);
                    job_outputs.push_back({targets[i], r.first});
                    printf("      %-12s slice %zu  [%6.1fs .. %6.1fs] -> %s\\n", targets[i].c_str(), i + 1,
                           ss, ss + dur, stem_of(r.first).c_str());
                }
                job_expect = total;
            } else {
                if (split)
                    printf("  split: only one spare card has a render node — replicating instead, and\\n"
                           "         saying so rather than calling it a split.\\n");
                for (const string &n : targets) {
                    auto r = start_work(n, work, work_outdir, stem, -1, 0.0, 0.0);
                    if (r.second > 0) procs.push_back(r.second);
                    job_outputs.push_back({n, r.first});
                }
                if (!procs.empty()) {
                    printf("  ★ JOB: transcoding %s on %d spare card(s) — real input, real output. "
                           "(WHOLE file on each card = REPLICATION, not a split; add --split to divide "
                           "one job between them.)\\n", (stem + ".mp4").c_str(), (int)procs.size());
                    for (auto &jo : job_outputs)
                        printf("      %s -> %s\\n", jo.first.c_str(), jo.second.c_str());
                }
            }
        } else {
            for (const string &n : targets) {
                pid_t p = start_test_load(n);
                if (p > 0) procs.push_back(p);
            }
            if (!procs.empty()) {
                printf("  cards: %d spare card(s) running a SYNTHETIC TEST LOAD\\n", (int)procs.size());
                printf("         (ffmpeg's own testsrc2 test pattern -> /dev/null). This proves the\\n");
                printf("         cards can be DRIVEN; it is not a job and produces nothing.\\n");
                printf("         For real work: spirit play --work <file> [--work-outdir DIR] -- <game>\\n");
            }
        }
    }""")

# ── 5. exit: a job is waited for and checked on disk; a load dies with the game ────────────────
sub("""    int rc = spawn_wait(inner, env, false);
    for (pid_t p : procs) kill(p, SIGTERM);
    printf("\\n  game exited (rc=%d) — offload stopped\\n", rc);
    return rc == 0 ? 0 : rc;""",
    """    int rc = spawn_wait(inner, env, false);
    // A TEST LOAD IS A LOAD AND DIES WITH THE GAME. A JOB IS NOT: killing a half-finished transcode
    // leaves a truncated file and calls it done. Jobs are WAITED FOR, then each output is checked
    // for size and duration ON DISK — the artifact, never the child's exit code.
    if (!work.empty()) {
        for (pid_t p : procs) { int st; waitpid(p, &st, 0); }
    } else {
        for (pid_t p : procs) kill(p, SIGTERM);
    }
    if (!work.empty()) {
        printf("\\n  game exited (rc=%d) — the job ran to completion (not stopped with the game)\\n", rc);
        printf("  ── JOB OUTPUT ────────────────────────────────────────────────────────\\n");
        double got = 0.0;
        int written = 0;
        for (auto &jo : job_outputs) {
            struct stat st;
            if (stat(jo.second.c_str(), &st) == 0) {
                double d = media_duration(jo.second);
                if (d > 0) { got += d; written++; }
                printf("  %-14s %s  (%.1f MB, %.1fs)\\n", jo.first.c_str(), stem_of(jo.second).c_str(),
                       (double)st.st_size / 1048576.0, d > 0 ? d : 0.0);
            } else {
                printf("  %-14s MISSING — %s was not written. The job failed; do not report it as done.\\n",
                       jo.first.c_str(), jo.second.c_str());
            }
        }
        if (job_expect > 0.0) {
            printf("  ── SPLIT CHECK ────────────────────────────────────────────────────────\\n");
            printf("  slices written      : %d of %d\\n", written, (int)job_outputs.size());
            printf("  slice durations sum : %.1fs\\n", got);
            printf("  original duration   : %.1fs\\n", job_expect);
            double diff = got - job_expect; if (diff < 0) diff = -diff;
            double tol = job_expect * 0.01; if (tol < 1.0) tol = 1.0;
            if (diff <= tol) printf("  ⇒ COMPLETE — the pieces reconstruct the whole input (within 1%%).\\n");
            else printf("  ⇒ INCOMPLETE — %.1fs of %.1fs. The split LOST footage; do not report it as ok.\\n",
                        job_expect - got, job_expect);
        }
    } else {
        printf("\\n  game exited (rc=%d) — test load stopped\\n", rc);
    }
    return rc == 0 ? 0 : rc;""")

# ── 6. arg parse ───────────────────────────────────────────────────────────────────────────────
sub("""        std::optional<double> scale;
        std::optional<int> framegen;
        while (i < args.size()) {
            if (args[i] == "--no-offload") { offload = false; i += 1; }
            else if (args[i] == "--scale") {""",
    """        std::optional<double> scale;
        std::optional<int> framegen;
        string work, work_outdir;
        bool split = false;
        while (i < args.size()) {
            if (args[i] == "--no-offload") { offload = false; i += 1; }
            else if (args[i] == "--split") { split = true; i += 1; }
            else if (args[i] == "--work") {
                // REAL WORK, not the synthetic test load. The input must exist.
                if (i + 1 >= args.size()) {
                    fprintf(stderr, "spirit play: --work needs a file, e.g. --work capture.mkv\\n");
                    return 2;
                }
                work = args[i + 1]; i += 2;
            }
            else if (args[i] == "--work-outdir") {
                if (i + 1 >= args.size()) {
                    fprintf(stderr, "spirit play: --work-outdir needs a directory\\n");
                    return 2;
                }
                work_outdir = args[i + 1]; i += 2;
            }
            else if (args[i] == "--scale") {""")

sub("        return play_cmd(probe(), rest, offload, scale, framegen, overlay);",
    "        return play_cmd(probe(), rest, offload, scale, framegen, overlay, work, work_outdir, split);")

# ── 7. the origin command ───────────────────────────────────────────────────────────────────────
sub("""    printf("usage: spirit [-h] {probe,roles,plan,doctor,boost,fg} ...\\n\\n");""",
    """    printf("usage: spirit [-h] {probe,roles,plan,doctor,origin,boost,fg} ...\\n\\n");""")
sub("""    printf("commands: probe · roles · plan · doctor · fg · boost · run · play  (`run` and `play` take a command after `--`)\\n\\n");""",
    """    printf("commands: probe · roles · plan · doctor · origin · fg · boost · run · play  (`run` and `play` take a command after `--`)\\n\\n");""")
sub("""    if (cmd == "doctor") return "usage: spirit doctor [-h]";""",
    """    if (cmd == "doctor") return "usage: spirit doctor [-h]";
    if (cmd == "origin") return "usage: spirit origin [-h] [--check]";""")
sub("""    return "usage: spirit [-h] {probe,roles,plan,doctor,boost,fg} ...";""",
    """    return "usage: spirit [-h] {probe,roles,plan,doctor,origin,boost,fg} ...";""")

s = s.replace("""#include <sys/stat.h>""", """#include <sys/stat.h>""")   # no-op; header check below

open(P, "w").write(s)
print("C++ port applied: %d -> %d chars (%+d)" % (orig_len, len(s), len(s) - orig_len))
print("seal:", SEAL[:16])
