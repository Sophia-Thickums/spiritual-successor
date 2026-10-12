//! spirit-gui — the native desktop app for Spiritual Successor.
//!
//! GTK4 + libadwaita, in Rust, the same shape as the GPU tool people already run (LACT). No browser,
//! no local server, no token. It drives the `spirit` command that sits beside it (or on PATH) and shows
//! what that command actually returned — the app never invents a status of its own.
//!
//! ★ THE HONESTY RULES THIS APP INHERITS FROM THE TOOL, because a GUI is where they get lost:
//!   * a card's capability is READ FROM THE TOOL (`can_encode`, `marginal` in `probe --json`), never
//!     re-derived here — a second copy of a rule is how a front end promises a job the tool refuses;
//!   * the on/off switch reports the state READ BACK FROM THE MACHINE (is the value reaching processes?),
//!     never "the button was pressed", and it re-reads when the command it ran has FINISHED rather than
//!     after a guessed delay;
//!   * every job's output is appended verbatim, so a failure is readable rather than summarised;
//!   * one control, one action: the "after a reboot" box is a modifier read when you turn the pin ON, not
//!     a second power switch with a surprise blast radius (outside read, 2026-10-12).

use adw::prelude::*;
use gtk::{gio, glib};
use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::rc::Rc;
use std::sync::{Arc, Mutex};

const APP_ID: &str = "com.sophia.SpiritualSuccessor";

/// The output pane is bounded. Above this many characters, the oldest quarter comes off the front.
const BUF_LIMIT: i32 = 400_000;

/// Everything an asynchronous run needs to show its work, so the run helper takes one argument instead
/// of four that must be remembered in the right order.
#[derive(Clone)]
struct Ui {
    buf: gtk::TextBuffer,
    view: gtk::TextView,
    mark: gtk::TextMark,
    set_busy: Rc<dyn Fn(bool)>,
}

impl Ui {
    /// ★ THE ONE PLACE THE OUTPUT PANE IS WRITTEN. This insert / cap-trim / move-mark / scroll dance used
    /// to be copied into five call sites, and two of the copies silently skipped the cap-trim — so the
    /// pane was bounded on the streaming paths and unbounded on the short ones, in code that read as
    /// identical. One function, one bound. (Outside read, 2026-10-12: "extraction next, with a test.")
    fn append(&self, text: &str) {
        let mut end = self.buf.end_iter();
        self.buf.insert(&mut end, text);
        let trim = overflow_to_trim(self.buf.char_count());
        if trim > 0 {
            let mut start = self.buf.start_iter();
            let mut cut = self.buf.start_iter();
            cut.forward_chars(trim);
            self.buf.delete(&mut start, &mut cut);
        }
        // ★ SCROLL BY MARK, NOT BY `upper()` — `upper` has not been recomputed for the text just
        // inserted, so poking it lands one insert behind and a finished job's last line can sit below
        // the fold.
        self.buf.move_mark(&self.mark, &self.buf.end_iter());
        self.view.scroll_mark_onscreen(&self.mark);
    }
}

/// How many characters must come off the FRONT of the pane, given its char count. Zero means nothing to
/// do. Kept pure and separate from the GTK call so a test can pin the bound itself — the copies that
/// skipped the trim are exactly why this is a function rather than a constant somebody remembers.
fn overflow_to_trim(char_count: i32) -> i32 {
    if char_count > BUF_LIMIT {
        char_count / 4
    } else {
        0
    }
}

/// ★ THE BUSY GATE AS A VALUE, NOT A BOOLEAN PASSED BETWEEN CALLERS. The fault this replaces was an
/// ownership question answered by comments: one function raised the gate, another returned a bool saying
/// whether it had taken responsibility for it, and a third acted on that bool. Get any link wrong and the
/// window wedges — Run, Doctor, both switches and the card field dead for the life of the window, on the
/// ORDINARY "no spare card" path. Here the gate is lowered by DROPPING this value, so every early return,
/// every error branch and every `?` releases it, and the only way to keep it up is to hand it to the batch,
/// which then owns the release and says so in one call. (Outside reads, 2026-10-12.)
struct Busy {
    set: Rc<dyn Fn(bool)>,
    /// Not shared, not cloned, never read by anyone else — a plain flag. It was an `Rc<Cell<bool>>`,
    /// which is the shape of a boolean two owners agree about, i.e. the thing this type exists to remove.
    handed_over: Cell<bool>,
}

impl Busy {
    /// Raise the gate around a given setter. Split from `raise` so a test can pass a recorder.
    fn raised_with(set: Rc<dyn Fn(bool)>) -> Busy {
        set(true);
        Busy { set, handed_over: Cell::new(false) }
    }
    fn raise(ui: &Ui) -> Busy {
        Busy::raised_with(ui.set_busy.clone())
    }
    /// The batch is running and now owns the release; this guard must NOT also lower the gate.
    fn handed_to_the_batch(self) {
        self.handed_over.set(true);
    }
}

impl Drop for Busy {
    fn drop(&mut self) {
        if !self.handed_over.get() {
            (self.set)(false);
        }
    }
}

// ── talking to the tool ─────────────────────────────────────────────────────────────────────────
/// The `spirit` binary: beside this executable first (an install puts them together), then PATH.
fn spirit_bin() -> PathBuf {
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let cand = dir.join("spirit");
            if cand.is_file() {
                return cand;
            }
        }
    }
    PathBuf::from("spirit")
}

fn run_spirit(args: &[&str]) -> (i32, String) {
    let bin = spirit_bin();
    match Command::new(&bin).args(args).output() {
        Ok(o) => {
            let mut t = String::from_utf8_lossy(&o.stdout).to_string();
            t.push_str(&String::from_utf8_lossy(&o.stderr));
            (o.status.code().unwrap_or(-1), t)
        }
        Err(e) => (-1, format!("could not run {}: {}\n", bin.display(), e)),
    }
}

fn shell_quote(a: &str) -> String {
    if !a.is_empty() && a.chars().all(|c| c.is_ascii_alphanumeric() || "-_./:=+@".contains(c)) {
        a.to_string()
    } else {
        format!("'{}'", a.replace('\'', "'\\''"))
    }
}

/// What happened to one child.
enum Ran {
    Code(i32),
    /// it ran, but the OS gave no exit code — killed by a signal
    Signalled,
    /// it never started, and `run_one` has already said why in the pane
    NotStarted,
}

/// ★ ONE CHILD, STREAMED — THE ONLY PLACE A JOB IS SPAWNED. `label` goes in front of every line, or is empty
/// for a single job, where a prefix would be noise. Both reader threads are joined before this returns, so
/// a caller's closing line can never be overtaken by output still in flight. The wording of the closing
/// line stays with the caller: the two callers say it differently, and neither is this function's business.
fn run_one(tx: &async_channel::Sender<String>, label: &str, shown: &str, argv: &[String]) -> Ran {
    let _ = tx.send_blocking(format!("$ spirit {}\n", shown));
    let prefix = if label.is_empty() { String::new() } else { format!("[{}] ", label) };
    let bin = spirit_bin();
    let mut child = match Command::new(&bin)
        .args(argv)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            let _ = tx.send_blocking(format!("{}could not run {}: {}\n", prefix, bin.display(), e));
            return Ran::NotStarted;
        }
    };
    let mut handles = Vec::new();
    for stream in [child.stdout.take().map(Stream::Out), child.stderr.take().map(Stream::Err)]
        .into_iter()
        .flatten()
    {
        let (t, p) = (tx.clone(), prefix.clone());
        handles.push(std::thread::spawn(move || {
            let rd: Box<dyn std::io::Read + Send> = match stream {
                Stream::Out(r) => Box::new(r),
                Stream::Err(r) => Box::new(r),
            };
            for line in BufReader::new(rd).lines().map_while(Result::ok) {
                let _ = t.send_blocking(format!("{}{}", p, line));
            }
        }));
    }
    let code = child.wait().ok().and_then(|s| s.code());
    for h in handles {
        let _ = h.join();
    }
    match code {
        Some(c) => Ran::Code(c),
        None => Ran::Signalled,
    }
}

/// Just enough of a type to put the two pipes in one loop.
enum Stream {
    Out(std::process::ChildStdout),
    Err(std::process::ChildStderr),
}

/// Mirrors the tool's own definition: the pin is ON when a process can actually receive it. "A file
/// exists" is never the test — a file proves only that someone wrote it.
fn pin_state() -> (bool, bool, String) {
    let live_env = std::env::var("MESA_VK_DEVICE_SELECT").map(|v| !v.is_empty()).unwrap_or(false);
    let live = live_env
        || Command::new("systemctl")
            .args(["--user", "show-environment"])
            .output()
            .ok()
            .map(|o| String::from_utf8_lossy(&o.stdout).lines().any(|l| l.starts_with("MESA_VK_DEVICE_SELECT=")))
            .unwrap_or(false);
    let persisted =
        std::fs::read_to_string(glib::user_config_dir().join("environment.d/90-spirit.conf"))
            .map(|s| s.lines().any(|l| l.starts_with("MESA_VK_DEVICE_SELECT=")))
            .unwrap_or(false);
    let text = match (live, persisted) {
        (true, true) => "On — this session, and after every reboot.",
        (true, false) => "On — this session only. A reboot turns it back off.",
        (false, true) => "On — written for your next login; this running session does not have it yet.",
        (false, false) => "Off — the tool pins only the games it launches itself.",
    };
    (live, persisted, text.to_string())
}

/// Run a command, streaming every line of its output into the output pane on the main thread, and call
/// `on_done` ON THE MAIN THREAD once the process has actually exited. The completion callback is the
/// point: the previous version re-read the switch state on a fixed 1.5 s timer, which could read the OLD
/// state while `enable` was still running and flip the switch back.
fn run_streaming(args: Vec<String>, ui: &Ui, header: &str, on_done: Option<Rc<dyn Fn()>>) {
    let ui = ui.clone();
    let shown: Vec<String> = args.iter().map(|a| shell_quote(a)).collect();
    let (tx, rx) = async_channel::unbounded::<String>();
    let (h, a) = (header.to_string(), shown.join(" "));

    (ui.set_busy)(true);
    let tx0 = tx.clone();
    std::thread::spawn(move || {
        match run_one(&tx0, "", &a, &args) {
            Ran::Code(0) => {
                let _ = tx0.send_blocking(format!("— done ({}) —\n", h));
            }
            Ran::Code(c) => {
                let _ = tx0.send_blocking(format!("— exited {} ({}) —\n", c, h));
            }
            Ran::Signalled => {
                let _ = tx0.send_blocking(format!("— killed ({}) —\n", h));
            }
            // `run_one` has already said why; a trailer would be a second telling of the same nothing
            Ran::NotStarted => {}
        }
        tx0.close();
    });

    let done_ui = ui.clone();
    glib::spawn_future_local(async move {
        while let Ok(line) = rx.recv().await {
            done_ui.append(&(line + "\n"));
        }
        (done_ui.set_busy)(false);
        // ★ THE REAL COMPLETION POINT. The first version spawned a SECOND future that polled a shared
        // `busy` flag every 200 ms — a poll dressed as a callback, and two control paths that can desync.
        if let Some(f) = on_done {
            f();
        }
    });
}

