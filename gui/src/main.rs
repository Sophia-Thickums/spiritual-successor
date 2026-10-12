//! spirit-gui — the native desktop app for Spiritual Successor.
//!
//! GTK4 + libadwaita, in Rust, the same shape as the GPU tool the owner already runs (LACT). No browser,
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
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::rc::Rc;

const APP_ID: &str = "com.sophia.SpiritualSuccessor";

/// Everything an asynchronous run needs to show its work, so the run helper takes one argument instead
/// of four that must be remembered in the right order.
#[derive(Clone)]
struct Ui {
    buf: gtk::TextBuffer,
    view: gtk::TextView,
    mark: gtk::TextMark,
    set_busy: Rc<dyn Fn(bool)>,
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
        let _ = tx0.send_blocking(format!("$ spirit {}\n", a));
        let bin = spirit_bin();
        match Command::new(&bin).args(&args).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn() {
            Ok(mut child) => {
                let mut handles = vec![];
                if let Some(out) = child.stdout.take() {
                    let t = tx.clone();
                    handles.push(std::thread::spawn(move || {
                        for line in BufReader::new(out).lines().map_while(Result::ok) {
                            let _ = t.send_blocking(line);
                        }
                    }));
                }
                if let Some(err) = child.stderr.take() {
                    let t = tx.clone();
                    handles.push(std::thread::spawn(move || {
                        for line in BufReader::new(err).lines().map_while(Result::ok) {
                            let _ = t.send_blocking(line);
                        }
                    }));
                }
                let code = child.wait().ok().and_then(|s| s.code());
                for h in handles {
                    let _ = h.join();
                }
                let _ = tx.send_blocking(match code {
                    Some(0) => format!("— done ({}) —\n", h),
                    Some(c) => format!("— exited {} ({}) —\n", c, h),
                    None => format!("— killed ({}) —\n", h),
                });
            }
            Err(e) => {
                let _ = tx.send_blocking(format!("could not run {}: {}", bin.display(), e));
            }
        }
        tx.close();
    });

    let done_ui = ui.clone();
    glib::spawn_future_local(async move {
        while let Ok(line) = rx.recv().await {
            let mut end = done_ui.buf.end_iter();
            done_ui.buf.insert(&mut end, &(line + "\n"));
            // ★ BOUNDED OUTPUT, AND IT FOLLOWS ITSELF. The buffer used to grow for the life of the
            // window and never scroll, so new output appeared off-screen and read as "nothing happened".
            if done_ui.buf.char_count() > 400_000 {
                let mut start = done_ui.buf.start_iter();
                let mut cut = done_ui.buf.start_iter();
                cut.forward_chars(done_ui.buf.char_count() / 4);
                done_ui.buf.delete(&mut start, &mut cut);
            }
            // ★ SCROLL BY MARK, NOT BY `upper()` — `upper` has not been recomputed for the text just
            // inserted, so poking it lands one insert behind and a finished job's last line can sit
            // below the fold.
            done_ui.buf.move_mark(&done_ui.mark, &done_ui.buf.end_iter());
            done_ui.view.scroll_mark_onscreen(&done_ui.mark);
        }
        (done_ui.set_busy)(false);
        // ★ THE REAL COMPLETION POINT. The first version spawned a SECOND future that polled a shared
        // `busy` flag every 200 ms — a poll dressed as a callback, and two control paths that can desync.
        if let Some(f) = on_done {
            f();
        }
    });
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
    let file_row = adw::ActionRow::builder().title("Video file").subtitle("none chosen").build();
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
                let driver = g.get("driver").and_then(|v| v.as_str()).unwrap_or("");
                let is_main = role
                    .and_then(|r| r.get("roles"))
                    .and_then(|v| v.as_array())
                    .map(|a| a.iter().any(|x| x.as_str() == Some("display")))
                    .unwrap_or(false);

                // ★ THE TOOL'S OWN ANSWER, not a driver-name guess and not a hardcoded threshold.
                let can_encode = g.get("can_encode").and_then(|v| v.as_bool()).unwrap_or(false);
                let marginal = g.get("marginal").and_then(|v| v.as_str()).map(|s| s.to_string());
                let known = g.get("can_encode").is_some();
                let (doing, tone) = if is_main {
                    ("your game — this card drives your monitor".to_string(), "dim-label")
                } else if !known {
                    // the tool did not answer: say so, rather than offering the card
                    ("unknown — the tool did not report this card".to_string(), "warning")
                } else if !can_encode {
                    (format!(
                        "cannot take a job: no VAAPI encoder for “{}”",
                        if driver.is_empty() { "unknown" } else { driver }
                    ), "error")
                } else if let Some(m) = marginal {
                    (format!("left out by default — {} (name it with --spare)", m), "warning")
                } else {
                    ("available for jobs".to_string(), "success")
                };

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
        let chosen: Rc<RefCell<Option<String>>> = Rc::new(RefCell::new(None));
        let chosen2 = chosen.clone();
        let window2 = window.clone();
        let has_file2 = has_file.clone();
        let set_busy2 = set_busy.clone();
        let busy2 = busy.clone();
        choose.connect_clicked(move |_| {
            let dlg = gtk::FileDialog::builder().title("Choose a video file").build();
            let file_row = file_row.clone();
            let chosen3 = chosen2.clone();
            let has_file3 = has_file2.clone();
            let set_busy3 = set_busy2.clone();
            let busy3 = busy2.clone();
            dlg.open(Some(&window2), gio::Cancellable::NONE, move |res| {
                if let Ok(f) = res {
                    if let Some(p) = f.path() {
                        // ★ MUST BE A FILE. A dialog can return a directory; the old code enabled the
                        // button on anything and handed a directory to the job.
                        if !p.is_file() {
                            file_row.set_subtitle("that is not a file — choose a video");
                            *chosen3.borrow_mut() = None;
                            has_file3.set(false);
                        } else {
                            let ps = p.display().to_string();
                            file_row.set_subtitle(&glib::markup_escape_text(&ps));
                            *chosen3.borrow_mut() = Some(ps);
                            has_file3.set(true);
                        }
                        // ★ RE-APPLY THE ONE GATE, never force a control on. Calling set_busy(false) here
                        // re-enabled Run, Doctor and the switches MID-JOB (outside read, 2026-10-12).
                        (set_busy3)(busy3.get());
                    }
                }
            });
        });
        let ui2 = ui.clone();
        let spare_entry = spare_entry.clone();
        run.connect_clicked(move |_| {
            if let Some(p) = chosen.borrow().clone() {
                let mut argv = vec![
                    "play".to_string(),
                    "--work".to_string(),
                    p,
                    "--split".to_string(),
                ];
                // ★ ONE NAME PER PIECE, TRIMMED, EMPTIES DROPPED. The tool itself splits on commas and
                //   spaces and trims each name, so "renderD131, renderD130" already worked — but a
                //   trailing or doubled comma ("renderD130,", "a,,b") would hand it an empty token, and
                //   a field that accepts a card name the machine does not have is the one place in this
                //   app where a user can express something untrue. TODO: replace this field with a
                //   checkbox per card built from the same rows that already say "left out by default —
                //   … (name it with --spare)", which would make an unmatchable name unrepresentable.
                let named: String = spare_entry
                    .text()
                    .split(',')
                    .map(|s| s.trim())
                    .filter(|s| !s.is_empty())
                    .collect::<Vec<_>>()
                    .join(",");
                if !named.is_empty() {
                    argv.push("--spare".to_string());
                    argv.push(named);
                }
                argv.push("--".to_string());
                argv.push("true".to_string());
                run_streaming(argv, &ui2, "job on the other cards", None);
            }
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
            let mut end = ui2.buf.end_iter();
            ui2.buf.insert(&mut end, "— re-reading this machine's cards —\n");
            ui2.buf.move_mark(&ui2.mark, &ui2.buf.end_iter());
            ui2.view.scroll_mark_onscreen(&ui2.mark);
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

fn main() -> glib::ExitCode {
    let app = adw::Application::builder().application_id(APP_ID).build();
    app.connect_activate(build_ui);
    app.run()
}
