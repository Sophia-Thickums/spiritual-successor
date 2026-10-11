#!/usr/bin/env python3
"""Port tonight's Python work into the Rust build — the same three changes as the C++ port:
the origin mark, --work (a REAL job), --split (one job DIVIDED across cards), and the rule that a
job is WAITED FOR and checked on disk rather than killed with the game. Every anchor is asserted.
"""
import sys

P = "rust/src/main.rs"
s = open(P).read()
n0 = len(s)

NOTICE = "Spiritual Successor by Sophia Marie DeClue — github.com/Sophia-Thickums/spiritual-successor"
H = 0xcbf29ce484222325
for _b in NOTICE.encode():
    H ^= _b
    H = (H * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
SEAL = "%016x" % H


def sub(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit("ANCHOR MISMATCH (%d != %d): %r" % (n, count, old[:100]))
    s = s.replace(old, new)


# ── 1. origin + the renamed load + the real job ────────────────────────────────────────────────
sub("fn start_encode(node: &str) -> std::io::Result<Child> {", '''// ── ORIGIN — the mark. Every run prints it; break it and the tool says so, loudly, forever.
// Hiding a trace in the code is an old tradition, and it works here because this is not a comment —
// it is a CHECK. A comment gets deleted by accident; a check that reads its own notice can only be
// removed ON PURPOSE. The honest limit: a determined person can rip the whole check out. What it
// guarantees is that taking the credit is a decision, not an oversight.
const ORIGIN_NOTICE: &str = "@NOTICE@";
const ORIGIN_SEAL: &str = "@SEAL@";
const ORIGIN_TRUE: &str = "@NOTICE@";

/// FNV-1a 64 — ten lines, no dependency, identical bytes to the Python and C++ builds.
/// NOT cryptographic, and it does not need to be: its job is to detect that the notice CHANGED.
fn origin_seal() -> String {
    let mut h: u64 = 0xcbf29ce484222325;
    for b in ORIGIN_NOTICE.as_bytes() {
        h ^= *b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    format!("{:016x}", h)
}

fn origin_cmd(check_only: bool) -> i32 {
    let actual = origin_seal();
    let intact = actual == ORIGIN_SEAL;
    if !check_only {
        println!("  {}", ORIGIN_NOTICE);
        println!("  mark: {}", &actual[..16]);
    }
    if check_only {
        return if intact { 0 } else { 1 };
    }
    if !intact {
        // NOT SILENCEABLE: prints on stdout, on every run, in the tool's own voice.
        println!();
        println!("  ★★ THIS IS A MODIFIED COPY. The origin mark does not verify.");
        println!("     This is a copy of, or derived from: {}", ORIGIN_TRUE);
        println!("     Expected mark {}, found {}.", &ORIGIN_SEAL[..16], &actual[..16]);
        println!("     Removing this notice is the one thing the licence forbids. Put it back.");
        println!();
    }
    0
}

fn base_of(path: &str) -> String {
    path.rsplit('/').next().unwrap_or(path).to_string()
}

fn stem_of(path: &str) -> String {
    let b = base_of(path);
    match b.rfind('.') {
        Some(i) if i > 0 => b[..i].to_string(),
        _ => b,
    }
}

/// Duration in seconds, or -1.0. -1.0 is returned, never a guess: a split whose extent is guessed
/// drops the tail silently.
fn media_duration(src: &str) -> f64 {
    match Command::new("ffprobe")
        .args(["-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", src])
        .output()
    {
        Ok(o) => String::from_utf8_lossy(&o.stdout).trim().parse::<f64>().unwrap_or(-1.0),
        Err(_) => -1.0,
    }
}

/// A SYNTHETIC load: ffmpeg's OWN generated test pattern (testsrc2), piped to /dev/null. The encoder
/// really runs — that is why the counters move — but the input is a test card and the output is
/// discarded. A LOAD proves a card can be DRIVEN; a JOB produces something. --work is the job.
fn start_test_load(node: &str) -> std::io::Result<Child> {''')

# give the renamed body its comment by inserting before the Command::new line
sub("""fn start_test_load(node: &str) -> std::io::Result<Child> {
    Command::new("ffmpeg")""",
    """fn start_test_load(node: &str) -> std::io::Result<Child> {
    Command::new("ffmpeg")""")

# real work, whole file or one segment
sub("fn is_wine_target(", '''/// A REAL job on a spare card, to a real file — the WHOLE input, or one SEGMENT of it.
/// `seg >= 0` SPLITS one job across cards: each card gets a slice and the pieces have to add back up.
/// Without it every card transcodes the same whole file, which is REPLICATION, a weaker claim.
fn start_work(
    node: &str,
    src: &str,
    outdir: &str,
    stem: &str,
    seg: i64,
    ss: f64,
    dur: f64,
) -> std::io::Result<(String, Child)> {
    let tag = if seg >= 0 {
        format!("{}_part{}", node, seg + 1)
    } else {
        node.to_string()
    };
    let out = format!("{}/{}.{}.mp4", outdir, stem, tag);
    let mut args: Vec<String> = vec![
        "-hide_banner".into(), "-loglevel".into(), "error".into(), "-y".into(),
    ];
    if seg >= 0 {
        args.push("-ss".into());
        args.push(format!("{:.3}", ss));
        args.push("-t".into());
        args.push(format!("{:.3}", dur));
    }
    args.push("-i".into());
    args.push(src.into());
    args.push("-vaapi_device".into());
    args.push(format!("/dev/dri/{}", node));
    args.push("-vf".into());
    args.push("format=nv12,hwupload".into());
    args.push("-c:v".into());
    args.push("h264_vaapi".into());
    args.push("-b:v".into());
    args.push("8M".into());
    args.push(out.clone());
    let ch = Command::new("ffmpeg")
        .args(&args)
        .env("LIBVA_DRIVER_NAME", "radeonsi")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;
    Ok((out, ch))
}

fn is_wine_target(''')

# ── 2. play signature ────────────────────────────────────────────────────────────────────────────
sub("fn play(db: &Json, cmd: &[String], do_offload: bool, scale: Option<f64>, framegen: Option<i64>, overlay: bool) -> i32 {",
    "fn play(db: &Json, cmd: &[String], do_offload: bool, scale: Option<f64>, framegen: Option<i64>,\n        overlay: bool, work: &str, work_outdir: &str, split: bool) -> i32 {")
sub('''eprintln!("usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] -- <game command...>");''',
    '''eprintln!("usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] \\
[--work FILE [--work-outdir DIR] [--split]] -- <game command...>");''')

# ── 3. the offload block becomes the job/split logic ─────────────────────────────────────────────
sub('''    let mut procs: Vec<Child> = Vec::new();
    if do_offload && which("ffmpeg").is_some() {
        for g in &spares {
            if let Some(n) = g.get("render_node").and_then(|v| v.as_str()) {
                if !n.is_empty() {
                    if let Ok(ch) = start_encode(n) {
                        procs.push(ch);
                    }
                }
            }
        }
        if !procs.is_empty() {
            println!(
                "  offload: {} real h264 VAAPI encode(s) running on the spare cards",
                procs.len()
            );
        }
    }''',
'''    let mut procs: Vec<Child> = Vec::new();
    let mut job_outputs: Vec<(String, String)> = Vec::new();
    let mut job_expect: f64 = -1.0;
    if do_offload && which("ffmpeg").is_some() {
        let targets: Vec<String> = spares
            .iter()
            .filter_map(|g| g.get("render_node").and_then(|v| v.as_str()))
            .filter(|n| !n.is_empty())
            .map(|n| n.to_string())
            .collect();
        if !work.is_empty() {
            // A REAL JOB. Real input, real output, on cards that are NOT the one running the game.
            if !std::path::Path::new(work).is_file() {
                println!(
                    "  work: REFUSED — {} is not a file. A job needs a real input; the synthetic",
                    work
                );
                println!("        test load is what runs when you do not give one.");
                return 1;
            }
            let outdir = if work_outdir.is_empty() { "." } else { work_outdir };
            let _ = std::fs::create_dir_all(outdir);
            let stem = stem_of(work);
            if split && targets.len() > 1 {
                let total = media_duration(work);
                if total <= 0.0 {
                    println!(
                        "  split: REFUSED — could not read the duration of {}, and a split whose",
                        work
                    );
                    println!("         extent is guessed drops the tail.");
                    return 1;
                }
                let n = targets.len();
                let span = total / n as f64;
                println!(
                    "  ★ SPLIT JOB: {}.mp4 is {:.1}s — divided into {} slices across {} cards, one each.",
                    stem, total, n, n
                );
                for (i, node) in targets.iter().enumerate() {
                    let ss = i as f64 * span;
                    let dur = if i < n - 1 { span } else { total - ss };
                    match start_work(node, work, outdir, &stem, i as i64, ss, dur) {
                        Ok((out, ch)) => {
                            println!(
                                "      {:<12} slice {}  [{:6.1}s .. {:6.1}s] -> {}",
                                node, i + 1, ss, ss + dur, base_of(&out)
                            );
                            procs.push(ch);
                            job_outputs.push((node.clone(), out));
                        }
                        Err(_) => {}
                    }
                }
                job_expect = total;
            } else {
                if split {
                    println!("  split: only one spare card has a render node — replicating instead, and");
                    println!("         saying so rather than calling it a split.");
                }
                for node in &targets {
                    if let Ok((out, ch)) = start_work(node, work, outdir, &stem, -1, 0.0, 0.0) {
                        job_outputs.push((node.clone(), out));
                        procs.push(ch);
                    }
                }
                if !procs.is_empty() {
                    println!(
                        "  ★ JOB: transcoding {}.mp4 on {} spare card(s) — real input, real output. \\
(WHOLE file on each card = REPLICATION, not a split; add --split to divide one job between them.)",
                        stem,
                        procs.len()
                    );
                    for (node, out) in &job_outputs {
                        println!("      {} -> {}", node, out);
                    }
                }
            }
        } else {
            for n in &targets {
                if let Ok(ch) = start_test_load(n) {
                    procs.push(ch);
                }
            }
            if !procs.is_empty() {
                println!("  cards: {} spare card(s) running a SYNTHETIC TEST LOAD", procs.len());
                println!("         (ffmpeg's own testsrc2 test pattern -> /dev/null). This proves the");
                println!("         cards can be DRIVEN; it is not a job and produces nothing.");
                println!("         For real work: spirit play --work <file> [--work-outdir DIR] -- <game>");
            }
        }
    }''')

# ── 4. exit: job waited for + checked on disk; load dies with the game ───────────────────────────
sub('''    let rc = spawn_argv(&inner, &env_pins);
    for p in procs.iter_mut() {
        let _ = p.kill();
        let _ = p.wait();
    }
    println!();
    println!("  game exited (rc={}) — offload stopped", rc);
    if rc == 0 {
        0
    } else {
        rc
    }
}''',
'''    let rc = spawn_argv(&inner, &env_pins);
    // A TEST LOAD IS A LOAD AND DIES WITH THE GAME. A JOB IS NOT: killing a half-finished transcode
    // leaves a truncated file and calls it done. Jobs are WAITED FOR, then each output is checked
    // for size and duration ON DISK — the artifact, never the child's exit code.
    if !work.is_empty() {
        for p in procs.iter_mut() {
            let _ = p.wait();
        }
    } else {
        for p in procs.iter_mut() {
            let _ = p.kill();
            let _ = p.wait();
        }
    }
    println!();
    if !work.is_empty() {
        println!("  game exited (rc={}) — the job ran to completion (not stopped with the game)", rc);
        println!("  ── JOB OUTPUT ────────────────────────────────────────────────────────");
        let mut got = 0.0f64;
        let mut written = 0usize;
        for (node, out) in &job_outputs {
            if std::path::Path::new(out).is_file() {
                let sz = std::fs::metadata(out).map(|m| m.len()).unwrap_or(0);
                let d = media_duration(out);
                if d > 0.0 {
                    got += d;
                    written += 1;
                }
                println!(
                    "  {:<14} {}  ({:.1} MB, {:.1}s)",
                    node,
                    base_of(out),
                    sz as f64 / 1048576.0,
                    if d > 0.0 { d } else { 0.0 }
                );
            } else {
                println!(
                    "  {:<14} MISSING — {} was not written. The job failed; do not report it as done.",
                    node, out
                );
            }
        }
        if job_expect > 0.0 {
            println!("  ── SPLIT CHECK ────────────────────────────────────────────────────────");
            println!("  slices written      : {} of {}", written, job_outputs.len());
            println!("  slice durations sum : {:.1}s", got);
            println!("  original duration   : {:.1}s", job_expect);
            let tol = if job_expect * 0.01 < 1.0 { 1.0 } else { job_expect * 0.01 };
            if (got - job_expect).abs() <= tol {
                println!("  ⇒ COMPLETE — the pieces reconstruct the whole input (within 1%).");
            } else {
                println!(
                    "  ⇒ INCOMPLETE — {:.1}s of {:.1}s. The split LOST footage; do not report it as ok.",
                    job_expect - got,
                    job_expect
                );
            }
        }
    } else {
        println!("  game exited (rc={}) — test load stopped", rc);
    }
    if rc == 0 {
        0
    } else {
        rc
    }
}''')

# ── 5. arg parse ─────────────────────────────────────────────────────────────────────────────────
sub('''            if argv[i] == "--no-offload" {
                offload = false;
                i += 1;
            } else if argv[i] == "--scale" {''',
'''            if argv[i] == "--no-offload" {
                offload = false;
                i += 1;
            } else if argv[i] == "--split" {
                split = true;
                i += 1;
            } else if argv[i] == "--work" {
                // REAL WORK, not the synthetic test load. The input must exist.
                match argv.get(i + 1) {
                    Some(v) => { work = v.clone(); }
                    None => {
                        eprintln!("spirit play: --work needs a file, e.g. --work capture.mkv");
                        return 2;
                    }
                }
                i += 2;
            } else if argv[i] == "--work-outdir" {
                match argv.get(i + 1) {
                    Some(v) => { work_outdir = v.clone(); }
                    None => {
                        eprintln!("spirit play: --work-outdir needs a directory");
                        return 2;
                    }
                }
                i += 2;
            } else if argv[i] == "--scale" {''')
sub("        return play(&probe(), &rest, offload, scale, framegen, overlay);",
    "        return play(&probe(), &rest, offload, scale, framegen, overlay, &work, &work_outdir, split);")

# ── 6. origin command + the every-run mark ───────────────────────────────────────────────────────
sub('''    if !matches!(cmd, "probe" | "roles" | "plan" | "doctor" | "fg" | "boost") {''',
    '''    if !matches!(cmd, "probe" | "roles" | "plan" | "doctor" | "origin" | "fg" | "boost") {''')
sub('''            "argument cmd: invalid choice: '{}' (choose from 'probe', 'roles', 'plan', 'doctor', 'boost', 'fg')",''',
    '''            "argument cmd: invalid choice: '{}' (choose from 'probe', 'roles', 'plan', 'doctor', 'origin', 'boost', 'fg')",''')

s = s.replace('println!("commands: probe · roles · plan · doctor · fg · boost · run · play  (`run` and `play` take a command after `--`)");',
              'println!("commands: probe · roles · plan · doctor · origin · fg · boost · run · play  (`run` and `play` take a command after `--`)");')
s = s.replace('println!("  {{probe,roles,plan,doctor,boost,fg}}");',
              'println!("  {{probe,roles,plan,doctor,origin,boost,fg}}");')

# 2. the play parser needs the new locals
sub("""        let mut overlay = false;
        let mut i = 1;""",
    """        let mut overlay = false;
        let mut work = String::new();
        let mut work_outdir = String::new();
        let mut split = false;
        let mut i = 1;""")

# 3. the origin command + the every-run mark
sub("""    if argv[0] == "-h" || argv[0] == "--help" {
        print_help(None);
        return 0;
    }""",
    """    if argv[0] == "-h" || argv[0] == "--help" {
        print_help(None);
        return 0;
    }

    if argv[0] == "origin" {
        let check = argv.iter().any(|a| a == "--check");
        return origin_cmd(check);
    }

    // ★ THE MARK, ON EVERY RUN. Skipped only for --json, where a machine is reading and a stray
    // line would break it. A tampered build cannot silence the warning inside origin_cmd.
    if !argv.iter().any(|a| a == "--json") {
        origin_cmd(false);
    }""")

sub('        "doctor" => "usage: spirit doctor [-h]",',
    '        "doctor" => "usage: spirit doctor [-h]",\n        "origin" => "usage: spirit origin [-h] [--check]",')

s = s.replace("@NOTICE@", NOTICE).replace("@SEAL@", SEAL)
open(P, "w").write(s)
print("rust port script finalised")
print("Rust port applied: %d -> %d chars (%+d)" % (n0, len(s), len(s) - n0))
print("seal:", SEAL)