/// ★ ONE JOB PER CARD *AT A TIME*, AND NO CHOSEN FILE LEFT UNRUN. `--split` divides ONE job across cards;
/// this is the other shape — every card pulls whole files off one shared queue and works through them, so
/// ten chosen files run across three spares and the rest WAIT THEIR TURN instead of being announced as
/// "not started" for want of a card. (Asked for on 2026-10-12: *"if a user does not list a card(s), then the jobs
/// chosen should be auto dispatched to the extra GPUs accessible."*)
///
/// ★ WHY THE APP DOES THIS AND NOT `spirit play` ON ITS OWN: the command takes one `--work FILE` (as it
/// should — one job, one output, checked on disk). Several files means several commands, so the app runs
/// one per card and shows them together. Each line carries the card it came from, because otherwise two
/// jobs interleave into one unreadable stream — and that prefix is the only thing about the output that is
/// not the tool's own words.
fn run_batch(cards: Vec<String>, files: Vec<String>, ui: &Ui, on_done: Rc<dyn Fn()>) {
    // ★ THIS FUNCTION DEPENDS ON AT LEAST ONE CARD AND THE CALLER GUARANTEES IT — `start_plan` returns
    //   `NoCards` for an empty list, so the `Batch` arm is the only way in. Nothing here defends itself:
    //   `tx` is never closed, so with zero cards no worker exists to drop a sender and the completion
    //   future's `recv()` would never end. Say it out loud rather than leaving it to the caller's memory.
    //   (Outside read, 2026-10-12.)
    debug_assert!(!cards.is_empty(), "run_batch needs at least one card; start_plan is what guarantees it");
    let ui0 = ui.clone();
    (ui.set_busy)(true);
    let (tx, rx) = async_channel::unbounded::<String>();
    let queue = Arc::new(Mutex::new(VecDeque::from(files)));
    // ★ ONE TOKEN PER CARD, so the pane knows every queue is drained rather than guessing from a timer.
    let total = cards.len();
    for card in cards {
        let tx = tx.clone();
        let queue = queue.clone();
        std::thread::spawn(move || {
            loop {
                // the lock is released by this expression, so no worker holds the queue while it works
                let Some(file) = take_next(&queue) else { break };
                let argv = job_argv(&file, &card);
                let shown: Vec<String> = argv.iter().map(|a| shell_quote(a)).collect();
                match run_one(&tx, &card, &shown.join(" "), &argv) {
                    Ran::Code(c) => {
                        let _ = tx.send_blocking(format!("[{}] — finished (rc={}) —", card, c));
                    }
                    Ran::Signalled => {
                        let _ = tx.send_blocking(format!("[{}] — killed —", card));
                    }
                    // ★ GIVE THE FILE BACK BEFORE STOPPING. This card cannot start a job at all, so the
                    //   file it just took has NOT been run — and dropping it here loses it from the count
                    //   the batch reports at the end, which reads the QUEUE. With one card that coincidence
                    //   hid the loss, because nothing was left in the queue anyway; with several cards that
                    //   stop at once, the files they took vanish and the note undercounts by exactly the
                    //   number of cards that broke. Put back, the number is the truth about the batch.
                    //   (Outside read, 2026-10-12: "the number is honest about the queue and dishonest
                    //   about the batch.")
                    //
                    //   ★★ THE `break` IS LOAD-BEARING — DO NOT MAKE IT `continue`. A worker that cannot
                    //   start a job fails ONCE and leaves, so a file handed back can bounce at most once
                    //   per card before every card that would fail has failed: bounded by the card count.
                    //   Change this to `continue` and one file bounces between the failing cards forever,
                    //   which is the runaway the bound in `the_queue_hands_every_file_to_exactly_one_card`
                    //   exists to survive. (Outside read, 2026-10-12.)
                    Ran::NotStarted => {
                        put_back(&queue, file);
                        break;
                    }
                }
            }
            // ★★ THIS MUST STAY THE LAST THING A WORKER SENDS. The reader stops as soon as it has counted
            //    one token per card, so anything sent after a token would be silently dropped. That is not
            //    a latent bug today — it is a constraint on anyone editing this function later. (Outside
            //    read, 2026-10-12.)
            let _ = tx.send_blocking(format!("\u{1}DONE\u{1}{}", card));
        });
    }

    let seen = Rc::new(Cell::new(0usize));
    glib::spawn_future_local(async move {
        while let Ok(line) = rx.recv().await {
            if let Some(rest) = line.strip_prefix('\u{1}') {
                if rest.starts_with("DONE\u{1}") {
                    seen.set(seen.get() + 1);
                    if seen.get() >= total {
                        break;
                    }
                    continue;
                }
            }
            ui0.append(&(line + "\n"));
        }
        // ★ NOTHING DISAPPEARS QUIETLY — that is the whole point of the queue. A worker that cannot start
        //   a job at all breaks out of its loop (`Ran::NotStarted`), so with one card the files still in
        //   the queue are simply never run. The code this replaced had a "N were NOT started" line; the
        //   queue took the line away and forgot to put it back. Said ONCE, at the end, when it is a fact
        //   about the whole batch rather than a guess per card. (Outside read, 2026-10-12.)
        let note = undelivered_note(queue.lock().map(|q| q.len()).unwrap_or(0));
        if !note.is_empty() {
            ui0.append(&note);
        }
        (ui0.set_busy)(false);
        on_done();
    });
}

/// The next file for this worker, or None when the queue is empty. The lock is released by this call, so no
/// card holds the queue while it works. It is a function rather than an expression so the thread-safety of
/// handing each file to exactly one card can be tested without starting a single child process.
fn take_next(queue: &Arc<Mutex<VecDeque<String>>>) -> Option<String> {
    queue.lock().ok().and_then(|mut q| q.pop_front())
}

/// ★ HAND BACK A FILE THAT WAS TAKEN BUT NEVER RUN. A card that cannot start a job at all stops — and the
/// file it had already taken has NOT been run. The batch reports what was never started by reading this
/// queue, so dropping that file makes the number honest about the QUEUE and dishonest about the BATCH:
/// with several cards stopping at once, the number is short by exactly the number that broke. Putting it
/// back is what makes the count the truth. (Outside read, 2026-10-12.)
///
/// This is safe against the count only because of the order in the worker: `put_back` runs BEFORE the
/// worker sends its `\u{1}DONE\u{1}` token, and the completion future reads the queue only after it has
/// counted one token per card. So every put-back has happened before the read. Move either one and the
/// number can be short again.
fn put_back(queue: &Arc<Mutex<VecDeque<String>>>, file: String) {
    if let Ok(mut q) = queue.lock() {
        q.push_front(file);
    }
}

/// The command line for one job: this file, on this card, and no game to wait for. Pure, so the one place
/// that decides what the tool is asked can be asserted rather than read.
fn job_argv(file: &str, card: &str) -> Vec<String> {
    vec![
        "play".into(),
        "--work".into(),
        file.to_string(),
        "--spare".into(),
        card.to_string(),
        "--".into(),
        "true".into(),
    ]
}

/// What the pane says before a batch starts. It says what is about to happen to every chosen file, because
/// the shape changes with the machine: one job each when there are enough cards, and a queue when there are
/// not.
fn batch_header(n_files: usize, n_cards: usize) -> String {
    if n_files <= n_cards {
        format!("— {} job(s), one per card, all at once —\n", n_files)
    } else {
        format!(
            "— {} job(s) across {} card(s): one at a time per card, and the rest wait their turn —\n",
            n_files, n_cards
        )
    }
}

/// What the pane says when a batch ends with files still queued — which happens when a card cannot start
/// a job at all and stops. Empty for the ordinary case, because a line that prints "0 files" on every good
/// batch is noise, and noise is how a real warning gets ignored. Pure, so the sentence is pinned by a test
/// rather than by whoever reads the pane next.
fn undelivered_note(left: usize) -> String {
    if left == 0 {
        String::new()
    } else {
        format!("\n★ {} file(s) were never started — the card stopped before reaching them.\n", left)
    }
}


/// Decide WHICH cards, then start one job per card. Split out so the card lookup can happen off the
/// main thread — the same freeze that was fixed in the card list, and reintroduced here by accident.
///
/// ★ ONE PATH, NAMED OR NOT. The named path used to skip the read entirely and hand the field to the tool
/// verbatim; a name the user typed then reached `--spare` and was refused, and nothing could translate it
/// because the document that describes the cards had not been read. Both paths now take the same read, the
/// same gate, and the same release. (From a real run, 2026-10-12.)
fn cards_for_jobs(
    files: Vec<String>,
    named: Vec<String>,
    main_override: Option<String>,
    ui: &Ui,
) {
    // ★ OFF THE MAIN THREAD. `probe` touches hardware. Calling it inline here froze the window on
    //   exactly the machine that most needed to show something — the same fault the card list was
    //   moved off-thread for. (Caught by the outside read, 2026-10-12.)
    let gate = Busy::raise(ui);
    let (tx, rx) = async_channel::bounded::<(String, String, String)>(1);
    std::thread::spawn(move || {
        let p = run_spirit(&["probe", "--json"]).1;
        let r = run_spirit(&["roles", "--json"]).1;
        let v = run_spirit(&["--version"]).1;
        let _ = tx.send_blocking((p, r, v));
    });
    let ui2 = ui.clone();
    glib::spawn_future_local(async move {
        let (pt, rt, vt) = rx.recv().await.unwrap_or_default();

        // ★ IF THE TOOL IS OLDER THAN THIS APP, SAY SO INSTEAD OF BLAMING THE MACHINE. A `spirit` that does
        //   not report capability at all used to produce "No card can take a job here" on a machine with two
        //   perfectly good spare cards — a true sentence that sent the reader looking for a card that was
        //   there. The pair is installed together, so a mismatch means one of them was replaced alone.
        //   (This exact failure happened here on 2026-10-12: the app was 1.1.0 and the command was 1.0.0.)
        if !probe_reports_capability(&pt) {
            ui2.append(&format!(
                "— the `spirit` this app ran did not report card capability at all. {}. Re-run the \
                 installer so the app and the command are the same version; until then nothing can be \
                 started, and that is the only reason. —\n",
                if vt.trim().is_empty() { "It did not answer `--version` either".to_string() }
                else { format!("It reports itself as `{}`", vt.trim()) }
            ));
            return; // `gate` drops here — nothing was started, and the window stays usable
        }

        let probe: serde_json::Value =
            serde_json::from_str(&pt).unwrap_or(serde_json::Value::Null);
        let roles: serde_json::Value =
            serde_json::from_str(&rt).unwrap_or(serde_json::Value::Null);

        let (cards, no_node) = if named.is_empty() {
            // THE DEFAULT: every card the tool calls free, in the tool's own order. Nothing has to be
            // listed for a job to be dispatched; naming cards only ADDS the ones the tool leaves out.
            pick_cards(&pt, &rt, main_override.as_deref())
        } else {
            let (nodes, unmatched) = resolve_named(&named, &probe, &roles);
            if !unmatched.is_empty() {
                let all: Vec<String> = card_names(&probe, &roles)
                    .iter()
                    .map(|(_, name, node)| format!("{} ({})", name, node))
                    .collect();
                ui2.append(&format!(
                    "— nothing here matches {}. The cards are: {}. —\n",
                    unmatched.join(", "), all.join(" · ")
                ));
            }
            (nodes, 0)
        };

        if named.is_empty() {
            if no_node > 0 {
                ui2.append(&format!(
                    "— {} card(s) can encode but reported no render node, so they were not used. Said here \
                     rather than dropped. —\n", no_node));
            }
        } else if cards.is_empty() {
            // The generic "no card here" line would be false: the user named cards and the names were the
            // problem. Say the true thing and stop; `gate` drops and the window is usable again.
            ui2.append("— none of the names you gave matched a card, so nothing was started. —\n");
            return;
        }

        // ★ THE GATE IS HANDED IN, NOT REMEMBERED. It was raised as a `Busy` value before the probe
        //   thread started; `start_jobs` either lowers it by dropping it (nothing was launched) or hands
        //   it to the batch, which owns the release from then on. There is no boolean to get wrong and no
        //   `if !... { release }` for a later edit to drop. (Outside read, 2026-10-12.)
        start_jobs(files, cards, &ui2, Some(gate));
    });
}

/// Did the tool answer the one question this app cannot work without? `can_encode` is how the card list and
/// the batch both decide whether a card can take a job; a document with cards but no such key at all is an
/// older `spirit`, not a machine with no capable cards. (Measured here, 2026-10-12.)
fn probe_reports_capability(probe_json: &str) -> bool {
    let probe: serde_json::Value =
        serde_json::from_str(probe_json).unwrap_or(serde_json::Value::Null);
    match probe.get("gpus").and_then(|g| g.as_array()) {
        Some(arr) if !arr.is_empty() => arr.iter().any(|g| g.get("can_encode").is_some()),
        _ => false,
    }
}

/// Every card the tool reports as driving a display. MORE THAN ONE (or none) means the answer is not
/// automatically apparent — two identical cards with a monitor each is a real machine shape — and the app
/// will not guess which one the game is on.
///
/// ★ NOTHING HERE IS DROPPED, AND THAT IS DELIBERATE. This used to build its list with `filter_map`, so a
/// display entry with no `card` key vanished — and a held-back set that silently shrinks is the one failure
/// this picker exists to prevent: the card would not be held back and could be offered a job. The id is
/// carried as `""` instead. Because `usability` reads a missing card id as `""` too, the two MATCH, so such
/// a card is held back rather than offered work. The safety is then a property of the code, not of the
/// documents this machine happens to produce. (Outside read, 2026-10-12: "safe by data, not safe by
/// construction".)
fn display_cards(roles: &serde_json::Value) -> Vec<String> {
    roles
        .as_array()
        .map(|a| {
            a.iter()
                .filter(|r| {
                    r.get("roles").and_then(|v| v.as_array())
                        .map(|x| x.iter().any(|y| y.as_str() == Some("display")))
                        .unwrap_or(false)
                })
                .map(|r| r.get("card").and_then(|v| v.as_str()).unwrap_or("").to_string())
                .collect()
        })
        .unwrap_or_default()
}

/// ★ WHICH CARDS ARE HELD BACK — every card the tool reports as driving a display, not just the first.
/// The old rule took the first display entry and offered every OTHER display card a job, and the window
/// greyed out all of them; the two disagreed. On a machine with two monitors on two identical cards — the
/// case this picker exists for — "hold back the first" means handing a job to the card running the game.
/// So: the tool's answer holds back ALL of them, and the user's choice holds back exactly the one they say
/// is theirs. Nothing is ever started on a card the user has named as the one they are playing on.
fn held_back(roles: &serde_json::Value, chosen: Option<&str>) -> Vec<String> {
    match chosen {
        Some(card) if !card.is_empty() => vec![card.to_string()],
        _ => display_cards(roles),
    }
}

/// What each card is called on screen: the tool's own name for it, else its card id.
fn card_names(probe: &serde_json::Value, roles: &serde_json::Value) -> Vec<(String, String, String)> {
    let empty = vec![];
    let gpus = probe.get("gpus").and_then(|g| g.as_array()).unwrap_or(&empty);
    let rlist = roles.as_array().unwrap_or(&empty);
    gpus.iter()
        .map(|g| {
            let card = g.get("card").and_then(|v| v.as_str()).unwrap_or("").to_string();
            let name = rlist
                .iter()
                .find(|r| r.get("card").and_then(|v| v.as_str()) == Some(card.as_str()))
                .and_then(|r| r.get("name"))
                .or_else(|| g.get("name"))
                .and_then(|v| v.as_str())
                .unwrap_or(card.as_str())
                .to_string();
            let node = g.get("render_node").and_then(|v| v.as_str()).unwrap_or("").to_string();
            (card, name, node)
        })
        .collect()
}

/// ★ A NAME THE USER TYPED IS A NAME THE TOOL SHOWS. The "Extra cards" field used to be handed to the
/// tool verbatim, so typing the card's own name — `MI25`, exactly as the list prints it — reached
/// `spirit play --spare MI25`, and the tool refused it: *"ignoring unknown render node(s): MI25"*. The
/// field wanted a render node and nothing said so. Now a token is matched, in order, against the render
/// node, the card id, and the card's name (case-insensitive substring). Anything unmatched comes back so
/// it can be SAID rather than silently dropped. (From a real run, 2026-10-12.)
fn resolve_named(
    named: &[String],
    probe: &serde_json::Value,
    roles: &serde_json::Value,
) -> (Vec<String>, Vec<String>) {
    let catalogue = card_names(probe, roles);
    let mut nodes: Vec<String> = Vec::new();
    let mut unmatched: Vec<String> = Vec::new();
    for tok in named {
        let t = tok.trim();
        if t.is_empty() {
            continue;
        }
        let lower = t.to_lowercase();
        let hit = catalogue
            .iter()
            .find(|(_, _, node)| !node.is_empty() && node == t)
            .or_else(|| catalogue.iter().find(|(card, _, _)| card == t))
            .or_else(|| catalogue.iter().find(|(_, name, _)| name.to_lowercase().contains(&lower)));
        match hit {
            Some((_, _, node)) if !node.is_empty() => nodes.push(node.clone()),
            _ => unmatched.push(t.to_string()),
        }
    }
    (nodes, unmatched)
}

/// ★ WHAT THE TOOL SAYS ABOUT A CARD, IN ONE PLACE. The window and the batch read the same fields with
/// different type assumptions: the pane treated `marginal` as a string (so any other JSON type fell through
/// to "available for jobs"), while the batch required it to be null. A card marked marginal with anything
/// but a string was therefore SHOWN as available and silently refused — the window saying one thing and the
/// machine doing another, which is the one thing this app is built not to do. One definition, both users.
/// (Outside read, 2026-10-12.)
enum Use {
    /// this card drives the monitor; the game is on it
    Main,
    /// the tool did not report `can_encode` at all — say so, never guess
    Unknown,
    /// the tool says there is no VAAPI encoder for the driver named
    CannotEncode(String),
    /// usable only if the user names it with `--spare`
    Marginal(String),
    /// can take a job
    Free,
}

fn usability(g: &serde_json::Value, held: &[String]) -> Use {
    let card = g.get("card").and_then(|v| v.as_str()).unwrap_or("");
    if held.iter().any(|h| h == card) {
        return Use::Main;
    }
    match g.get("can_encode") {
        None => Use::Unknown,
        Some(v) if v.as_bool() != Some(true) => Use::CannotEncode(
            g.get("driver").and_then(|d| d.as_str()).unwrap_or("unknown").to_string(),
        ),
        Some(_) => match g.get("marginal") {
            // ★ PRESENT AND NOT NULL IS THE TEST — not "is a string". A reason the tool phrased as
            // anything else still means the card is left out, and it is still shown as left out.
            Some(m) if !m.is_null() => Use::Marginal(
                m.as_str().map(|s| s.to_string()).unwrap_or_else(|| m.to_string()),
            ),
            Some(_) => Use::Free,
            // ★ THE KEY IS MISSING ENTIRELY. The tool emits `marginal` on every card, so this is a document
            //   this app does not understand — say "unknown" rather than assuming the card is free. The old
            //   batch rule refused in this case; treating it as free would have silently relaxed that.
            None => Use::Unknown,
        },
    }
}

/// What the card list says about a card, and how it is toned — in the same place as the verdict itself, so
/// the line the user reads and the choice the machine makes come from one value. Returns
/// (subtitle, css class, is-this-the-display-card). Pure, and thus testable without a window.
fn describe(u: Use) -> (String, &'static str, bool) {
    match u {
        // true whether the tool named one display card or several
        Use::Main => (
            "drives your monitor — nothing is ever started on this card".to_string(),
            "dim-label",
            true,
        ),
        // the tool did not answer: say so, rather than offering the card
        Use::Unknown => (
            "unknown — the tool did not report this card".to_string(),
            "warning",
            false,
        ),
        Use::CannotEncode(driver) => (
            format!(
                "cannot take a job: no VAAPI encoder for “{}”",
                if driver.is_empty() { "unknown" } else { driver.as_str() }
            ),
            "error",
            false,
        ),
        Use::Marginal(why) => (
            format!("left out by default — {} (name it with --spare)", why),
            "warning",
            false,
        ),
        Use::Free => ("available for jobs".to_string(), "success", false),
    }
}

/// ★ WHICH CARDS CAN TAKE A JOB — a filter over the ONE definition above, read from the tool's own
/// answers and never re-derived here. Pure: no GTK, no threads, no hardware, so a test can hand it the
/// exact bytes a dead worker produces and pin what happens.
///
/// Returns the render nodes in the tool's own order, and how many USABLE cards reported no render node —
/// counted, never dropped silently, which is the one thing this app is built not to do.
fn pick_cards(probe_json: &str, roles_json: &str, main_override: Option<&str>) -> (Vec<String>, usize) {
    let probe: serde_json::Value =
        serde_json::from_str(probe_json).unwrap_or(serde_json::Value::Null);
    let roles: serde_json::Value =
        serde_json::from_str(roles_json).unwrap_or(serde_json::Value::Null);
    // ★ THE USER'S CHOICE WINS OVER THE TOOL'S GUESS. The tool reads the kernel, which is the best answer
    //   available — but it cannot say which of two identical cards the game is actually on, so when the
    //   user picks one, that is the answer used by BOTH the card list and the batch.
    let held = held_back(&roles, main_override);
    let mut cards: Vec<String> = Vec::new();
    let mut no_node = 0usize;
    if let Some(arr) = probe.get("gpus").and_then(|g| g.as_array()) {
        for g in arr {
            if !matches!(usability(g, &held), Use::Free) {
                continue;
            }
            match g.get("render_node").and_then(|v| v.as_str()) {
                Some(n) => cards.push(n.to_string()),
                None => no_node += 1,
            }
        }
    }
    (cards, no_node)
}

/// What starting a batch needs from the window, as a trait so a test can hand it a recorder instead of a
/// GTK window. Everything the gate depends on is asserted against a fake pane, with no display and no
/// hardware — which is the whole point of splitting it out.
trait Pane {
    fn note(&self, text: &str);
    fn start_batch(&self, cards: Vec<String>, files: Vec<String>);
}

impl Pane for Ui {
    fn note(&self, text: &str) {
        self.append(text);
    }
    fn start_batch(&self, cards: Vec<String>, files: Vec<String>) {
        run_batch(cards, files, self, Rc::new(|| {}));
    }
}

/// What the gate decision hangs on, with no window in it. `NoCards` means nothing was launched, so the
/// guard must lower the gate; `Batch` carries the line to print and means the batch owns the release.
enum Start {
    NoCards,
    Batch(String),
}

fn start_plan(files: &[String], cards: &[String]) -> Start {
    if cards.is_empty() {
        return Start::NoCards;
    }
    Start::Batch(batch_header(files.len(), cards.len()))
}

/// One job per card at a time, every chosen file run, and what will not run is SAID.
///
/// ★ THE GATE IS A VALUE, NOT A RETURN CODE. `gate` was raised by the caller and is lowered here by being
/// DROPPED, unless a batch was launched — in which case it is handed over and the batch releases it when
/// every card's queue is drained. The fault this replaces was an ownership question answered by a comment
/// and a bool between two functions; get it wrong and clicking Run on a machine with no usable spare card
/// locked Run, Doctor, both switches and the card field for the life of the window — the ordinary case, not
/// an exotic one. There is nothing here for a later edit to forget. (Outside reads, 2026-10-12.)
fn start_jobs(files: Vec<String>, cards: Vec<String>, pane: &dyn Pane, gate: Option<Busy>) {
    match start_plan(&files, &cards) {
        Start::NoCards => {
            pane.note(
                "No card can take a job here. Either there is no spare card, or the spares are on narrow \
                 links — name one in \"Extra cards\" to use it anyway.\n",
            );
            // ★ NOTHING IS DONE WITH `gate` HERE, and that is the point — it is a parameter, so it drops
            //   when this function RETURNS, and dropping is what lowers the gate. (An earlier version of
            //   this comment said "at the end of the arm"; a parameter is not scoped to an arm, and a
            //   comment that misdescribes the mechanism is the fault this type was built to remove.)
        }
        Start::Batch(header) => {
            pane.note(&header);
            pane.start_batch(cards, files);
            if let Some(g) = gate {
                g.handed_to_the_batch();
            }
        }
    }
}

// ── the window ───────────────────────────────────────────────────────────────────────────────────
fn build_ui(app: &adw::Application) {
    let window = adw::ApplicationWindow::builder()
        .application(app)
        .title("Spiritual Successor")
        .default_width(780)
        .default_height(880)
        .build();

    let header = adw::HeaderBar::new();
    let refresh = gtk::Button::from_icon_name("view-refresh-symbolic");
    refresh.set_tooltip_text(Some("Re-read this machine's cards"));
    header.pack_start(&refresh);

    let page = adw::PreferencesPage::new();

    // ---- your cards ------------------------------------------------------------------------------
    let cards = adw::PreferencesGroup::builder()
        .title("Your cards")
        .description("Read from this machine by the tool itself. One card drives your monitor; the \
                      others can take jobs.")
        .build();

    // ★ WHICH CARD IS YOURS — the default is not always apparent, so it is the user's to set.
    //   The tool reads the kernel and reports which card drives a display, and that is the best answer
    //   available. But a machine can have two identical cards with a monitor on each, and then the kernel
    //   cannot say which one the game is actually on — nor can this app. So the choice is offered here,
    //   it applies to BOTH the card list and the batch, and it is remembered for this session only: a
    //   reboot forgets it, the same as every other switch in this window. (2026-10-12.)
    let main_row = adw::ComboRow::builder()
        .title("Which card drives your monitor")
        .subtitle("read from the tool")
        .build();
    // index → card id; index 0 means "whatever the tool says", which is stored as "" and read as None.
    let main_choices: Rc<RefCell<Vec<String>>> = Rc::new(RefCell::new(vec![String::new()]));
    let main_suppress: Rc<Cell<bool>> = Rc::new(Cell::new(false));
    let main_override: Rc<RefCell<Option<String>>> = Rc::new(RefCell::new(None));
    {
        let main_choices = main_choices.clone();
        let main_override = main_override.clone();
        let main_suppress = main_suppress.clone();
        main_row.connect_selected_notify(move |row| {
            // ★ REBUILDING THE MODEL FIRES THIS TOO. Without the guard the app would rewrite the user's
            //   choice from a half-updated list every time the cards were re-read.
            if main_suppress.get() {
                return;
            }
            let i = row.selected() as usize;
            let val = main_choices.borrow().get(i).cloned().unwrap_or_default();
            *main_override.borrow_mut() = if val.is_empty() { None } else { Some(val) };
        });
    }
    cards.add(&main_row);

    // ---- the switch ------------------------------------------------------------------------------
    let on_group = adw::PreferencesGroup::builder()
        .title("Keep it on")
        .description("By default the tool pins only the game it launches itself. Turn this on and every \
                      game you start renders on the card your monitor is plugged into. Nothing is armed \
                      at boot unless you ask.")
        .build();
    let state_row = adw::ActionRow::builder().title("State").build();
    let switch_row = adw::SwitchRow::builder()
        .title("Keep it on")
        .subtitle("this session only — a reboot turns it back off")
        .build();
    let persist_row = adw::ActionRow::builder()
        .title("Also after a reboot")
        .subtitle("read ONLY when you turn the switch on above; turning it off removes both")
        .build();
    let persist_check = gtk::CheckButton::new();
    persist_check.set_valign(gtk::Align::Center);
    persist_row.add_suffix(&persist_check);
    persist_row.set_activatable_widget(Some(&persist_check));
    on_group.add(&state_row);
    on_group.add(&switch_row);
    on_group.add(&persist_row);

    // ---- a job -----------------------------------------------------------------------------------
    let job_group = adw::PreferencesGroup::builder()
        .title("Give your other cards a job")
        .description("Pick a video file. The cards that can take a job encode it on their own hardware \
                      encoders, the slices are joined back into one file, and every output is checked on \
                      disk afterwards. Any card left out is named.")
        .build();
    let file_row = adw::ActionRow::builder()
        .title("Files")
        .subtitle("none chosen — choose one or several; one job goes to each spare card")
        .build();
    let choose = gtk::Button::with_label("Choose…");
    choose.set_valign(gtk::Align::Center);
    file_row.add_suffix(&choose);
    let run_row = adw::ActionRow::builder().title("Run the job").build();
    let run = gtk::Button::with_label("Use my other cards");
    run.set_valign(gtk::Align::Center);
    run.set_sensitive(false);
    run_row.add_suffix(&run);
    // ★ A FIELD FOR CARD NAMES, because the README tells you to name a card and the window gave you no way
    //   to do it. Cards on a narrow link are left out by default on purpose; naming them here is how you
    //   say "yes, that one, I know". Empty means "use whatever the tool considers safe", which is the
    //   default everywhere else.
    let spare_row = adw::ActionRow::builder()
        .title("Extra cards")
        .subtitle("optional — name cards the default leaves out, e.g. renderD130")
        .build();
    let spare_entry = gtk::Entry::builder().placeholder_text("renderD131,renderD130").width_chars(22).build();
    spare_entry.set_valign(gtk::Align::Center);
    spare_row.add_suffix(&spare_entry);
    job_group.add(&file_row);
    job_group.add(&run_row);
    job_group.add(&spare_row);

    // ---- doctor ----------------------------------------------------------------------------------
    let doc_group = adw::PreferencesGroup::builder()
        .title("Check my machine")
        .description("Runs a real program on every card and reports what it found. Give it a minute.")
        .build();
    let doc_row = adw::ActionRow::builder().title("Doctor").build();
    let doctor = gtk::Button::with_label("Run doctor");
    doctor.set_valign(gtk::Align::Center);
    doc_row.add_suffix(&doctor);
    doc_group.add(&doc_row);

    page.add(&cards);
    page.add(&on_group);
    page.add(&job_group);
    page.add(&doc_group);

    // ---- output ----------------------------------------------------------------------------------
    let buf = gtk::TextBuffer::new(None);
    let view = gtk::TextView::with_buffer(&buf);
    view.set_editable(false);
    view.set_monospace(true);
    view.set_wrap_mode(gtk::WrapMode::WordChar);
    view.set_top_margin(8);
    view.set_bottom_margin(8);
    view.set_left_margin(8);
    view.set_right_margin(8);
    let out_scroll = gtk::ScrolledWindow::builder()
        .hscrollbar_policy(gtk::PolicyType::Never)
        .min_content_height(220)
        .child(&view)
        .build();
    let out_frame = gtk::Frame::new(None);
    out_frame.set_child(Some(&out_scroll));
    let out_group = adw::PreferencesGroup::builder().title("Output").build();
    out_group.add(&out_frame);
    page.add(&out_group);

    let scroller =
        gtk::ScrolledWindow::builder().hscrollbar_policy(gtk::PolicyType::Never).child(&page).build();
    let toolbar = adw::ToolbarView::new();
    toolbar.add_top_bar(&header);
    toolbar.set_content(Some(&scroller));
    window.set_content(Some(&toolbar));

    // ── behaviour ────────────────────────────────────────────────────────────────────────────────
    let suppress = Rc::new(Cell::new(false));
    let busy = Rc::new(Cell::new(false));
    let has_file = Rc::new(Cell::new(false));

    // ★ ONE PLACE THAT DECIDES WHAT CAN BE PRESSED. Nothing here was disabled before a run, so a job
    // could be started twice — two transcodes writing the same output path and interleaving in one pane.
    let set_busy: Rc<dyn Fn(bool)> = {
        let (b, d, r, rr, s1, s2, pc, se) =
            (refresh.clone(), doctor.clone(), run.clone(), run_row.clone(), switch_row.clone(),
             persist_row.clone(), persist_check.clone(), spare_entry.clone());
        let busy = busy.clone();
        let has_file = has_file.clone();
        Rc::new(move |v: bool| {
            busy.set(v);
            b.set_sensitive(!v);
            d.set_sensitive(!v);
            s1.set_sensitive(!v);
            s2.set_sensitive(!v);
            rr.set_sensitive(!v);
            pc.set_sensitive(!v);
            // ★ THE CARD-NAME FIELD IS PART OF THE GATE TOO. It was left out, so while a job ran every
            //   other control greyed out and this one stayed live: the names of the job already running
            //   could be edited, leaving the window describing something the machine was not doing.
            //   (Found by the outside read, 2026-10-12.)
            se.set_sensitive(!v);
            r.set_sensitive(!v && has_file.get());
        })
    };

    let mark = buf.create_mark(None, &buf.end_iter(), false);
    let ui =
        Ui { buf: buf.clone(), view: view.clone(), mark, set_busy: set_busy.clone() };

    // cards
    let card_rows: Rc<RefCell<Vec<adw::ActionRow>>> = Rc::new(RefCell::new(Vec::new()));
    // ★ A GENERATION, so overlapping fills cannot double the card list (outside read, 2026-10-12). The
    // drain used to run at ENTRY: a second fill starting before the first finished meant neither
    // re-drained after the other populated, and the user got two sets of rows. Now only the NEWEST fill
    // renders, and the drain happens immediately before the rows go in — so the invariant holds even if
    // two ever overlap.
    let fill_gen: Rc<Cell<u64>> = Rc::new(Cell::new(0));
    let fill_cards: Rc<dyn Fn(Option<Rc<dyn Fn()>>)> = {
        let cards = cards.clone();
        let card_rows = card_rows.clone();
        let fill_gen = fill_gen.clone();
        let main_row = main_row.clone();
        let main_choices = main_choices.clone();
        let main_suppress = main_suppress.clone();
        let main_override = main_override.clone();
        Rc::new(move |on_done: Option<Rc<dyn Fn()>>| {
            let my = fill_gen.get().wrapping_add(1);
            fill_gen.set(my);
            // ★ OFF THE MAIN THREAD (outside read, 2026-10-12). `probe` touches hardware, and running it
            // synchronously froze the window on exactly the machine that most needed to show something —
            // the busy gate there was theatre, since the main loop never turned over between true/false.
            let (tx, rx) = async_channel::bounded::<(String, String)>(1);
            std::thread::spawn(move || {
                let p = run_spirit(&["probe", "--json"]).1;
                let r = run_spirit(&["roles", "--json"]).1;
                let _ = tx.send_blocking((p, r));
            });
            let cards = cards.clone();
            let card_rows = card_rows.clone();
            let fill_gen_in = fill_gen.clone();
            let main_row_in = main_row.clone();
            let main_choices_in = main_choices.clone();
            let main_suppress_in = main_suppress.clone();
            let main_override_in = main_override.clone();
            glib::spawn_future_local(async move {
            if let Ok((pt, rt)) = rx.recv().await {
            if fill_gen_in.get() == my {
            for r in card_rows.borrow_mut().drain(..) {
                cards.remove(&r);
            }
            let probe: serde_json::Value = serde_json::from_str(&pt).unwrap_or(serde_json::Value::Null);
            let roles: serde_json::Value = serde_json::from_str(&rt).unwrap_or(serde_json::Value::Null);
            let empty = vec![];
            let gpus = probe.get("gpus").and_then(|g| g.as_array()).unwrap_or(&empty).clone();
            let rlist = roles.as_array().unwrap_or(&empty).clone();
            // ★ ONE DEFINITION OF "WHAT THIS CARD CAN DO", shared with the batch. The two used to read
            // `marginal` with different type assumptions, so a card could be SHOWN as "available for jobs"
            // and silently refused when the user pressed Run. (Outside read, 2026-10-12.)
            let claimed = display_cards(&roles);
            let auto = if claimed.len() == 1 { claimed[0].clone() } else { String::new() };
            let chosen = main_override_in.borrow().clone();
            let held = held_back(&roles, chosen.as_deref());

            // ★ REBUILD THE PICKER from the document just read. The user's choice is kept by card id while
            //   that card is still here; option 0 is always "whatever the tool says", so clearing the choice
            //   is possible without a second control. The subtitle says plainly when the tool could not tell.
            {
                let keep = main_override_in.borrow().clone();
                let mut ids: Vec<String> = vec![String::new()];
                let mut labels: Vec<String> = vec![match claimed.len() {
                    1 => format!("The tool's own answer — {}", auto),
                    _ => "The tool's own answer".to_string(),
                }];
                for (card, name, node) in card_names(&probe, &roles) {
                    ids.push(card);
                    labels.push(format!(
                        "{} · {}",
                        name,
                        if node.is_empty() { "no render node".to_string() } else { node }
                    ));
                }
                main_suppress_in.set(true);
                *main_choices_in.borrow_mut() = ids.clone();
                let refs: Vec<&str> = labels.iter().map(|s| s.as_str()).collect();
                main_row_in.set_model(Some(&gtk::StringList::new(&refs)));
                let sel = ids.iter().position(|c| Some(c.clone()) == keep).unwrap_or(0);
                main_row_in.set_selected(sel as u32);
                main_suppress_in.set(false);
                main_row_in.set_subtitle(&match claimed.len() {
                    0 => "the tool named no card as driving a display — pick the one your game runs on"
                        .to_string(),
                    1 => format!("read from the tool: {} — nothing is ever started on this card", auto),
                    n => format!(
                        "{} cards report a display, so the tool cannot tell which one your game is on — \
                         pick it here", n
                    ),
                });
            }

            for g in &gpus {
                let card = g.get("card").and_then(|v| v.as_str()).unwrap_or("");
                let role = rlist.iter().find(|r| r.get("card").and_then(|v| v.as_str()) == Some(card));
                let name = role
                    .and_then(|r| r.get("name"))
                    .or_else(|| g.get("name"))
                    .and_then(|v| v.as_str())
                    .unwrap_or(card)
                    .to_string();
                let link = g.get("link").cloned().unwrap_or(serde_json::Value::Null);
                let width = link.get("max_width").and_then(|v| v.as_i64());
                let gbps = link.get("gbps_max").and_then(|v| v.as_f64());
                let gen = link.get("max_gen").and_then(|v| v.as_str()).unwrap_or("");

                // ★ THE TOOL'S OWN ANSWER, not a driver-name guess and not a hardcoded threshold.
                let (doing, tone, is_main) = describe(usability(g, &held));

                let row = adw::ActionRow::builder()
                    .title(glib::markup_escape_text(&name).as_str())
                    .subtitle(format!(
                        "x{} {} · {} GB/s · {}",
                        width.map(|w| w.to_string()).unwrap_or_else(|| "?".into()),
                        if gen.is_empty() { "?" } else { gen },
                        gbps.map(|g| format!("{:.1}", g)).unwrap_or_else(|| "?".into()),
                        glib::markup_escape_text(&doing),
                    ))
                    .build();
                // ★ A LABEL'S CSS CLASS, NOT MARKUP (fixed after the first run): Pango markup does not
                // allow `class` on a `<span>`, so the tags rendered as broken text with a Gtk-WARNING on
                // every row.
                let tag = gtk::Label::new(Some(if is_main { "main" } else { "spare" }));
                tag.add_css_class(tone);
                tag.set_valign(gtk::Align::Center);
                row.add_suffix(&tag);
                cards.add(&row);
                card_rows.borrow_mut().push(row);
            }
            }  // end `if fill_gen.get() == my`
            }
            // ★ OUTSIDE THE `recv` GUARD (outside read, E1). With `on_done` inside `if let Ok(...)`, a
            // worker that failed or a closed channel never released the busy gate — Refresh, Doctor and
            // the switches would be dead for the life of the window with no way back.
            if let Some(f) = on_done {
                f();
            }
            });
        })
    };

    // state line
    let refresh_state: Rc<dyn Fn()> = {
        let state_row = state_row.clone();
        let switch_row = switch_row.clone();
        let persist_check = persist_check.clone();
        let suppress = suppress.clone();
        Rc::new(move || {
            let (live, persisted, text) = pin_state();
            state_row.set_subtitle(&glib::markup_escape_text(&text));
            suppress.set(true);
            // ★ THE SWITCH IS THE SESSION PIN; THE BOX IS THE PERSISTED FILE. Binding the switch to
            // `live || persisted` showed "on" for a pin this session does not have — which is the one
            // thing this app is built not to do.
            switch_row.set_active(live);
            persist_check.set_active(persisted);
            suppress.set(false);
        })
    };
    refresh_state();

    // the switch — one power control; the reboot box is a modifier read at the moment of turning ON.
    {
        let ui = ui.clone();
        let refresh_state = refresh_state.clone();
        let suppress2 = suppress.clone();
        let persist_check = persist_check.clone();
        switch_row.connect_active_notify(move |sw| {
            if suppress2.get() {
                return;
            }
            let mut args = vec![];
            if sw.is_active() {
                args.push("enable".to_string());
                if persist_check.is_active() {
                    args.push("--persist".to_string());
                }
            } else {
                args.push("disable".to_string());
            }
            // the state is re-read in the run's OWN completion callback — after the child has exited
            run_streaming(args, &ui, "on/off switch", Some(refresh_state.clone()));
        });
    }
    // ★ THE REBOOT BOX DOES NOTHING ON ITS OWN. It used to run `disable` when un-ticked, which turned the
    // WELL as the session pin — a second power switch wearing a modifier's label. Its value is read when
    // the switch above is turned on, and nowhere else.
    persist_check.connect_toggled(|_| {});

    // choose a file
    {
        let file_row = file_row.clone();
        let chosen: Rc<RefCell<Vec<String>>> = Rc::new(RefCell::new(Vec::new()));
        let chosen2 = chosen.clone();
        let window2 = window.clone();
        let has_file2 = has_file.clone();
        let set_busy2 = set_busy.clone();
        let busy2 = busy.clone();
        choose.connect_clicked(move |_| {
            // ★ SEVERAL FILES AT ONCE. `open` returns one file; a batch needs `open_multiple`.
            let dlg = gtk::FileDialog::builder().title("Choose one or more video files").build();
            let file_row = file_row.clone();
            let chosen3 = chosen2.clone();
            let has_file3 = has_file2.clone();
            let set_busy3 = set_busy2.clone();
            let busy3 = busy2.clone();
            dlg.open_multiple(Some(&window2), gio::Cancellable::NONE, move |res| {
                if let Ok(list) = res {
                    let mut picked: Vec<String> = Vec::new();
                    let mut skipped = 0usize;
                    for i in 0..list.n_items() {
                        let Some(obj) = list.item(i) else { continue };
                        let Ok(f) = obj.downcast::<gio::File>() else { continue };
                        if let Some(p) = f.path() {
                            if p.is_file() {
                                picked.push(p.display().to_string());
                            } else {
                                skipped += 1;   // a directory; counted, never silently ignored
                            }
                        }
                    }
                    if picked.is_empty() {
                        file_row.set_subtitle("nothing usable chosen — that was not a file");
                        *chosen3.borrow_mut() = Vec::new();
                        has_file3.set(false);
                    } else {
                        let shown = if picked.len() == 1 {
                            picked[0].clone()
                        } else {
                            format!("{} files chosen — first is {}", picked.len(), picked[0])
                        };
                        file_row.set_subtitle(&glib::markup_escape_text(&shown));
                        *chosen3.borrow_mut() = picked;
                        has_file3.set(true);
                    }
                    let _ = skipped;
                    // ★ RE-APPLY THE ONE GATE, never force a control on. Calling set_busy(false) here
                    // re-enabled Run, Doctor and the switches MID-JOB (outside read, 2026-10-12).
                    (set_busy3)(busy3.get());
                }
            });
        });
        let ui2 = ui.clone();
        let spare_entry = spare_entry.clone();
        let main_override_run = main_override.clone();
        run.connect_clicked(move |_| {
            let files = chosen.borrow().clone();
            if files.is_empty() {
                return;
            }
            // ★ ONE NAME PER PIECE, TRIMMED, EMPTIES DROPPED: a trailing or doubled comma must not hand
            //   the tool an empty token. Named cards are used exactly as given, in order, and the card
            //   lookup for the unnamed case happens off the main thread inside `cards_for_jobs`.
            let named: Vec<String> = spare_entry
                .text()
                .split(',')
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
                .collect();
            cards_for_jobs(files, named, main_override_run.borrow().clone(), &ui2);
        });
    }

    // doctor
    {
        let ui2 = ui.clone();
        doctor.connect_clicked(move |_| {
            run_streaming(vec!["doctor".into()], &ui2, "doctor", None);
        });
    }

    // refresh — re-read the cards (which runs probe+roles once), and say so. It used to ALSO stream a
    // separate `probe`, running probe twice for one click.
    {
        let fill_cards = fill_cards.clone();
        let ui2 = ui.clone();
        let refresh_state2 = refresh_state.clone();
        refresh.connect_clicked(move |_| {
            (ui2.set_busy)(true);
            ui2.append("— re-reading this machine's cards —\n");
            let ui3 = ui2.clone();
            let rs = refresh_state2.clone();
            fill_cards(Some(Rc::new(move || {
                (ui3.set_busy)(false);
                rs();
            })));
        });
    }

    // first fill, so the window has content the moment it appears (off the main thread)
    fill_cards(None);

    window.present();
}

/// ★ THE TESTS PIN WHAT WAS ONLY INCIDENTALLY TRUE. Everything here is a pure function or a fake pane, so
/// it runs with no display and touches no hardware. The rule this file learned the hard way: a green suite
/// over a test that cannot fail is theatre — so each of these names the state it fires in, and the suite
/// was mutation-checked (force the trim to zero, seed the card picker, break the gate handover) to prove it
/// can go red.
#[cfg(test)]
mod tests {
    use super::*;

    // ★ THE SHAPE THIS MACHINE ACTUALLY PRODUCES, read live on 2026-10-12 — including the one detail that
    //   matters: `render_node` is a BASENAME ("renderD128"), not "renderD128". A fixture that
    //   disagrees with the machine tests a machine nobody has.
    const PROBE: &str = r#"{"gpus":[
        {"card":"card1","name":"Navi 48 [Radeon RX 9070/9070 XT/9070 GRE]","render_node":"renderD128","can_encode":true,"marginal":null},
        {"card":"card3","name":"Granite Ridge [Radeon 610M]","render_node":"renderD131","can_encode":true,"marginal":null},
        {"card":"card0","name":"Vega 10 [Instinct MI25/MI25x2/V340/V320]","render_node":"renderD129","can_encode":true,"marginal":"x4 link at the host — too narrow to use unasked"},
        {"card":"card2","name":"Vega 10 XTX [Radeon Vega Frontier Edition]","render_node":"renderD130","can_encode":true,"marginal":"x1 link at the host — too narrow to use unasked"}
    ]}"#;
    const ROLES: &str = r#"[{"card":"card1","name":"Navi 48 [Radeon RX 9070/9070 XT/9070 GRE]","roles":["render","display"]}]"#;

    /// A window that is not a window. Records what was said and whether a batch was started — and nothing
    /// else, so a test can assert the two facts the gate depends on without GTK.
    #[derive(Default)]
    struct Fake {
        notes: RefCell<Vec<String>>,
        started_files: Cell<usize>,
        started_cards: Cell<usize>,
    }

    impl Pane for Fake {
        fn note(&self, text: &str) {
            self.notes.borrow_mut().push(text.to_string());
        }
        fn start_batch(&self, cards: Vec<String>, files: Vec<String>) {
            self.started_cards.set(cards.len());
            self.started_files.set(files.len());
        }
    }

    fn recorder() -> (Rc<Cell<bool>>, Rc<dyn Fn(bool)>) {
        let up = Rc::new(Cell::new(false));
        let set: Rc<dyn Fn(bool)> = {
            let up = up.clone();
            Rc::new(move |v: bool| up.set(v))
        };
        (up, set)
    }

    // ── the bound on the output pane ──────────────────────────────────────────────────────────────

    #[test]
    fn nothing_is_trimmed_at_or_under_the_bound() {
        assert_eq!(overflow_to_trim(0), 0);
        assert_eq!(overflow_to_trim(BUF_LIMIT - 1), 0);
        assert_eq!(overflow_to_trim(BUF_LIMIT), 0, "at the bound, still nothing");
    }

    #[test]
    fn over_the_bound_a_quarter_comes_off_the_front() {
        assert_eq!(overflow_to_trim(BUF_LIMIT + 1), (BUF_LIMIT + 1) / 4);
        // and the pane keeps following itself: one trim puts it back under the bound without throwing
        // the recent output away
        let after = BUF_LIMIT + 1 - overflow_to_trim(BUF_LIMIT + 1);
        assert!(after < BUF_LIMIT, "one trim must put the pane back under the bound");
        assert!(after > BUF_LIMIT / 2, "...without discarding the recent output");
    }

    // ── the dead-worker path: an empty result must still release the busy gate ────────────────────
    // ★ THIS IS THE ONE THAT USED TO BE PROSE. An earlier version asserted only "no cards come back",
    //   leaving "and therefore the gate goes down" to be inferred by reading two other functions.

    #[test]
    fn a_dead_worker_leaves_the_gate_down_and_a_launched_batch_keeps_it_up() {
        let files = vec!["/tmp/a.mp4".to_string()];

        // (a) EXACTLY what a dead worker hands the completion future: `unwrap_or_default()` -> ("", "").
        let (cards, _) = pick_cards("", "", None);
        let (up, set) = recorder();
        let pane = Fake::default();
        let gate = Busy::raised_with(set.clone());
        assert!(up.get(), "raising the gate turns it on");
        start_jobs(files.clone(), cards, &pane, Some(gate));
        assert_eq!(pane.started_files.get(), 0, "a dead worker must start no job");
        assert!(
            !up.get(),
            "★ and the gate MUST be down — nothing was launched, so nothing else can lower it"
        );
        assert!(pane.notes.borrow().iter().any(|n| n.contains("No card can take a job here")));

        // (b) A REAL BATCH: it owns the release from the moment it is launched.
        let (cards, _) = pick_cards(PROBE, ROLES, None);
        let (up, set) = recorder();
        let pane = Fake::default();
        let gate = Busy::raised_with(set.clone());
        start_jobs(files, cards, &pane, Some(gate));
        assert_eq!(pane.started_files.get(), 1, "the one file goes out");
        assert_eq!(pane.started_cards.get(), 1, "onto the one usable card");
        assert!(up.get(), "the batch releases the gate when every job finishes, not before");
    }

    #[test]
    fn a_reply_that_is_valid_json_but_says_nothing_is_the_same_case() {
        // A worker that ran but produced no usable answer must land on the SAME branch as a dead one.
        for (p, r) in [("{}", "[]"), ("null", "null"), ("[]", "\"not an object\"")] {
            let (cards, _) = pick_cards(p, r, None);
            let (up, set) = recorder();
            let pane = Fake::default();
            start_jobs(vec!["/tmp/a.mp4".into()], cards, &pane, Some(Busy::raised_with(set)));
            assert_eq!(pane.started_files.get(), 0, "probe {p:?} roles {r:?} must start nothing");
            assert!(!up.get(), "probe {p:?} roles {r:?} must leave the gate down");
        }
    }

    // ── which cards are offered, read from the tool's own answers ─────────────────────────────────

    #[test]
    fn the_card_that_drives_the_monitor_is_never_offered_a_job() {
        // card0 is the display card AND can encode AND is not marginal — the case that would take a job.
        let (cards, _) = pick_cards(PROBE, ROLES, None);
        assert!(!cards.iter().any(|c| c == "renderD128"), "the display card is the game's");
    }

    #[test]
    fn a_card_the_tool_marks_marginal_is_left_out_by_default() {
        let (cards, _) = pick_cards(PROBE, ROLES, None);
        assert!(!cards.iter().any(|c| c == "renderD129"), "the MI25's x4 link is left out by default");
        assert!(!cards.iter().any(|c| c == "renderD130"), "the FE's x1 link is left out by default");
        assert!(cards.iter().any(|c| c == "renderD131"), "...and a clean spare IS offered");
    }

    #[test]
    fn a_card_with_no_encoder_is_not_offered() {
        // every card on THIS machine can encode, so the case brings its own document
        let probe = r#"{"gpus":[
            {"card":"card3","name":"Granite Ridge","render_node":"renderD131","can_encode":true,"marginal":null},
            {"card":"card1","name":"some card","render_node":"renderD128","can_encode":false,"marginal":null}]}"#;
        let (cards, _) = pick_cards(probe, "[]", None);
        assert_eq!(cards, vec!["renderD131".to_string()]);
    }

    #[test]
    fn a_usable_card_with_no_render_node_is_counted_not_dropped() {
        let probe = r#"{"gpus":[{"card":"card2","render_node":null,"can_encode":true,"marginal":null}]}"#;
        let (cards, no_node) = pick_cards(probe, "[]", None);
        assert!(cards.is_empty());
        assert_eq!(no_node, 1, "it must be SAID, not silently dropped");
    }

    #[test]
    fn when_the_tool_did_not_answer_the_card_is_not_offered() {
        // no `can_encode` key at all — the tool did not answer, so this app does not guess
        let probe = r#"{"gpus":[{"card":"card2","render_node":"renderD130"}]}"#;
        let (cards, no_node) = pick_cards(probe, "[]", None);
        assert!(cards.is_empty());
        assert_eq!(no_node, 0);
        assert!(matches!(usability(&serde_json::from_str::<serde_json::Value>(probe).unwrap()["gpus"][0], &[]), Use::Unknown));
    }

    /// ★ THE DIVERGENCE THAT WAS CLOSED. The pane and the batch read the same document; the pane used to
    /// treat `marginal` as a string while the batch required null, so a card marked marginal with any other
    /// JSON type was SHOWN as "available for jobs" and refused when Run was pressed.
    ///
    /// This asserts they AGREE — which, since both now call `usability`, is structural rather than a
    /// coincidence, and the mutation check confirmed it can no longer be broken from one side. The POLICY
    /// itself (what counts as marginal) is pinned by the test below, because a change to the shared
    /// definition moves both sides together and this test would rightly stay green.
    #[test]
    fn what_the_pane_says_and_what_the_batch_does_cannot_disagree() {
        let corpus = [
            (PROBE, ROLES),
            (r#"{"gpus":[{"card":"c","can_encode":true,"marginal":true,"render_node":"renderD200"}]}"#, "[]"),
            (r#"{"gpus":[{"card":"c","can_encode":true,"marginal":{"reason":"x4"},"render_node":"renderD201"}]}"#, "[]"),
            (r#"{"gpus":[{"card":"c","can_encode":true,"marginal":0,"render_node":"renderD202"}]}"#, "[]"),
            (r#"{"gpus":[{"card":"c","can_encode":true,"marginal":"x1","render_node":"renderD203"}]}"#, "[]"),
            (r#"{"gpus":[{"card":"c","can_encode":true,"marginal":null,"render_node":"renderD204"}]}"#, "[]"),
            (r#"{"gpus":[{"card":"c","can_encode":false,"marginal":null,"render_node":"renderD205"}]}"#, "[]"),
            (r#"{"gpus":[{"card":"c","render_node":"renderD206"}]}"#, "[]"),
            (r#"{"gpus":[{"card":"c","can_encode":true,"marginal":null,"render_node":"renderD207"}]}"#,
             r#"[{"card":"c","roles":["display"]}]"#),
        ];
        for (probe, roles) in corpus {
            let doc: serde_json::Value = serde_json::from_str(probe).unwrap();
            let rdoc: serde_json::Value = serde_json::from_str(roles).unwrap();
            let held = held_back(&rdoc, None);
            let (cards, _) = pick_cards(probe, roles, None);
            for g in doc["gpus"].as_array().unwrap() {
                let node = g.get("render_node").and_then(|v| v.as_str()).unwrap_or("?");
                let offered = cards.iter().any(|c| c == node);
                let (subtitle, _, _) = describe(usability(g, &held));
                assert_eq!(
                    offered,
                    subtitle == "available for jobs",
                    "{probe} — the pane says {subtitle:?} but the batch {} it",
                    if offered { "offered" } else { "refused" }
                );
            }
        }
    }

    /// ★ THE POLICY, PINNED. "Present and not null" is the test for marginal — not "is a string". A reason
    /// the tool phrased as anything else still means the card is left out, AND is still shown as left out.
    #[test]
    fn a_marginal_reason_is_a_marginal_card_whatever_its_json_type() {
        for m in ["\"x4 through a bridge\"", "0", "true", "{\"reason\":\"x1\"}", "\"narrow\""] {
            let probe = format!(
                r#"{{"gpus":[{{"card":"c","can_encode":true,"marginal":{m},"render_node":"renderD300"}}]}}"#
            );
            let g: serde_json::Value =
                serde_json::from_str::<serde_json::Value>(&probe).unwrap()["gpus"][0].clone();
            assert!(
                matches!(usability(&g, &[]), Use::Marginal(_)),
                "marginal={m} must be a marginal card, not a free one"
            );
            let (cards, _) = pick_cards(&probe, "[]", None);
            assert!(cards.is_empty(), "marginal={m} must not be offered a job");
            let (subtitle, _, _) = describe(usability(&g, &[]));
            assert!(subtitle.starts_with("left out by default — "), "and the pane must say so: {subtitle:?}");
        }
        // ...and null is NOT marginal. This is the negation, so the test above cannot pass vacuously.
        let probe = r#"{"gpus":[{"card":"c","can_encode":true,"marginal":null,"render_node":"renderD301"}]}"#;
        let g: serde_json::Value =
            serde_json::from_str::<serde_json::Value>(probe).unwrap()["gpus"][0].clone();
        assert!(matches!(usability(&g, &[]), Use::Free));
        assert_eq!(pick_cards(probe, "[]", None).0, vec!["renderD301".to_string()]);
    }

    // ── the batch itself ──────────────────────────────────────────────────────────────────────────

    #[test]
    fn the_job_the_app_starts_is_the_tool_argv() {
        assert_eq!(
            job_argv("/tmp/one.mp4", "renderD131"),
            vec!["play", "--work", "/tmp/one.mp4", "--spare", "renderD131", "--", "true"]
                .iter().map(|s| s.to_string()).collect::<Vec<_>>()
        );
    }

    #[test]
    fn more_files_than_cards_means_a_queue_and_the_line_says_so() {
        assert_eq!(batch_header(3, 3), "— 3 job(s), one per card, all at once —\n");
        assert_eq!(batch_header(1, 3), "— 1 job(s), one per card, all at once —\n");
        let h = batch_header(10, 3);
        assert!(h.contains("10 job(s) across 3 card(s)"), "{h:?}");
        assert!(h.contains("wait their turn"), "the leftovers must be queued, not dropped: {h:?}");
    }

    /// ★ A BATCH THAT ENDS WITH FILES STILL IN THE QUEUE MUST SAY SO. A card that cannot start a job at
    /// all stops, so with one card the rest of the queue is never run — and the queue was introduced to
    /// REPLACE the old "N were NOT started" line, not to lose it. `undelivered_note` is the line that says
    /// it, once, at the end of the batch.
    ///
    /// Zero must stay SILENT: a line printing "0 files" on every good batch is noise, and noise is how a
    /// real warning gets ignored. Both directions are asserted, so neither half can quietly rot.
    #[test]
    fn a_batch_that_leaves_files_in_the_queue_says_how_many() {
        assert_eq!(undelivered_note(0), "", "a clean batch says nothing at all");
        assert!(undelivered_note(1).contains('1'), "{:?}", undelivered_note(1));
        let three = undelivered_note(3);
        assert!(three.contains('3'), "{three:?}");
        assert!(three.contains("never started"), "say what happened, not just a number: {three:?}");
    }

    /// ★ THE COUNT MUST BE HONEST ABOUT THE BATCH, NOT ONLY ABOUT THE QUEUE — the case the pure
    /// `undelivered_note` test above cannot see, because it never drives a drain. Several cards can stop at
    /// once (the tool cannot be spawned at all); each has already TAKEN a file. If those files are not put
    /// back, the end-of-batch number is short by exactly the number of cards that broke, and the files they
    /// took are reported nowhere.
    #[test]
    fn a_card_that_stops_gives_back_the_file_it_took_but_never_ran() {
        let files: Vec<String> = (0..10).map(|i| format!("/tmp/f{i}.mp4")).collect();
        let queue = Arc::new(Mutex::new(VecDeque::from(files.clone())));
        // three cards each take one, then discover they cannot start a job at all and stop
        let taken: Vec<String> = (0..3).filter_map(|_| take_next(&queue)).collect();
        assert_eq!(taken.len(), 3);
        assert_eq!(queue.lock().unwrap().len(), 7, "seven still queued");
        assert_eq!(
            undelivered_note(7),
            undelivered_note(queue.lock().unwrap().len()),
            "and on its own the queue would undercount"
        );
        for f in taken {
            put_back(&queue, f);
        }
        assert_eq!(queue.lock().unwrap().len(), 10, "every taken file is back in the queue");
        assert!(
            undelivered_note(queue.lock().unwrap().len()).contains("10"),
            "the batch must report TEN never started, not seven"
        );
    }

    /// ★ THE QUEUE IS THE NEW MECHANISM, so it is tested on its own — the same `take_next` the workers
    /// call, under real threads. Every chosen file must go to exactly ONE card: none dropped, none run
    /// twice. That is what replaces the old "N were NOT started" line.
    #[test]
    fn the_queue_hands_every_file_to_exactly_one_card() {
        let files: Vec<String> = (0..50).map(|i| format!("/tmp/f{i}.mp4")).collect();
        let queue = Arc::new(Mutex::new(VecDeque::from(files.clone())));
        let taken: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let mut handles = Vec::new();
        // ★ BOUNDED ON PURPOSE. This loop guards `take_next`, so it has to survive the case where
        // `take_next` is BROKEN — and a broken `take_next` that never removes anything turns an
        // unbounded `while let` into an infinite loop that pushes into `taken` forever. Measured
        // 2026-10-12: exactly that, three threads, 27 GB of RAM, the kernel killed the process and
        // systemd-oomd killed the terminal it was running in. A guard that detects a fault by
        // exhausting the machine is not a guard. Bounded, the same fault fails as an assertion.
        let cap = files.len() * 4;
        for _card in 0..3 {
            let queue = queue.clone();
            let taken = taken.clone();
            handles.push(std::thread::spawn(move || {
                let mut left = cap;
                while left > 0 {
                    match take_next(&queue) {
                        Some(f) => taken.lock().unwrap().push(f),
                        None => break,
                    }
                    left -= 1;
                }
            }));
        }
        for h in handles {
            h.join().unwrap();
        }
        let mut got = taken.lock().unwrap().clone();
        got.sort();
        let mut want = files.clone();
        want.sort();
        assert_eq!(got, want, "every file exactly once — none lost, none run twice");
    }

    #[test]
    fn empty_cards_plan_no_batch_at_all() {
        let files = vec!["/tmp/a.mp4".to_string()];
        assert!(matches!(start_plan(&files, &[]), Start::NoCards));
        assert!(matches!(start_plan(&files, &["renderD131".to_string()]), Start::Batch(_)));
    }

    #[test]
    fn files_but_no_cards_still_launches_nothing_and_still_releases_the_gate() {
        // The case where `cards.is_empty()` and `jobs.is_empty()` could be confused: files chosen, no cards.
        let (up, set) = recorder();
        let pane = Fake::default();
        let files = vec!["/tmp/a.mp4".to_string(), "/tmp/b.mp4".to_string()];
        start_jobs(files, vec![], &pane, Some(Busy::raised_with(set)));
        assert_eq!(pane.started_files.get(), 0, "no cards means no jobs");
        assert!(!up.get(), "and the gate must be down");
    }

    // ── a name the user typed is a name the list shows ─────────────────────────────────────────────

    /// From a real run: the card's own name was typed — `MI25` — exactly what the card list printed — and the tool answered
    /// *"ignoring unknown render node(s): MI25 ... REFUSED"*. The field wanted a render node and nothing
    /// said so. These are the four spellings a person could reasonably use for one card.
    #[test]
    fn a_card_can_be_named_the_way_the_list_prints_it() {
        let probe: serde_json::Value = serde_json::from_str(PROBE).unwrap();
        let roles: serde_json::Value = serde_json::from_str(ROLES).unwrap();
        for spelling in ["renderD129", "card0", "MI25", "mi25", "Vega 10 [Instinct"] {
            let (nodes, unmatched) =
                resolve_named(&[spelling.to_string()], &probe, &roles);
            assert_eq!(nodes, vec!["renderD129".to_string()], "{spelling:?} must resolve to the MI25's node");
            assert!(unmatched.is_empty(), "{spelling:?} must not be reported as unmatched");
        }
        // ...and a name that matches nothing comes BACK, so the pane can say so instead of dropping it.
        let (nodes, unmatched) =
            resolve_named(&["nonesuch".to_string(), "card2".to_string()], &probe, &roles);
        assert_eq!(nodes, vec!["renderD130".to_string()]);
        assert_eq!(unmatched, vec!["nonesuch".to_string()]);
    }

    /// The MI25 and the Vega FE are named the same way in different spellings; a substring must not steal
    /// the wrong card. Both names contain "Vega 10"? No — one is "MI25" and the other "Vega FE", and the
    /// exact node and exact card id always win over a substring.
    #[test]
    fn an_exact_node_or_card_id_beats_a_loose_name_match() {
        let probe: serde_json::Value = serde_json::from_str(PROBE).unwrap();
        let roles: serde_json::Value = serde_json::from_str(ROLES).unwrap();
        let (nodes, _) = resolve_named(&["card2".to_string()], &probe, &roles);
        assert_eq!(nodes, vec!["renderD130".to_string()], "the id wins");
        let (nodes, _) = resolve_named(&["renderD131".to_string()], &probe, &roles);
        assert_eq!(nodes, vec!["renderD131".to_string()]);
    }

    // ── the main card: the tool's answer, and the user's override ──────────────────────────────────

    #[test]
    fn one_display_card_is_taken_and_several_are_all_held_back() {
        let roles: serde_json::Value = serde_json::from_str(ROLES).unwrap();
        assert_eq!(display_cards(&roles).len(), 1);
        assert_eq!(held_back(&roles, None), vec!["card1".to_string()]);

        // ★ THE BLOCKER THE OUTSIDE READ FOUND. Two cards with a monitor each: the old rule held back only
        //   the FIRST and offered the second a job — handing work to a card that may be running the game.
        //   Both are held back now, and the picker is how the user says which one is theirs.
        let two = r#"[{"card":"card1","roles":["display"]},{"card":"card2","roles":["display"]}]"#;
        let rdoc: serde_json::Value = serde_json::from_str(two).unwrap();
        assert_eq!(display_cards(&rdoc).len(), 2);
        assert_eq!(held_back(&rdoc, None), vec!["card1".to_string(), "card2".to_string()]);
        let (cards, _) = pick_cards(PROBE, two, None);
        assert!(!cards.iter().any(|c| c == "renderD128"), "the first display card is held back");
        assert!(!cards.iter().any(|c| c == "renderD130"), "...and so is the second");

        // and the user's choice narrows it to exactly the one they name
        assert_eq!(held_back(&rdoc, Some("card2")), vec!["card2".to_string()]);

        // no display card at all is a third case, and it holds nothing back
        let none = r#"[{"card":"card1","roles":["encode"]}]"#;
        let rdoc: serde_json::Value = serde_json::from_str(none).unwrap();
        assert_eq!(display_cards(&rdoc).len(), 0);
        assert!(held_back(&rdoc, None).is_empty());

        // ★ THE ONE BOUNDARY WHERE "EVERY DISPLAY CARD" COULD PART COMPANY WITH WHAT IS HELD BACK. A
        //   display entry with no `card` key used to be dropped by `filter_map`, which silently SHRANK the
        //   held-back set — and a shrunk held-back set offers a monitor card a job. It is carried now, and
        //   the second assertion is the one that matters: a probe entry with no card id matches the
        //   carried empty id and is held back. Safe by construction, not by what the documents happen to
        //   contain. (Outside read, 2026-10-12.)
        let no_card = r#"[{"card":"card1","roles":["display"]},{"roles":["display"]}]"#;
        let rdoc: serde_json::Value = serde_json::from_str(no_card).unwrap();
        let held = held_back(&rdoc, None);
        assert_eq!(display_cards(&rdoc).len(), 2, "nothing is dropped: {held:?}");
        assert_eq!(held, vec!["card1".to_string(), String::new()]);
        let nameless = r#"{"gpus":[{"can_encode":true,"marginal":null,"render_node":"renderD900"}]}"#;
        let g = &serde_json::from_str::<serde_json::Value>(nameless).unwrap()["gpus"][0];
        assert!(
            matches!(usability(g, &held), Use::Main),
            "a card the tool called a display is never offered a job, even when it arrives without an id"
        );
    }

    /// The old batch rule required `marginal` to be PRESENT and null; a card with `can_encode` and no
    /// `marginal` key at all was refused. Treating that document as "free" would silently relax a safety
    /// rule, so it is "unknown" — and the pane must say unknown rather than offer the card.
    #[test]
    fn a_card_that_answers_can_encode_but_not_marginal_is_not_assumed_free() {
        let probe = r#"{"gpus":[{"card":"c","render_node":"renderD400","can_encode":true}]}"#;
        let g: serde_json::Value =
            serde_json::from_str::<serde_json::Value>(probe).unwrap()["gpus"][0].clone();
        assert!(matches!(usability(&g, &[]), Use::Unknown));
        assert!(pick_cards(probe, "[]", None).0.is_empty());
        // ...and WITH `marginal: null` present it is free — the negation, so the above is not vacuous
        let probe = r#"{"gpus":[{"card":"c","render_node":"renderD401","can_encode":true,"marginal":null}]}"#;
        assert_eq!(pick_cards(probe, "[]", None).0, vec!["renderD401".to_string()]);
    }

    /// ★ EVERY WORD THE PANE CAN SAY, PINNED. The agreement test next door cannot fail on a wording change
    /// — both sides move together — so the wording itself is pinned here, one case per verdict.
    #[test]
    fn every_verdict_the_pane_can_print_is_pinned() {
        let f = |u: Use| describe(u).0;
        assert_eq!(f(Use::Main), "drives your monitor — nothing is ever started on this card");
        assert_eq!(f(Use::Unknown), "unknown — the tool did not report this card");
        assert_eq!(
            f(Use::CannotEncode("unknown".into())),
            "cannot take a job: no VAAPI encoder for “unknown”"
        );
        assert_eq!(
            f(Use::CannotEncode("nvidia".into())),
            "cannot take a job: no VAAPI encoder for “nvidia”"
        );
        assert_eq!(f(Use::Marginal("x4".into())), "left out by default — x4 (name it with --spare)");
        assert_eq!(f(Use::Free), "available for jobs");
    }

    /// ★ THE OVERRIDE IS THE WHOLE POINT OF THE PICKER: with two cards claiming a display, the user's
    /// choice decides which one is held back — and it decides it for the batch, not just for the list.
    #[test]
    fn the_users_choice_of_main_card_decides_which_card_is_held_back() {
        // The tool says card1 (the XT) drives the display. Its own answer holds the XT back, leaving the
        // iGPU as the only spare this machine offers by default.
        assert_eq!(pick_cards(PROBE, ROLES, None).0, vec!["renderD131".to_string()]);

        // The user says the game is actually on the iGPU (card3). The XT becomes a spare, and the card the
        // user named as theirs is held back — which is the whole point of naming it.
        let (cards, _) = pick_cards(PROBE, ROLES, Some("card3"));
        assert!(cards.iter().any(|c| c == "renderD128"), "the user's choice frees the XT");
        assert!(!cards.iter().any(|c| c == "renderD131"), "and the iGPU is held back as theirs");

        // ...and naming a card as theirs really does hold it back.
        let (cards, _) = pick_cards(PROBE, ROLES, Some("card1"));
        assert!(!cards.iter().any(|c| c == "renderD128"), "their own card is never offered");
        assert_eq!(cards, vec!["renderD131".to_string()]);
    }

    // ── the app and the command must be the same version ───────────────────────────────────────────

    /// ★ THE ONE TEST THAT LEAVES THE BUILD. It runs the `spirit` this app would actually run — beside the
    /// test binary first, then on PATH — and puts its REAL output through the REAL decision code. The
    /// invariant is machine-independent: a card the tool calls free must be offered a job, and a card the
    /// tool holds back must not be. It is skipped, loudly, only where there is no `spirit` to ask.
    #[test]
    fn the_documents_this_machine_actually_produces_go_through_the_real_decision_code() {
        let bin = spirit_bin();
        let run = |args: &[&str]| -> Option<String> {
            let o = Command::new(&bin).args(args).output().ok()?;
            if !o.status.success() {
                return None;
            }
            Some(String::from_utf8_lossy(&o.stdout).to_string())
        };
        let Some(probe) = run(&["probe", "--json"]) else {
            eprintln!("SKIPPED: no `spirit` at {} — nothing to ask", bin.display());
            return;
        };
        let Some(roles) = run(&["roles", "--json"]) else {
            eprintln!("SKIPPED: `spirit roles --json` did not answer");
            return;
        };
        println!("asked {} — {} bytes of probe, {} bytes of roles",
                 bin.display(), probe.len(), roles.len());

        // ★ THE PAIRING FIRST, because that is what actually broke here: an app newer than its command
        //   gets documents with no capability in them, and then blames the machine.
        assert!(
            probe_reports_capability(&probe),
            "the `spirit` at {} answered `probe --json` WITHOUT any `can_encode` — it is older than this \
             app. Nothing can be started until the pair is the same version.",
            bin.display()
        );

        let doc: serde_json::Value = serde_json::from_str(&probe).unwrap();
        let rdoc: serde_json::Value = serde_json::from_str(&roles).unwrap();
        let held = held_back(&rdoc, None);
        println!("the tool says {} card(s) drive a display: {}", held.len(), held.join(", "));
        let (cards, _) = pick_cards(&probe, &roles, None);
        println!("this run would start {} job(s): {}", cards.len(), cards.join(", "));

        for g in doc["gpus"].as_array().unwrap() {
            let node = g.get("render_node").and_then(|v| v.as_str()).unwrap_or("?");
            let (subtitle, _, _) = describe(usability(g, &held));
            assert_eq!(
                cards.iter().any(|c| c == node),
                subtitle == "available for jobs",
                "{}: the pane would say {subtitle:?} and the batch would do the opposite",
                g.get("name").and_then(|v| v.as_str()).unwrap_or(node)
            );
        }

        // and a name taken straight off the machine's own list must resolve to a node
        for (card, name, node) in card_names(&doc, &rdoc) {
            if node.is_empty() {
                continue;
            }
            let first_word = name.split_whitespace().next().unwrap_or("").to_string();
            if first_word.is_empty() {
                continue;
            }
            let (nodes, _) = resolve_named(&[first_word.clone()], &doc, &rdoc);
            assert!(
                !nodes.is_empty(),
                "typing {first_word:?} (the first word of {card}'s own name) resolved to nothing"
            );
        }
    }

    /// ★ THIS IS THE FAULT THAT COST A SESSION. The app was 1.1.0 and the installed command was 1.0.0, so
    /// `probe --json` carried no `can_encode` at all; every card read "unknown" and Run answered "No card
    /// can take a job here" on a machine with two good spare cards. The pair is what has to be checked.
    #[test]
    fn a_document_with_cards_but_no_capability_key_is_an_old_command_not_a_useless_machine() {
        assert!(!probe_reports_capability(""), "nothing at all");
        assert!(!probe_reports_capability("{}"), "a document with no cards");
        assert!(!probe_reports_capability(r#"{"gpus":[]}"#), "a machine that reported no cards");
        // EXACTLY the shape the old command produced: cards, render nodes, and no capability at all
        assert!(!probe_reports_capability(
            r#"{"gpus":[{"card":"card1","render_node":"renderD128","name":"9070 XT"}]}"#
        ));
        // ...and the shape the matching command produces
        assert!(probe_reports_capability(PROBE));
    }
}

fn main() -> glib::ExitCode {
    let app = adw::Application::builder().application_id(APP_ID).build();
    app.connect_activate(build_ui);
    app.run()
}
