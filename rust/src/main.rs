// spirit — the Spiritual Successor: put EVERY GPU in a machine to work.
//
// Faithful Rust port of the stdlib-Python original (spirit.py). No crates: std only,
// matching the original's "no dependencies" promise (it shells out to lspci/vulkaninfo/
// unshare/ffmpeg only when present, exactly as the Python does).
//
// Copyright 2026 Sophia Marie DeClue. MIT.

use std::collections::BTreeMap;
use std::env;
use std::ffi::OsString;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

// ── constants ─────────────────────────────────────────────────────────────────────────
const SPEED_GBPS: [(f64, f64); 5] = [
    (2.5, 0.25),
    (5.0, 0.50),
    (8.0, 1.00),
    (16.0, 1.97),
    (32.0, 4.00),
];
const EFF: f64 = 0.85; // measured efficiency of the theoretical ceiling on this class of box
const PLAN_HEADROOM: f64 = 0.60; // use at most 60% of a link

// boost: the two ways to more frames — UPSCALING via gamescope, FRAME GEN via lsfg-vk.
const LSFG_LAYER: &str = "/usr/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json";

/// Python `os.path.expanduser("~/.config/lsfg-vk/conf.toml")`.
fn lsfg_conf() -> String {
    let home = env::var("HOME").unwrap_or_default();
    format!("{}/.config/lsfg-vk/conf.toml", home)
}

const LSFG_APPID: &str = "993090";

fn steam_manifest() -> String {
    let home = env::var("HOME").unwrap_or_default();
    format!(
        "{}/.local/share/Steam/steamapps/appmanifest_{}.acf",
        home, LSFG_APPID
    )
}

fn lossless_dir() -> String {
    let home = env::var("HOME").unwrap_or_default();
    format!("{}/.local/share/Steam/steamapps/common/Lossless Scaling", home)
}

// ★ THE WITNESS — the only instrument that can see an INSERTED frame. Our own Vulkan layer,
// installed system-wide, INERT unless SPIRIT_PRESENT_COUNT names a file.
const COUNTER_LAYER: &str = "/usr/share/vulkan/implicit_layer.d/VkLayer_SPIRIT_present_counter.json";
const COUNTER_LIB: &str = "/usr/local/lib/libspirit_present_counter.so";

const DRM: &str = "/sys/class/drm";
const PCI: &str = "/sys/bus/pci/devices";

fn gen_name(speed: f64) -> Option<&'static str> {
    match speed {
        s if (s - 2.5).abs() < f64::EPSILON => Some("Gen1"),
        s if (s - 5.0).abs() < f64::EPSILON => Some("Gen2"),
        s if (s - 8.0).abs() < f64::EPSILON => Some("Gen3"),
        s if (s - 16.0).abs() < f64::EPSILON => Some("Gen4"),
        s if (s - 32.0).abs() < f64::EPSILON => Some("Gen5"),
        _ => None,
    }
}

fn speed_gbps(speed: f64) -> Option<f64> {
    SPEED_GBPS
        .iter()
        .find(|(k, _)| (k - speed).abs() < f64::EPSILON)
        .map(|(_, v)| *v)
}

// ── tiny sysfs readers (mirror _read / _int / _speed) ──────────────────────────────────
fn read_trim(p: &str) -> Option<String> {
    fs::read_to_string(p).ok().map(|s| s.trim().to_string())
}

fn read_int(p: &str) -> Option<i64> {
    read_trim(p).and_then(|v| v.parse::<i64>().ok())
}

fn read_speed(p: &str) -> Option<f64> {
    let v = read_trim(p)?;
    if v.is_empty() {
        return None;
    }
    v.split_whitespace().next().and_then(|t| t.parse::<f64>().ok())
}

// ── python-compatible numeric formatting ──────────────────────────────────────────────
/// Python `round(float)` -> int, round-half-to-even.
fn py_round_i(x: f64) -> i64 {
    let f = x.floor();
    let diff = x - f;
    let r = if diff > 0.5 {
        f + 1.0
    } else if diff < 0.5 {
        f
    } else if (f as i64) % 2 == 0 {
        f
    } else {
        f + 1.0
    };
    r as i64
}

/// Python `round(x, 2)`.
fn round2(x: f64) -> f64 {
    py_round_i(x * 100.0) as f64 / 100.0
}

/// Python `str(float)` (repr) — shortest round-trip, always with a '.' or exponent.
fn py_str_float(f: f64) -> String {
    if f.is_nan() {
        return "nan".to_string();
    }
    if f.is_infinite() {
        return if f > 0.0 { "inf".into() } else { "-inf".into() };
    }
    let s = format!("{}", f);
    if s.contains('.') || s.contains('e') || s.contains('E') {
        s
    } else {
        format!("{}.0", s)
    }
}

/// Helper: repr of an Option<f64> for `%s` (None prints as "None").
fn opt_float_str(v: Option<f64>) -> String {
    match v {
        Some(f) => py_str_float(f),
        None => "None".to_string(),
    }
}

/// Python `repr(str)` — for the `%r` uses in the lsfg profile messages.
fn py_repr_str(s: &str) -> String {
    let has_single = s.contains('\'');
    let has_double = s.contains('"');
    let quote = if has_single && !has_double { '"' } else { '\'' };
    let mut out = String::new();
    out.push(quote);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == quote => {
                out.push('\\');
                out.push(c);
            }
            c => out.push(c),
        }
    }
    out.push(quote);
    out
}

/// Python `float(str)` for the forms the CLI accepts (whitespace, signs, inf/nan, underscores).
fn py_float(s: &str) -> Option<f64> {
    let t = s.trim();
    if t.is_empty() {
        return None;
    }
    let low = t.to_ascii_lowercase();
    let (sign, body) = if let Some(r) = low.strip_prefix('+') {
        (1.0, r.to_string())
    } else if let Some(r) = low.strip_prefix('-') {
        (-1.0, r.to_string())
    } else {
        (1.0, low.clone())
    };
    let v = match body.as_str() {
        "inf" | "infinity" => f64::INFINITY,
        "nan" => f64::NAN,
        _ => {
            let cleaned: String = body.chars().filter(|c| *c != '_').collect();
            match cleaned.parse::<f64>() {
                Ok(x) => x,
                Err(_) => return None,
            }
        }
    };
    Some(sign * v)
}

/// Python `int(str)` (base 10) for `--framegen`.
fn py_int(s: &str) -> Option<i64> {
    let t = s.trim();
    if t.is_empty() {
        return None;
    }
    t.replace('_', "").parse::<i64>().ok()
}

/// `%-Ns` left-justified to a minimum width (never truncates).
fn ljust(s: &str, w: usize) -> String {
    let n = s.chars().count();
    if n >= w {
        s.to_string()
    } else {
        let mut out = s.to_string();
        out.push_str(&" ".repeat(w - n));
        out
    }
}

/// Python `s[:n]` (code-point slice).
fn head(s: &str, n: usize) -> String {
    s.chars().take(n).collect()
}

// ── which(1) ───────────────────────────────────────────────────────────────────────────
fn which(name: &str) -> Option<PathBuf> {
    let path = env::var_os("PATH")?;
    for dir in env::split_paths(&path) {
        let cand = dir.join(name);
        if let Ok(md) = fs::metadata(&cand) {
            if md.is_file() {
                #[cfg(unix)]
                {
                    use std::os::unix::fs::PermissionsExt;
                    if md.permissions().mode() & 0o111 != 0 {
                        return Some(cand);
                    }
                }
                #[cfg(not(unix))]
                {
                    return Some(cand);
                }
            }
        }
    }
    None
}

// ── the true host link: minimum negotiated width/speed along the sysfs chain ──────────
fn chain_for(bdf: &str) -> Vec<String> {
    let real = fs::canonicalize(format!("{}/{}", PCI, bdf))
        .ok()
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_else(|| bdf.to_string());
    let segs: Vec<String> = real
        .split('/')
        .filter(|s| s.starts_with("0000:"))
        .map(|s| s.to_string())
        .collect();
    if segs.is_empty() {
        vec![bdf.to_string()]
    } else {
        segs
    }
}

struct BindingLink {
    binding_element: Option<String>,
    min_current_w: Option<i64>,
    min_current_s: Option<f64>,
    min_max_w: Option<i64>,
    min_max_s: Option<f64>,
}

/// (binding_element, min_current_w, min_current_s, min_max_w, min_max_s).
/// Currents and maxima are each taken independently; size/sort on the MAXIMA because
/// `current_link_*` DROPS AT IDLE.
fn binding_link(bdf: &str) -> BindingLink {
    let mut mw: Option<i64> = None;
    let mut mseg: Option<String> = None;
    let mut ms: Option<f64> = None;
    let mut xw: Option<i64> = None;
    let mut xs: Option<f64> = None;
    for seg in chain_for(bdf) {
        let base = format!("{}/{}", PCI, seg);
        if let Some(w) = read_int(&format!("{}/current_link_width", base)) {
            if mw.is_none() || w < mw.unwrap() {
                mw = Some(w);
                mseg = Some(seg.clone());
            }
        }
        if let Some(s) = read_speed(&format!("{}/current_link_speed", base)) {
            if ms.is_none() || s < ms.unwrap() {
                ms = Some(s);
            }
        }
        if let Some(xwi) = read_int(&format!("{}/max_link_width", base)) {
            if xw.is_none() || xwi < xw.unwrap() {
                xw = Some(xwi);
            }
        }
        if let Some(xsi) = read_speed(&format!("{}/max_link_speed", base)) {
            if xs.is_none() || xsi < xs.unwrap() {
                xs = Some(xsi);
            }
        }
    }
    BindingLink {
        binding_element: mseg,
        min_current_w: mw,
        min_current_s: ms,
        min_max_w: xw,
        min_max_s: xs,
    }
}

fn link_gbps(width: Option<i64>, speed: Option<f64>) -> Option<f64> {
    let width = width?;
    let speed = speed?;
    let per = speed_gbps(speed)?;
    Some(round2(per * width as f64 * EFF))
}

// ── lspci name ─────────────────────────────────────────────────────────────────────────
fn lspci_name(bdf: &str) -> Option<String> {
    which("lspci")?;
    let out = Command::new("lspci")
        .args(["-mm", "-s", bdf])
        .stderr(Stdio::null())
        .output()
        .ok()?;
    let out = String::from_utf8_lossy(&out.stdout).into_owned();
    if out.contains('"') {
        let parts: Vec<&str> = out.split('"').collect();
        if parts.len() >= 8 {
            return Some(format!("{} {}", parts[5], parts[7]).trim().to_string());
        }
    }
    None
}

// ── simple glob (only the two patterns the original uses) ─────────────────────────────
/// Entries of `dir` whose file name starts with `prefix` and (if `suffix` given) which carry
/// a child of that name; returns sorted, matching Python glob's "exists" semantics closely.
fn glob_children(dir: &str, prefix: &str, suffix: Option<&str>) -> Vec<PathBuf> {
    let mut out: Vec<PathBuf> = Vec::new();
    if let Ok(rd) = fs::read_dir(dir) {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if !name.starts_with(prefix) {
                continue;
            }
            let full = Path::new(dir).join(&name);
            match suffix {
                Some(sfx) => {
                    let child = full.join(sfx);
                    if child.exists() {
                        out.push(child);
                    }
                }
                None => out.push(full),
            }
        }
    }
    out.sort();
    out
}

fn connectors(card: &str) -> (i64, i64) {
    let statuses = glob_children(DRM, &format!("{}-", card), Some("status"));
    let total = statuses.len() as i64;
    let online = statuses
        .iter()
        .filter(|p| read_trim(&p.to_string_lossy()) == Some("connected".to_string()))
        .count() as i64;
    (online, total)
}

fn render_node(card: &str) -> Option<String> {
    let dir = format!("{}/{}/device/drm", DRM, card);
    let g = glob_children(&dir, "renderD", None);
    g.first()
        .map(|p| p.file_name().unwrap().to_string_lossy().into_owned())
}

// ── minimal JSON value (order-preserving, to match json.dumps exactly) ────────────────
#[derive(Clone, Debug)]
enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    fn get<'a>(&'a self, key: &str) -> Option<&'a Json> {
        match self {
            Json::Obj(m) => m.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
    fn as_i64(&self) -> Option<i64> {
        match self {
            Json::Int(i) => Some(*i),
            _ => None,
        }
    }
    fn as_f64(&self) -> Option<f64> {
        match self {
            Json::Float(f) => Some(*f),
            Json::Int(i) => Some(*i as f64),
            _ => None,
        }
    }
    fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }
    fn as_arr(&self) -> Option<&Vec<Json>> {
        match self {
            Json::Arr(a) => Some(a),
            _ => None,
        }
    }
    fn is_truthy(&self) -> bool {
        match self {
            Json::Null => false,
            Json::Bool(b) => *b,
            Json::Int(i) => *i != 0,
            Json::Float(f) => *f != 0.0,
            Json::Str(s) => !s.is_empty(),
            Json::Arr(a) => !a.is_empty(),
            Json::Obj(m) => !m.is_empty(),
        }
    }
}

// ── JSON parser ───────────────────────────────────────────────────────────────────────
struct Parser<'a> {
    b: &'a [u8],
    i: usize,
}
impl<'a> Parser<'a> {
    fn ws(&mut self) {
        while self.i < self.b.len() && matches!(self.b[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }
    fn parse(&mut self) -> Result<Json, String> {
        self.ws();
        if self.i >= self.b.len() {
            return Err("eof".into());
        }
        match self.b[self.i] {
            b'{' => self.obj(),
            b'[' => self.arr(),
            b'"' => Ok(Json::Str(self.string()?)),
            b't' => {
                self.lit("true")?;
                Ok(Json::Bool(true))
            }
            b'f' => {
                self.lit("false")?;
                Ok(Json::Bool(false))
            }
            b'n' => {
                self.lit("null")?;
                Ok(Json::Null)
            }
            _ => self.number(),
        }
    }
    fn lit(&mut self, s: &str) -> Result<(), String> {
        if self.b[self.i..].starts_with(s.as_bytes()) {
            self.i += s.len();
            Ok(())
        } else {
            Err("bad literal".into())
        }
    }
    fn obj(&mut self) -> Result<Json, String> {
        self.i += 1; // {
        let mut m = Vec::new();
        self.ws();
        if self.i < self.b.len() && self.b[self.i] == b'}' {
            self.i += 1;
            return Ok(Json::Obj(m));
        }
        loop {
            self.ws();
            let k = self.string()?;
            self.ws();
            if self.b.get(self.i) != Some(&b':') {
                return Err("expected :".into());
            }
            self.i += 1;
            let v = self.parse()?;
            m.push((k, v));
            self.ws();
            match self.b.get(self.i) {
                Some(&b',') => {
                    self.i += 1;
                }
                Some(&b'}') => {
                    self.i += 1;
                    break;
                }
                _ => return Err("expected , or }".into()),
            }
        }
        Ok(Json::Obj(m))
    }
    fn arr(&mut self) -> Result<Json, String> {
        self.i += 1; // [
        let mut a = Vec::new();
        self.ws();
        if self.i < self.b.len() && self.b[self.i] == b']' {
            self.i += 1;
            return Ok(Json::Arr(a));
        }
        loop {
            let v = self.parse()?;
            a.push(v);
            self.ws();
            match self.b.get(self.i) {
                Some(&b',') => {
                    self.i += 1;
                }
                Some(&b']') => {
                    self.i += 1;
                    break;
                }
                _ => return Err("expected , or ]".into()),
            }
        }
        Ok(Json::Arr(a))
    }
    fn string(&mut self) -> Result<String, String> {
        if self.b.get(self.i) != Some(&b'"') {
            return Err("expected string".into());
        }
        self.i += 1;
        let mut s = String::new();
        while self.i < self.b.len() {
            let c = self.b[self.i];
            match c {
                b'"' => {
                    self.i += 1;
                    return Ok(s);
                }
                b'\\' => {
                    self.i += 1;
                    let e = *self.b.get(self.i).ok_or("bad escape")?;
                    self.i += 1;
                    match e {
                        b'"' => s.push('"'),
                        b'\\' => s.push('\\'),
                        b'/' => s.push('/'),
                        b'b' => s.push('\u{8}'),
                        b'f' => s.push('\u{c}'),
                        b'n' => s.push('\n'),
                        b'r' => s.push('\r'),
                        b't' => s.push('\t'),
                        b'u' => {
                            let h = std::str::from_utf8(&self.b[self.i..self.i + 4])
                                .map_err(|_| "bad unicode")?;
                            let mut cp = u32::from_str_radix(h, 16).map_err(|_| "bad hex")?;
                            self.i += 4;
                            if (0xD800..0xDC00).contains(&cp)
                                && self.b.get(self.i) == Some(&b'\\')
                                && self.b.get(self.i + 1) == Some(&b'u')
                            {
                                self.i += 2;
                                let h2 = std::str::from_utf8(&self.b[self.i..self.i + 4])
                                    .map_err(|_| "bad unicode")?;
                                let lo = u32::from_str_radix(h2, 16).map_err(|_| "bad hex")?;
                                self.i += 4;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                            s.push(char::from_u32(cp).unwrap_or('\u{fffd}'));
                        }
                        _ => return Err("bad escape".into()),
                    }
                }
                _ => {
                    // copy one UTF-8 sequence
                    let start = self.i;
                    let len = utf8_len(c);
                    self.i += len;
                    s.push_str(
                        std::str::from_utf8(&self.b[start..self.i]).map_err(|_| "bad utf8")?,
                    );
                }
            }
        }
        Err("unterminated string".into())
    }
    fn number(&mut self) -> Result<Json, String> {
        let start = self.i;
        while self.i < self.b.len()
            && matches!(self.b[self.i], b'0'..=b'9' | b'-' | b'+' | b'.' | b'e' | b'E')
        {
            self.i += 1;
        }
        let t = std::str::from_utf8(&self.b[start..self.i]).map_err(|_| "bad number")?;
        if t.contains('.') || t.contains('e') || t.contains('E') {
            t.parse::<f64>().map(Json::Float).map_err(|e| e.to_string())
        } else {
            match t.parse::<i64>() {
                Ok(i) => Ok(Json::Int(i)),
                Err(_) => t.parse::<f64>().map(Json::Float).map_err(|e| e.to_string()),
            }
        }
    }
}

fn utf8_len(b: u8) -> usize {
    if b < 0x80 {
        1
    } else if b >> 5 == 0b110 {
        2
    } else if b >> 4 == 0b1110 {
        3
    } else {
        4
    }
}

fn json_parse(text: &str) -> Result<Json, String> {
    let mut p = Parser {
        b: text.as_bytes(),
        i: 0,
    };
    let v = p.parse()?;
    Ok(v)
}

// ── JSON serializer matching json.dumps(..., indent=2, ensure_ascii=True) ─────────────
fn json_escape(s: &str, out: &mut String) {
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c if (c as u32) < 0x80 => out.push(c),
            c => {
                let cp = c as u32;
                if cp > 0xFFFF {
                    let v = cp - 0x10000;
                    let hi = 0xD800 + (v >> 10);
                    let lo = 0xDC00 + (v & 0x3FF);
                    out.push_str(&format!("\\u{:04x}\\u{:04x}", hi, lo));
                } else {
                    out.push_str(&format!("\\u{:04x}", cp));
                }
            }
        }
    }
}

fn json_dump_value(v: &Json, depth: usize, out: &mut String) {
    let ind = |d: usize| "  ".repeat(d);
    match v {
        Json::Null => out.push_str("null"),
        Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Json::Int(i) => out.push_str(&i.to_string()),
        Json::Float(f) => {
            if f.is_nan() {
                out.push_str("NaN");
            } else if f.is_infinite() {
                out.push_str(if *f > 0.0 { "Infinity" } else { "-Infinity" });
            } else {
                out.push_str(&py_str_float(*f));
            }
        }
        Json::Str(s) => {
            out.push('"');
            json_escape(s, out);
            out.push('"');
        }
        Json::Arr(a) => {
            if a.is_empty() {
                out.push_str("[]");
                return;
            }
            out.push_str("[\n");
            for (i, item) in a.iter().enumerate() {
                out.push_str(&ind(depth + 1));
                json_dump_value(item, depth + 1, out);
                if i + 1 < a.len() {
                    out.push(',');
                }
                out.push('\n');
            }
            out.push_str(&ind(depth));
            out.push(']');
        }
        Json::Obj(m) => {
            if m.is_empty() {
                out.push_str("{}");
                return;
            }
            out.push_str("{\n");
            for (i, (k, val)) in m.iter().enumerate() {
                out.push_str(&ind(depth + 1));
                out.push('"');
                json_escape(k, out);
                out.push_str("\": ");
                json_dump_value(val, depth + 1, out);
                if i + 1 < m.len() {
                    out.push(',');
                }
                out.push('\n');
            }
            out.push_str(&ind(depth));
            out.push('}');
        }
    }
}

fn json_dump(v: &Json) -> String {
    let mut s = String::new();
    json_dump_value(v, 0, &mut s);
    s
}

// ── probe ──────────────────────────────────────────────────────────────────────────────
fn probe() -> Json {
    if let Some(fake) = env::var_os("SPIRIT_DEVICES") {
        let path = PathBuf::from(&fake);
        if path.exists() {
            if let Ok(text) = fs::read_to_string(&path) {
                if let Ok(mut db) = json_parse(&text) {
                    if let Json::Obj(ref mut m) = db {
                        if !m.iter().any(|(k, _)| k == "gpufan") {
                            m.push(("gpufan".into(), Json::Str("probe/0.1".into())));
                        }
                    }
                    return db;
                }
            }
        }
    }

    let mut cards: Vec<String> = Vec::new();
    if let Ok(rd) = fs::read_dir(DRM) {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if is_card(&name) {
                cards.push(name);
            }
        }
    }
    cards.sort_by_key(|c| c[4..].parse::<i64>().unwrap_or(0));

    let mut gpus: Vec<Json> = Vec::new();
    for cn in &cards {
        let base = format!("{}/{}/device", DRM, cn);
        if !Path::new(&base).exists() {
            continue;
        }
        let bdf = fs::canonicalize(&base)
            .ok()
            .and_then(|p| p.file_name().map(|f| f.to_string_lossy().into_owned()))
            .unwrap_or_default();
        let vendor = read_trim(&format!("{}/vendor", base)).unwrap_or_default();
        let device = read_trim(&format!("{}/device", base)).unwrap_or_default();
        let uev = read_trim(&format!("{}/uevent", base)).unwrap_or_default();
        let driver = uev
            .lines()
            .find(|l| l.starts_with("DRIVER="))
            .and_then(|l| l.splitn(2, '=').nth(1))
            .unwrap_or("")
            .to_string();
        let bl = binding_link(&bdf);
        let own_w = read_int(&format!("{}/current_link_width", base));
        let total = read_int(&format!("{}/mem_info_vram_total", base));
        let used = read_int(&format!("{}/mem_info_vram_used", base));
        let (online, nconn) = connectors(cn);
        let integrated = total.map(|t| t < 2 * 1024 * 1024 * 1024).unwrap_or(false);

        let name = lspci_name(&bdf).unwrap_or_else(|| {
            format!(
                "{}:{}",
                tail2(&vendor),
                tail2(&device)
            )
        });

        // link sub-object
        let mw = bl.min_current_w;
        let ms = bl.min_current_s;
        let xw = bl.min_max_w;
        let xs = bl.min_max_s;
        let max_gen = match xs {
            Some(s) => match gen_name(s) {
                Some(n) => Json::Str(n.into()),
                None if s != 0.0 => Json::Str(format!("{:.1} GT/s", s)),
                None => Json::Null,
            },
            None => Json::Null,
        };
        let current_gen = match ms {
            Some(s) => match gen_name(s) {
                Some(n) => Json::Str(n.into()),
                None if s != 0.0 => Json::Str(format!("{:.1} GT/s", s)),
                None => Json::Null,
            },
            None => Json::Null,
        };
        let idle_degraded = (mw.is_some() && xw.is_some() && mw.unwrap() < xw.unwrap())
            || (ms.is_some() && xs.is_some() && ms.unwrap() < xs.unwrap());
        let capped = own_w.is_some() && xw.is_some() && own_w.unwrap() > xw.unwrap();

        let gbps_max = link_gbps(xw, xs);
        let gbps_cur = link_gbps(mw, ms);

        let link = Json::Obj(vec![
            (
                "binding_element".into(),
                bl.binding_element
                    .clone()
                    .map(Json::Str)
                    .unwrap_or(Json::Null),
            ),
            ("max_width".into(), xw.map(Json::Int).unwrap_or(Json::Null)),
            ("max_gen".into(), max_gen),
            (
                "gbps_max".into(),
                gbps_max.map(Json::Float).unwrap_or(Json::Null),
            ),
            (
                "current_width".into(),
                mw.map(Json::Int).unwrap_or(Json::Null),
            ),
            ("current_gen".into(), current_gen),
            (
                "gbps_current".into(),
                gbps_cur.map(Json::Float).unwrap_or(Json::Null),
            ),
            ("idle_degraded".into(), Json::Bool(idle_degraded)),
            (
                "own_reports_width".into(),
                own_w.map(Json::Int).unwrap_or(Json::Null),
            ),
            ("capped_by_topology".into(), Json::Bool(capped)),
        ]);

        let vram_total_mib = match total {
            Some(t) if t != 0 => Json::Int(py_round_i(t as f64 / 1048576.0)),
            _ => Json::Null,
        };
        let vram_used_mib = match used {
            Some(t) if t != 0 => Json::Int(py_round_i(t as f64 / 1048576.0)),
            _ => Json::Null,
        };

        let g = Json::Obj(vec![
            ("card".into(), Json::Str(cn.clone())),
            ("bdf".into(), Json::Str(bdf.clone())),
            ("name".into(), Json::Str(name)),
            ("driver".into(), Json::Str(driver)),
            ("vendor".into(), Json::Str(vendor)),
            ("device".into(), Json::Str(device)),
            ("integrated".into(), Json::Bool(integrated)),
            (
                "render_node".into(),
                render_node(cn).map(Json::Str).unwrap_or(Json::Null),
            ),
            ("vram_total_mib".into(), vram_total_mib),
            ("vram_used_mib".into(), vram_used_mib),
            ("display_outputs".into(), Json::Int(online)),
            ("connectors_total".into(), Json::Int(nconn)),
            ("has_output".into(), Json::Bool(online > 0)),
            ("link".into(), link),
        ]);
        gpus.push(g);
    }

    // stable sort by -(gbps_max or 0)
    gpus.sort_by(|a, b| {
        let ka = a.get("link").and_then(|l| l.get("gbps_max")).and_then(|v| v.as_f64()).unwrap_or(0.0);
        let kb = b.get("link").and_then(|l| l.get("gbps_max")).and_then(|v| v.as_f64()).unwrap_or(0.0);
        kb.partial_cmp(&ka).unwrap_or(std::cmp::Ordering::Equal)
    });

    Json::Obj(vec![
        ("gpufan".into(), Json::Str("probe/0.1".into())),
        ("gpus".into(), Json::Arr(gpus)),
    ])
}

fn is_card(name: &str) -> bool {
    if !name.starts_with("card") {
        return false;
    }
    let rest = &name[4..];
    !rest.is_empty() && rest.chars().all(|c| c.is_ascii_digit())
}

/// Python `s[2:]`.
fn tail2(s: &str) -> String {
    s.chars().skip(2).collect()
}

fn gpus_of(db: &Json) -> Vec<Json> {
    db.get("gpus")
        .and_then(|v| v.as_arr())
        .cloned()
        .unwrap_or_default()
}

// ── roles ──────────────────────────────────────────────────────────────────────────────
fn score(g: &Json) -> f64 {
    let base = g
        .get("link")
        .and_then(|l| l.get("gbps_max"))
        .and_then(|v| v.as_f64())
        .unwrap_or(0.0)
        + g.get("vram_total_mib")
            .and_then(|v| v.as_f64())
            .unwrap_or(0.0)
            / 4096.0;
    if g.get("integrated").map(|v| v.is_truthy()).unwrap_or(false) {
        base - 5.0
    } else {
        base
    }
}

fn card_of(g: &Json) -> String {
    g.get("card").and_then(|v| v.as_str()).unwrap_or("").to_string()
}

fn has_output(g: &Json) -> bool {
    g.get("has_output").map(|v| v.is_truthy()).unwrap_or(false)
}

fn assign_roles(db: &Json, _profile: &str) -> Vec<Json> {
    let g = gpus_of(db);
    let mut ranked: Vec<Json> = g.clone();
    ranked.sort_by(|a, b| {
        score(b).partial_cmp(&score(a)).unwrap_or(std::cmp::Ordering::Equal)
    });

    let mut used: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut reasons: BTreeMap<String, Vec<String>> = BTreeMap::new();

    // 1) render — ALWAYS the card feeding the monitor
    let monitor_cards: Vec<Json> = ranked.iter().filter(|c| has_output(c)).cloned().collect();
    let render: Option<Json> = if !monitor_cards.is_empty() {
        // max by score, first maximal
        let mut best: Option<&Json> = None;
        for c in &monitor_cards {
            if best.is_none() || score(c) > score(best.unwrap()) {
                best = Some(c);
            }
        }
        best.cloned()
    } else if !ranked.is_empty() {
        Some(ranked[0].clone())
    } else {
        None
    };
    let rd: Option<String> = render.as_ref().map(|r| card_of(r));
    if let Some(ref r) = render {
        let why = if has_output(r) {
            "FEEDS THE MONITOR — the main GPU by rule (rendering anywhere else costs a per-frame cross-GPU present)".to_string()
        } else {
            "strongest card by score (NO monitor detected — headless rig)".to_string()
        };
        used.entry(card_of(r)).or_default().push("render".into());
        reasons.entry(card_of(r)).or_default().push(why);
    }

    // 2) display — the render card owns the panel it presents to
    let display = render.clone();
    if let Some(ref d) = display {
        let why = if has_output(d) {
            "owns the connected panel"
        } else {
            "assumed display (no panel detected)"
        };
        used.entry(card_of(d)).or_default().push("display".into());
        reasons.entry(card_of(d)).or_default().push(why.into());
    }

    // 3) frame-gen + upscale — a SECOND card that owns a panel
    let fg = ranked
        .iter()
        .find(|c| has_output(c) && Some(card_of(c)) != rd)
        .cloned();
    if let Some(ref f) = fg {
        used.entry(card_of(f)).or_default().push("framegen".into());
        reasons.entry(card_of(f)).or_default().push(
            "second panel owner: interpolated frames are generated on the display GPU".into(),
        );
        used.entry(card_of(f)).or_default().push("upscale".into());
        reasons
            .entry(card_of(f))
            .or_default()
            .push("same card spatial-upscales the presented frame".into());
    } else {
        let cand = ranked.iter().find(|c| {
            let n = c.get("connectors_total").map(|v| v.is_truthy()).unwrap_or(false);
            n && Some(card_of(c)) != rd
        });
        if let Some(cand) = cand {
            let n = cand.get("connectors_total").and_then(|v| v.as_i64()).unwrap_or(0);
            reasons.entry(card_of(cand)).or_default().push(format!(
                "SPARE framegen/upscale target — plug a display into one of its {} connectors to activate",
                n
            ));
        }
    }

    // 4) encode — best amdgpu that is NOT the renderer
    pick(
        &ranked,
        &mut used,
        &mut reasons,
        "encode",
        &|c: &Json| {
            c.get("driver").and_then(|v| v.as_str()) == Some("amdgpu") && Some(card_of(c)) != rd
        },
        "VCN/VCE video encode (AMD has no concurrent-session limit) — records/streams off the render card",
    );

    // 5) compute — the largest VRAM OFF the render card
    let nonrender: Vec<Json> = ranked
        .iter()
        .filter(|c| Some(card_of(c)) != rd)
        .cloned()
        .collect();
    let comp: Option<Json> = {
        let mut best: Option<&Json> = None;
        for c in &nonrender {
            let kv = c.get("vram_total_mib").and_then(|v| v.as_f64()).unwrap_or(0.0);
            let kb = best
                .map(|b| b.get("vram_total_mib").and_then(|v| v.as_f64()).unwrap_or(0.0))
                .unwrap_or(f64::NEG_INFINITY);
            if best.is_none() || kv > kb {
                best = Some(c);
            }
        }
        best.cloned()
    };
    if let Some(ref c) = comp {
        used.entry(card_of(c)).or_default().push("compute".into());
        reasons.entry(card_of(c)).or_default().push(
            "largest VRAM off the render card: process-level compute (agents, LLM, Blender)".into(),
        );
    }

    // 6) analysis — a non-render card carrying the fewest roles so far
    let ana: Option<Json> = {
        let mut best: Option<&Json> = None;
        let mut best_len: Option<usize> = None;
        for c in ranked.iter().filter(|c| Some(card_of(c)) != rd) {
            let n = used.get(&card_of(c)).map(|v| v.len()).unwrap_or(0);
            if best_len.is_none() || n < best_len.unwrap() {
                best = Some(c);
                best_len = Some(n);
            }
        }
        best.cloned()
    };
    if let Some(ref a) = ana {
        used.entry(card_of(a)).or_default().push("analysis".into());
        reasons.entry(card_of(a)).or_default().push(
            "batch lane: video analysis, proxies, preview bakes (deadline-free)".into(),
        );
    }

    // 7) desktop/compositor — an integrated GPU that owns the panel
    pick(
        &ranked,
        &mut used,
        &mut reasons,
        "desktop",
        &|c: &Json| {
            c.get("integrated").map(|v| v.is_truthy()).unwrap_or(false) && has_output(c)
        },
        "drive the desktop/compositor so the gaming card keeps its VRAM for frames",
    );

    // honest advice
    if let (Some(ref render), Some(ref rd)) = (&render, &rd) {
        if has_output(render) {
            let rs = score(render);
            let stronger = ranked
                .iter()
                .find(|c| Some(card_of(c)) != Some(rd.clone()) && !has_output(c) && score(c) > rs);
            if let Some(s) = stronger {
                let nm = s.get("name").and_then(|v| v.as_str()).unwrap_or("");
                reasons.entry(rd.clone()).or_default().push(format!(
                    "NOTE: {} scores higher but feeds no monitor — plug your display into it to make IT the main GPU (we keep the monitor card as main until you do)",
                    head(nm, 26)
                ));
            }
        }
    }

    // 8) compute-pool — every remaining card with usable VRAM joins the SYSTEM lane
    for c in &ranked {
        let cn = card_of(c);
        if used.contains_key(&cn) {
            continue;
        }
        let vram = c.get("vram_total_mib").and_then(|v| v.as_f64()).unwrap_or(0.0);
        if vram >= 2048.0 {
            used.entry(cn.clone()).or_default().push("compute-pool".into());
            reasons.entry(cn).or_default().push(
                "spare card: joins the system compute pool (agents, renders, transcodes) — no in-game framegraph role, but the box uses it".into(),
            );
        } else {
            reasons.entry(cn).or_default().push(
                "too little VRAM for a useful role (integrated; shares system RAM). Plug the panel into the motherboard to make it the desktop card.".into(),
            );
        }
    }

    let mut plan: Vec<Json> = Vec::new();
    for c in &gpus_of(db) {
        let cn = card_of(c);
        let roles = used.get(&cn).cloned().unwrap_or_default();
        let why = reasons.get(&cn).cloned().unwrap_or_default();
        plan.push(Json::Obj(vec![
            ("card".into(), Json::Str(cn.clone())),
            (
                "name".into(),
                c.get("name").cloned().unwrap_or(Json::Null),
            ),
            ("bdf".into(), c.get("bdf").cloned().unwrap_or(Json::Null)),
            (
                "link_gbps_max".into(),
                c.get("link")
                    .and_then(|l| l.get("gbps_max"))
                    .cloned()
                    .unwrap_or(Json::Null),
            ),
            (
                "vram_total_mib".into(),
                c.get("vram_total_mib").cloned().unwrap_or(Json::Null),
            ),
            (
                "roles".into(),
                Json::Arr(if roles.is_empty() {
                    vec![Json::Str("idle".into())]
                } else {
                    roles.into_iter().map(Json::Str).collect()
                }),
            ),
            (
                "why".into(),
                Json::Arr(if why.is_empty() {
                    vec![Json::Str(
                        "idle — no role on this link beats doing nothing; not faked".into(),
                    )]
                } else {
                    why.into_iter().map(Json::Str).collect()
                }),
            ),
        ]));
    }
    plan
}

fn pick(
    ranked: &[Json],
    used: &mut BTreeMap<String, Vec<String>>,
    reasons: &mut BTreeMap<String, Vec<String>>,
    role: &str,
    pred: &dyn Fn(&Json) -> bool,
    why: &str,
) -> Option<Json> {
    for c in ranked {
        let cn = card_of(c);
        if used.get(&cn).map(|v| v.iter().any(|r| r == role)).unwrap_or(false) {
            continue;
        }
        if pred(c) {
            used.entry(cn.clone()).or_default().push(role.to_string());
            reasons.entry(cn).or_default().push(why.to_string());
            return Some(c.clone());
        }
    }
    None
}

// ── text renderers ─────────────────────────────────────────────────────────────────────
fn print_roles(db: &Json, profile: &str) {
    let db_roles = assign_roles(db, profile);
    let n = db_roles.len();
    let used_cards = db_roles
        .iter()
        .filter(|r| r.get("roles").and_then(|v| v.as_arr()).map(|a| a.len() != 1 || a[0].as_str() != Some("idle")).unwrap_or(false))
        .count();
    println!(
        "spirit roles — profile: {} — {} GPU(s) detected, {} with a job",
        profile, n, used_cards
    );
    println!();
    println!(
        "{}",
        format!(
            "{} {} {} {} {}",
            ljust("card", 6),
            ljust("name", 26),
            ljust("link", 9),
            ljust("VRAM", 8),
            "role(s)"
        )
    );
    println!("{}", "-".repeat(104));
    for r in &db_roles {
        let card = r.get("card").and_then(|v| v.as_str()).unwrap_or("");
        let name = r.get("name").and_then(|v| v.as_str()).unwrap_or("");
        let link = match r.get("link_gbps_max") {
            Some(v) if v.is_truthy() => opt_float_str(v.as_f64()),
            _ => "-".into(),
        };
        let vram = match r.get("vram_total_mib").and_then(|v| v.as_i64()) {
            Some(v) if v != 0 => format!("{}M", v),
            _ => "-".into(),
        };
        let roles: Vec<String> = r
            .get("roles")
            .and_then(|v| v.as_arr())
            .map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect())
            .unwrap_or_default();
        println!(
            "{}",
            format!(
                "{} {} {} {} {}",
                ljust(card, 6),
                ljust(&head(name, 26), 26),
                ljust(&link, 9),
                ljust(&vram, 8),
                roles.join(", ")
            )
        );
        if let Some(arr) = r.get("why").and_then(|v| v.as_arr()) {
            for w in arr {
                println!("        └─ {}", w.as_str().unwrap_or(""));
            }
        }
    }
    println!();
    println!("Every card is either working or explicitly idle-with-a-reason. That is the whole point:");
    println!("the Spiritual Successor uses ALL the silicon, not just the fastest card.");
}

fn print_table(db: &Json) {
    println!("spirit probe — the TRUE host link of each GPU (min negotiated width along the sysfs chain)");
    println!();
    println!(
        "{}",
        format!(
            "{} {} {} {} {} {} {} {}",
            ljust("card", 6),
            ljust("bdf", 13),
            ljust("name", 30),
            ljust("link", 9),
            ljust("GB/s est", 9),
            ljust("VRAM", 8),
            ljust("out", 6),
            "note"
        )
    );
    println!("{}", "-".repeat(118));
    for g in &gpus_of(db) {
        let link = g.get("link").unwrap();
        let card = g.get("card").and_then(|v| v.as_str()).unwrap_or("");
        let bdf = g.get("bdf").and_then(|v| v.as_str()).unwrap_or("");
        let name = g.get("name").and_then(|v| v.as_str()).unwrap_or("");
        let vram_total = g.get("vram_total_mib").and_then(|v| v.as_i64());
        let vr = match vram_total {
            Some(v) if v != 0 => format!("{} MiB", v),
            _ => "-".into(),
        };
        let integrated = g.get("integrated").map(|v| v.is_truthy()).unwrap_or(false);
        let capped = link
            .get("capped_by_topology")
            .map(|v| v.is_truthy())
            .unwrap_or(false);
        let max_width = link.get("max_width").and_then(|v| v.as_i64());
        let mut note = String::new();
        if integrated {
            note = "INTEGRATED — shares system RAM; 'link' is the CPU root port, not a card link".into();
        } else if capped {
            note = format!(
                "card says x{}, HOST gives x{}",
                link.get("own_reports_width").and_then(|v| v.as_i64()).unwrap_or(0),
                max_width.unwrap_or(0)
            );
        } else if max_width.unwrap_or(99) < 4 {
            note = "NARROW — batch/offline card".into();
        }
        if link.get("idle_degraded").map(|v| v.is_truthy()).unwrap_or(false) {
            note.push_str(&format!(
                "  [idle now: x{} {}]",
                link.get("current_width").and_then(|v| v.as_i64()).unwrap_or(0),
                link
                    .get("current_gen")
                    .and_then(|v| v.as_str())
                    .unwrap_or("None")
            ));
        }
        let link_str = if max_width.is_some() {
            format!(
                "x{} {}",
                max_width.unwrap(),
                link.get("max_gen")
                    .and_then(|v| v.as_str())
                    .unwrap_or("None")
            )
        } else {
            "-".into()
        };
        let gbps = match link.get("gbps_max") {
            Some(v) if v.is_truthy() => opt_float_str(v.as_f64()),
            _ => "-".into(),
        };
        println!(
            "{}",
            format!(
                "{} {} {} {} {} {} {} {}",
                ljust(card, 6),
                ljust(bdf, 13),
                ljust(&head(name, 30), 30),
                ljust(&link_str, 9),
                ljust(&gbps, 9),
                ljust(&vr, 8),
                ljust(if g.get("has_output").map(|v| v.is_truthy()).unwrap_or(false) {
                    "yes"
                } else {
                    "no"
                }, 6),
                note
            )
        );
    }
    println!();
    println!("Planning law: a secondary may take work only if it crosses the link ONCE or at");
    println!("FRAME rate toward the display. Per-frame PIXEL offload is dead on links < ~25 GB/s.");
}

// ── plan ────────────────────────────────────────────────────────────────────────────────
struct Workload {
    res: Option<(i64, i64)>,
    bpp: i64,
    fps: Option<i64>,
    live: bool,
    why: &'static str,
    hidden: Option<i64>,
    tokens_per_s: Option<i64>,
}

fn workloads() -> Vec<(&'static str, Workload)> {
    vec![
        (
            "1440p60-framegen",
            Workload {
                res: Some((2560, 1440)),
                bpp: 4,
                fps: Some(60),
                live: false,
                why: "frame-gen with the display on the secondary: base frames cross once, one way",
                hidden: None,
                tokens_per_s: None,
            },
        ),
        (
            "4k60-framegen",
            Workload {
                res: Some((3840, 2160)),
                bpp: 4,
                fps: Some(60),
                live: false,
                why: "same at 4K; needs ~8 GB/s sustained",
                hidden: None,
                tokens_per_s: None,
            },
        ),
        (
            "1440p-perframe-split",
            Workload {
                res: Some((2560, 1440)),
                bpp: 4,
                fps: Some(60),
                live: true,
                why: "AFR/SFR — needs the frame back EVERY frame: expect infeasible",
                hidden: None,
                tokens_per_s: None,
            },
        ),
        (
            "llm-13b",
            Workload {
                res: None,
                bpp: 2,
                live: false,
                fps: None,
                why: "process-level compute; only token/KV traffic crosses, ~KB/token",
                hidden: Some(4096),
                tokens_per_s: Some(40),
            },
        ),
    ]
}

fn plan(db: &Json, workload: &str, vram_mib: Option<i64>) -> i32 {
    let wls = workloads();
    let w = match wls.iter().find(|(n, _)| *n == workload) {
        Some((_, w)) => w,
        None => {
            let names: Vec<&str> = wls.iter().map(|(n, _)| *n).collect();
            println!("unknown workload. known: {}", names.join(", "));
            return 1;
        }
    };
    let (s, per_sec, per_frame) = if let Some((rw, rh)) = w.res {
        let s = (rw * rh * w.bpp) as f64;
        (s, s * w.fps.unwrap() as f64, true)
    } else {
        let s = (w.hidden.unwrap() * w.bpp) as f64;
        (s, s * w.tokens_per_s.unwrap() as f64, false)
    };
    println!("plan — {}", workload);
    println!("  {}", w.why);
    println!();
    println!(
        "  payload: {:.2} MB {}",
        s / 1e6,
        if per_frame { "per frame" } else { "per token" }
    );
    println!("  sustained one-way: {:.2} GB/s", per_sec / 1e9);
    println!();
    if let Some(vm) = vram_mib {
        println!(
            "  VRAM asked: {} MiB (measured on this box: a light load costs ~800 MiB, a GPU-bound 4K load ~8,200 MiB — ~10x)",
            vm
        );
        println!();
    }
    println!(
        "{}",
        format!(
            "  {} {} {} {} {} {}",
            ljust("card", 8),
            ljust("link GB/s", 11),
            ljust("capacity", 10),
            ljust("verdict", 9),
            ljust("VRAM", 8),
            "arithmetic"
        )
    );
    println!("  {}", "-".repeat(104));
    for g in &gpus_of(db) {
        let b = g.get("link").and_then(|l| l.get("gbps_max")).and_then(|v| v.as_f64());
        let b = match b {
            Some(v) if v != 0.0 => v,
            _ => continue,
        };
        let cap = b * PLAN_HEADROOM;
        let load = per_sec / 1e9;
        let (mut verdict, mut arith): (String, String) = if w.live {
            (
                "INFEASIBLE".into(),
                format!(
                    "live: result must return every frame ({:.1}% of link at 100%, latency on the critical path)",
                    load / b * 100.0
                ),
            )
        } else if load <= cap {
            (
                if load <= cap * 0.7 { "ok".into() } else { "marginal".into() },
                format!(
                    "{:.1}% of link (cap {:.1}%)",
                    load / b * 100.0,
                    PLAN_HEADROOM * 100.0
                ),
            )
        } else {
            (
                "INFEASIBLE".into(),
                format!(
                    "{:.1}% of link > {:.1}% cap",
                    load / b * 100.0,
                    PLAN_HEADROOM * 100.0
                ),
            )
        };
        let has_out = has_output(g);
        if !has_out && workload.contains("framegen") {
            let nconn = g.get("connectors_total").and_then(|v| v.as_i64()).unwrap_or(0);
            verdict = "INFEASIBLE".into();
            arith = if nconn != 0 {
                format!(
                    "no panel attached — has {} connector(s); plug a display in to use as the frame-gen target",
                    nconn
                )
            } else {
                "no display outputs at all — cannot be a frame-gen target".into()
            };
        }
        let vram = g.get("vram_total_mib").and_then(|v| v.as_i64());
        let mut vcol = match vram {
            Some(v) if v != 0 => format!("{}M", v),
            _ => "-".into(),
        };
        if let (Some(vm), Some(v)) = (vram_mib, vram) {
            if v != 0 {
                if vm > v {
                    verdict = "INFEASIBLE".into();
                    arith = format!("needs {} MiB VRAM; this card has {} MiB", vm, v);
                } else {
                    vcol = format!("{}M ok", v);
                }
            }
        }
        println!(
            "{}",
            format!(
                "  {} {} {} {} {} {}",
                ljust(g.get("card").and_then(|v| v.as_str()).unwrap_or(""), 8),
                ljust(&py_str_float(b), 11),
                ljust(&format!("{:.2} GB/s", cap), 10),
                ljust(&verdict, 9),
                ljust(&vcol, 8),
                arith
            )
        );
    }
    0
}

// ── doctor ───────────────────────────────────────────────────────────────────────────────
fn isolation_count(vid: &str, did: &str) -> Option<usize> {
    which("vulkaninfo")?;
    let out = Command::new("vulkaninfo")
        .arg("--summary")
        .env("MESA_VK_DEVICE_SELECT", format!("{}:{}!", vid, did))
        .stderr(Stdio::null())
        .output()
        .ok()?;
    let s = String::from_utf8_lossy(&out.stdout);
    Some(s.matches("deviceName").count())
}

struct Check {
    check: String,
    card: Option<String>,
    verdict: String,
    detail: String,
}

fn doctor(db: &Json, as_json: bool) -> i32 {
    let gpus = gpus_of(db);
    let mut checks: Vec<Check> = Vec::new();

    // A. the probe reads a link for every card
    let miss: Vec<String> = gpus
        .iter()
        .filter(|g| {
            g.get("link")
                .and_then(|l| l.get("gbps_max"))
                .map(|v| matches!(v, Json::Null))
                .unwrap_or(true)
        })
        .map(card_of)
        .collect();
    checks.push(Check {
        check: "probe-links".into(),
        card: None,
        verdict: if miss.is_empty() { "PASS" } else { "FAIL" }.into(),
        detail: if miss.is_empty() {
            "a host link for every card".into()
        } else {
            format!("missing links: {}", miss.join(","))
        },
    });

    // B. routing ISOLATES on every card
    if which("vulkaninfo").is_none() {
        checks.push(Check {
            check: "routing-isolation".into(),
            card: None,
            verdict: "UNKNOWN".into(),
            detail: "vulkaninfo not installed — cannot test".into(),
        });
    } else {
        for g in &gpus {
            let vendor = g.get("vendor").and_then(|v| v.as_str()).unwrap_or("");
            let device = g.get("device").and_then(|v| v.as_str()).unwrap_or("");
            let n = isolation_count(&tail2(vendor), &tail2(device));
            let (v, d) = match n {
                None => ("UNKNOWN", "could not run a test child".to_string()),
                Some(1) => ("PASS", "a child sees exactly 1 device".to_string()),
                Some(k) => ("FAIL", format!("a child sees {} devices (expected 1)", k)),
            };
            checks.push(Check {
                check: "routing-isolation".into(),
                card: Some(card_of(g)),
                verdict: v.into(),
                detail: d,
            });
        }
    }

    // C. telemetry is readable
    let n_ok = gpus
        .iter()
        .filter(|g| read_int(&format!("{}/{}/device/mem_info_vram_used", DRM, card_of(g))).is_some())
        .count();
    checks.push(Check {
        check: "telemetry".into(),
        card: None,
        verdict: if n_ok == gpus.len() { "PASS" } else { "FAIL" }.into(),
        detail: format!("VRAM counters readable on {}/{} cards", n_ok, gpus.len()),
    });

    // D. the role plan places at least one card, and says why for the rest
    let rp = assign_roles(db, "game");
    let placed = rp
        .iter()
        .filter(|r| {
            r.get("roles")
                .and_then(|v| v.as_arr())
                .map(|a| !(a.len() == 1 && a[0].as_str() == Some("idle")))
                .unwrap_or(false)
        })
        .count();
    let unexplained: Vec<String> = rp
        .iter()
        .filter(|r| {
            let idle = r
                .get("roles")
                .and_then(|v| v.as_arr())
                .map(|a| a.len() == 1 && a[0].as_str() == Some("idle"))
                .unwrap_or(false);
            let why_empty = r
                .get("why")
                .and_then(|v| v.as_arr())
                .map(|a| a.is_empty())
                .unwrap_or(true);
            idle && why_empty
        })
        .map(|r| r.get("card").and_then(|v| v.as_str()).unwrap_or("").to_string())
        .collect();
    checks.push(Check {
        check: "role-plan".into(),
        card: None,
        verdict: if placed > 0 && unexplained.is_empty() { "PASS" } else { "FAIL" }.into(),
        detail: format!(
            "places {}/{} cards{}",
            placed,
            rp.len(),
            if unexplained.is_empty() {
                String::new()
            } else {
                format!("; unexplained idle: {}", unexplained.join(","))
            }
        ),
    });

    // E. UPSCALING — the path the tool advertises actually exists on this machine.
    let (gs_state, gs_detail) = gamescope_state();
    checks.push(Check {
        check: "upscaling".into(),
        card: None,
        verdict: if gs_state == "READY" { "PASS" } else { "UNKNOWN" }.into(),
        detail: format!("{} — {}", gs_state, gs_detail),
    });

    // F. FRAME GENERATION — reported honestly; UNKNOWN is never green, and absent is not a failure
    //    of THIS machine (it needs software the user must own). Only a broken install is a FAIL.
    let (ls_state, ls_detail) = lsfg_state();
    let ls_verdict = match ls_state.as_str() {
        "READY" => "PASS",
        "NOT-WORKING" => "FAIL",
        _ => "UNKNOWN",
    };
    checks.push(Check {
        check: "frame-generation".into(),
        card: None,
        verdict: ls_verdict.into(),
        detail: format!("{} — {}", ls_state, ls_detail),
    });

    let bad = checks.iter().any(|c| c.verdict == "FAIL");
    let overall = if !bad { "READY" } else { "PROBLEMS FOUND" };

    if as_json {
        let checks_json: Vec<Json> = checks
            .iter()
            .map(|c| {
                let mut m = vec![("check".to_string(), Json::Str(c.check.clone()))];
                if let Some(ref card) = c.card {
                    m.push(("card".into(), Json::Str(card.clone())));
                }
                m.push(("verdict".into(), Json::Str(c.verdict.clone())));
                m.push(("detail".into(), Json::Str(c.detail.clone())));
                Json::Obj(m)
            })
            .collect();
        let obj = Json::Obj(vec![
            ("spirit".into(), Json::Str("doctor/0.1".into())),
            ("overall".into(), Json::Str(overall.into())),
            ("checks".into(), Json::Arr(checks_json)),
        ]);
        println!("{}", json_dump(&obj));
        return if bad { 1 } else { 0 };
    }

    println!("spirit doctor — verifying this machine's multi-GPU readiness");
    println!();
    for c in &checks {
        let card = match &c.card {
            Some(card) => format!("{} ", ljust(card, 6)),
            None => String::new(),
        };
        println!(
            "  [{}] {}{}",
            ljust(&c.verdict, 7),
            card,
            c.detail
        );
    }
    println!();
    println!(
        "  OVERALL: {}{}",
        overall,
        if !bad { "" } else { " — see FAIL lines above" }
    );
    if bad {
        1
    } else {
        0
    }
}

// ── fg ────────────────────────────────────────────────────────────────────────────────────
const FRAME_MIB: [(&str, f64); 3] = [("1080p", 8.29), ("1440p", 14.75), ("4k", 33.18)];
const HANDOFF_GBPS: [(&str, f64); 3] = [("card0", 0.76), ("card2", 0.25), ("card3", 2.91)];

fn fg(db: &Json, res: &str, mode: &str) -> i32 {
    // split on last '@' if present, else last two chars
    let (mut rr, hzs): (String, String) = if let Some(pos) = res.rfind('@') {
        (res[..pos].to_string(), res[pos + 1..].to_string())
    } else {
        let n = res.chars().count();
        let chars: Vec<char> = res.chars().collect();
        let cut = n.saturating_sub(2);
        (chars[..cut].iter().collect(), chars[cut..].iter().collect())
    };
    // r.rstrip("@")
    while rr.ends_with('@') {
        rr.pop();
    }
    let res_key = if FRAME_MIB.iter().any(|(k, _)| *k == rr) {
        rr.clone()
    } else {
        "1440p".to_string()
    };
    let hz: i64 = hzs.parse::<i64>().unwrap_or(60);
    let payload = FRAME_MIB
        .iter()
        .find(|(k, _)| *k == res_key)
        .map(|(_, v)| *v)
        .unwrap();
    let budget = 1000.0 / hz as f64;
    let one_way = mode == "present-on-secondary";

    println!(
        "spirit fg — can a card carry the frame-gen/upscale pass for {}@{}?",
        res_key, hz
    );
    println!();
    println!(
        "  frame payload: {:.2} MiB    frame budget: {:.2} ms",
        payload, budget
    );
    println!();
    println!(
        "{}",
        format!(
            "  {} {} {} {} {}",
            ljust("card", 8),
            ljust("handoff", 12),
            ljust("one-way ms", 14),
            ljust("round-trip ms", 12),
            "verdict"
        )
    );
    println!("  {}", "-".repeat(84));
    for g in &gpus_of(db) {
        let c = card_of(g);
        let rate = HANDOFF_GBPS.iter().find(|(k, _)| *k == c).map(|(_, v)| *v);
        match rate {
            None => {
                println!(
                    "{}",
                    format!(
                        "  {} {} {} {} {}",
                        ljust(&c, 8),
                        ljust("n/a", 12),
                        ljust("-", 14),
                        ljust("-", 12),
                        "not a card pair here / not measured"
                    )
                );
            }
            Some(rate) => {
                let ow = payload / 1024.0 / rate * 1000.0;
                let rt = ow * 2.0;
                let (verdict, arith) = if one_way {
                    let ok = ow <= budget;
                    (
                        if ok { "FEASIBLE one-way" } else { "INFEASIBLE one-way" },
                        format!("{:.1} ms vs {:.1} ms budget", ow, budget),
                    )
                } else {
                    let ok = rt <= budget;
                    (
                        if ok { "FEASIBLE round-trip" } else { "INFEASIBLE round-trip" },
                        format!("{:.1} ms vs {:.1} ms budget", rt, budget),
                    )
                };
                println!(
                    "{}",
                    format!(
                        "  {} {} {} {} {} ({})",
                        ljust(&c, 8),
                        ljust(&format!("{:.2} GB/s", rate), 12),
                        ljust(&format!("{:.2}", ow), 14),
                        ljust(&format!("{:.2}", rt), 12),
                        verdict,
                        arith
                    )
                );
            }
        }
    }
    println!();
    println!("  ★ ARCHITECTURE RULE, from measurement: put the DISPLAY on the card doing the frame-gen, so a");
    println!("    frame crosses ONE WAY. A round trip doubles the cost and is the first thing to fail.");
    println!("  ★ The handoff table above is MEASURED ON THE DEV BOX and is a property of its links — on any");
    println!("    other machine re-measure it before trusting a verdict. RADV lands the import in a");
    println!("    HOST-VISIBLE aperture heap, so these are aperture rates, not peer-VRAM rates.");
    0
}

// ── run ─────────────────────────────────────────────────────────────────────────────────
fn run(db: &Json, role: &str, card: &str, cmd: &[String]) -> i32 {
    let g = gpus_of(db)
        .into_iter()
        .find(|x| card_of(x) == card || x.get("bdf").and_then(|v| v.as_str()) == Some(card));
    let g = match g {
        Some(g) => g,
        None => {
            eprintln!("no such card: {}", card);
            return 2;
        }
    };
    let bdf = g.get("bdf").and_then(|v| v.as_str()).unwrap_or("");
    let vendor = g.get("vendor").and_then(|v| v.as_str()).unwrap_or("");
    let device = g.get("device").and_then(|v| v.as_str()).unwrap_or("");
    let pci = format!("pci-{}", bdf);
    let vk = format!("{}:{}!", tail2(vendor), tail2(device));

    println!(
        "# routing role={} to {} ({}, link {} GB/s)",
        role,
        card_of(&g),
        head(g.get("name").and_then(|v| v.as_str()).unwrap_or(""), 34),
        opt_float_str(g.get("link").and_then(|l| l.get("gbps_max")).and_then(|v| v.as_f64()))
    );
    println!("# env: DRI_PRIME={}  MESA_VK_DEVICE_SELECT={}", pci, vk);

    let mut child_env: Vec<(OsString, OsString)> = Vec::new();
    child_env.push(("DRI_PRIME".into(), pci.clone().into()));
    child_env.push(("MESA_VK_DEVICE_SELECT".into(), vk.clone().into()));
    if role == "compute" {
        child_env.push(("GGML_VK_VISIBLE_DEVICES".into(), bdf.into()));
    }
    let render_node = g.get("render_node").and_then(|v| v.as_str());
    if role == "encode" {
        if let Some(rn) = render_node {
            child_env.push((
                "SPIRIT_VAAPI_DEVICE".into(),
                format!("/dev/dri/{}", rn).into(),
            ));
            println!("#      add to ffmpeg:  -vaapi_device /dev/dri/{}", rn);
        }
    }
    if role == "compute" {
        println!("#      ggml: GGML_VK_VISIBLE_DEVICES={}", bdf);
    }
    let _ = std::io::stdout().flush();

    if cmd.is_empty() {
        return 0;
    }
    let mut c = Command::new(&cmd[0]);
    c.args(&cmd[1..]);
    for (k, v) in &child_env {
        c.env(k, v);
    }
    match c.status() {
        Ok(st) => st.code().unwrap_or(0),
        Err(e) => {
            eprintln!("spirit: failed to execute: {}", e);
            1
        }
    }
}

// ── boost: what extra frames can THIS machine give you, right now ───────────────────────
// Two ways to more frames, and they are NOT the same thing: UPSCALING (render below native and
// scale up — gamescope FSR 1.0) and FRAME GEN (interpolate extra frames — lsfg-vk, which the
// user must own; we detect it and drive it, we never ship it).

/// (state, detail) for the present counter. READY / PARTIAL / ABSENT — never a silent pass.
fn counter_state() -> (String, String) {
    if !Path::new(COUNTER_LAYER).exists() {
        return (
            "ABSENT".into(),
            "the present counter layer is not installed, so frame-gen output can only \
be ASSERTED, not measured"
                .into(),
        );
    }
    if !Path::new(COUNTER_LIB).exists() {
        return (
            "PARTIAL".into(),
            format!("manifest is present but {} is missing", COUNTER_LIB),
        );
    }
    (
        "READY".into(),
        "present counter installed — frame-gen output is measurable".into(),
    )
}

/// The counter file is a fixed-width decimal the layer rewrites on every present.
fn read_counter(path: &str) -> i64 {
    let raw = fs::read_to_string(path)
        .map(|s| s.trim().to_string())
        .unwrap_or_default();
    if raw.is_empty() {
        return 0;
    }
    py_int(&raw).unwrap_or(0)
}

/// (state, detail). READY / ABSENT / PARTIAL / UNKNOWN — never a silent pass.
fn gamescope_state() -> (String, String) {
    if which("gamescope").is_none() {
        return ("ABSENT".into(), "gamescope is not installed".into());
    }
    let out = match Command::new("gamescope").arg("--help").output() {
        Ok(o) => o,
        Err(e) => return ("UNKNOWN".into(), format!("gamescope --help failed ({})", e)),
    };
    // ★ gamescope prints its ENTIRE help to STDERR, not stdout. Read BOTH streams.
    let mut combined = String::from_utf8_lossy(&out.stdout).into_owned();
    combined.push('\n');
    combined.push_str(&String::from_utf8_lossy(&out.stderr));
    let low = combined.to_lowercase();
    if low.contains("fsr") {
        ("READY".into(), "gamescope present; FSR 1.0 upscaling available".into())
    } else {
        ("PARTIAL".into(), "gamescope present but no fsr filter is listed in --help".into())
    }
}

/// (state, detail) for lsfg-vk — checked against the surfaces that OWN the answer.
/// ★ NOT `lsfg-vk-cli validate`: that only validates its own config file and reports a clean
/// "valid" while the runtime cannot initialise at all. The manifest is what distinguishes a
/// working install from a broken one.
fn lsfg_state() -> (String, String) {
    let dir = Path::new(LSFG_LAYER)
        .parent()
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_default();
    let base = Path::new(LSFG_LAYER)
        .file_name()
        .map(|f| f.to_string_lossy().into_owned())
        .unwrap_or_default();
    let prefix = format!("{}.disabled", base);
    let mut stood: Vec<String> = Vec::new();
    if let Ok(rd) = fs::read_dir(&dir) {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if name.starts_with(&prefix) {
                stood.push(format!("{}/{}", dir, name));
            }
        }
    }
    stood.sort();
    let layer_exists = Path::new(LSFG_LAYER).exists();
    if !stood.is_empty() && !layer_exists {
        return (
            "STOOD-DOWN".into(),
            format!(
                "installed but disabled at the loader; enable with: sudo mv {} {}",
                stood[stood.len() - 1], LSFG_LAYER
            ),
        );
    }
    if !layer_exists {
        return ("ABSENT".into(), "lsfg-vk is not installed".into());
    }
    let ldir = lossless_dir();
    if !Path::new(&ldir).exists() {
        return (
            "NEEDS-GAME".into(),
            "Lossless Scaling is not in the Steam library".into(),
        );
    }
    if !Path::new(&format!("{}/Lossless.dll", ldir)).exists() {
        return (
            "NEEDS-GAME".into(),
            "no Lossless.dll in the Lossless Scaling install".into(),
        );
    }
    let man = match fs::read(&steam_manifest()) {
        Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
        Err(_) => String::new(),
    };
    // ★ the manifest spells it "BetaKey"; a lowercase literal never matches. Compare folded.
    let man = man.to_lowercase();
    if !man.contains("betakey") || !man.contains("\"lsfg-vk\"") {
        return (
            "NEEDS-BRANCH".into(),
            "Lossless Scaling is on Steam's DEFAULT branch; switch it to the \"lsfg-vk\" beta (Steam > Lossless Scaling > Properties > Betas). Without it the frame-gen shaders are absent and the layer fails with \"The specified shader DLL does not exist\"".into(),
        );
    }
    if which("lsfg-vk-cli").is_none() {
        return ("PARTIAL".into(), "layer present but lsfg-vk-cli is missing".into());
    }
    (
        "INSTALLED".into(),
        "layer + the lsfg-vk branch are present (path not exercised until a game runs)".into(),
    )
}

/// The preferred mode of a connected output on the monitor card, or None. Rootless sysfs read.
fn native_res(db: &Json) -> Option<(i64, i64)> {
    let main = main_card(db)?;
    let card = card_of(&main);
    let paths = glob_children(DRM, &format!("{}-", card), Some("modes"));
    let mut best: Option<(i64, i64)> = None;
    for p in &paths {
        let content = match fs::read_to_string(p) {
            Ok(c) => c,
            Err(_) => continue,
        };
        let first = content.split('\n').next().unwrap_or("").trim().to_string();
        if let Some((w, h)) = parse_res(&first) {
            if w < 1024 {
                continue;
            }
            if best.is_none() || w * h > best.unwrap().0 * best.unwrap().1 {
                best = Some((w, h));
            }
        }
    }
    best
}

/// Python `re.match(r"^(\d+)x(\d+)$", s)` for a resolution string.
fn parse_res(s: &str) -> Option<(i64, i64)> {
    let idx = s.find('x')?;
    let (a, b) = (&s[..idx], &s[idx + 1..]);
    if a.is_empty() || b.is_empty() {
        return None;
    }
    if !a.bytes().all(|c| c.is_ascii_digit()) || !b.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    Some((a.parse::<i64>().ok()?, b.parse::<i64>().ok()?))
}

/// Render resolution for a scale factor, forced even. ★ ROUND, do not truncate.
fn render_res(native: (i64, i64), factor: f64) -> (i64, i64) {
    let w = std::cmp::max(2, py_round_i(native.0 as f64 * factor)) / 2 * 2;
    let h = std::cmp::max(2, py_round_i(native.1 as f64 * factor)) / 2 * 2;
    (w, h)
}

/// Make sure a profile exists for this multiplier. Returns (profile_name, note). Never silent.
fn ensure_lsfg_profile(multiplier: i64) -> (Option<String>, String) {
    let conf = lsfg_conf();
    let name = format!("spirit {}x", multiplier);
    let text = match fs::read_to_string(&conf) {
        Ok(t) => t,
        Err(_) => {
            return (
                None,
                format!(
                    "no lsfg-vk config at {} — create a profile in lsfg-vk-ui first",
                    conf
                ),
            )
        }
    };
    if text.contains(&format!("name = \"{}\"", name)) {
        return (Some(name.clone()), format!("profile {} already present", py_repr_str(&name)));
    }
    let block = format!(
        "\n[[profile]]\nactive_in = []\nflow_scale = 1.0\nmultiplier = {}\nname = \"{}\"\noverride_present_mode = true\npacing_mode = \"vsync\"\nperformance_mode = false\npreserve_swapchain_image_count = false\n",
        multiplier, name
    );
    // ★ KEEP THE USER'S ORIGINAL, ONCE (fix 2026-10-11, from the outside review). This copied
    // unconditionally, so a SECOND multiplier overwrote the backup with the already-modified file —
    // and the user's original lsfg-vk config was then gone for good. A backup that is rewritten on
    // every use is not a backup; it is a copy of the last thing we did.
    let bak = format!("{}.bak-spirit", conf);
    let write_res: std::io::Result<()> = (|| {
        if !Path::new(&bak).exists() {
            fs::copy(&conf, &bak)?;
        }
        let mut fh = fs::OpenOptions::new().append(true).open(&conf)?;
        fh.write_all(block.as_bytes())
    })();
    if let Err(e) = write_res {
        return (None, format!("could not write the profile ({})", e));
    }
    (
        Some(name.clone()),
        format!(
            "added profile {} (backup: {}.bak-spirit)",
            py_repr_str(&name),
            conf
        ),
    )
}

fn boost(db: &Json) -> i32 {
    let (gs_state, gs_detail) = gamescope_state();
    let (ls_state, ls_detail) = lsfg_state();
    let native = native_res(db);

    println!("spirit boost — what extra frames this machine can give you, right now");
    println!();
    println!("  UPSCALING — render below native and scale up. Fewer pixels rendered = real FPS.");
    println!("    gamescope : {} {}", ljust(&gs_state, 11), gs_detail);
    match native {
        Some((nw, nh)) => {
            println!("    display   : {}x{} native, on the monitor card", nw, nh);
            for f in [0.9_f64, 0.8, 0.7, 0.6] {
                let (rw, rh) = render_res((nw, nh), f);
                println!(
                    "      --scale {:.1}  ->  render {}x{}  ({:.0}% of the pixels)",
                    f,
                    rw,
                    rh,
                    100.0 * f * f
                );
            }
        }
        None => {
            println!("    display   : no connected output's preferred mode could be read");
        }
    }
    println!();
    println!("  FRAME GENERATION — interpolated frames. The big multiplier, and the one that needs");
    println!("  software we do not ship: lsfg-vk, which requires owning Lossless Scaling.");
    println!("    lsfg-vk   : {} {}", ljust(&ls_state, 11), ls_detail);
    let (c_state, c_detail) = counter_state();
    println!("    witness   : {} {}", ljust(&c_state, 11), c_detail);
    println!("                (our own present counter, below the frame-gen layer — the only");
    println!("                 instrument that can see an INSERTED frame; without it a frame-gen");
    println!("                 claim is an assertion, not a measurement)");
    println!();
    if gs_state == "READY" {
        println!("  ⇒ Upscaling is available NOW:   spirit play --scale 0.7 -- <your game>");
    } else {
        println!("  ⇒ Upscaling is not ready: {}", gs_detail);
    }
    if ls_state == "INSTALLED" {
        println!("  ⇒ Frame generation is available: spirit play --framegen 2 -- <your game>");
    } else {
        println!("  ⇒ Frame generation is not ready: {}", ls_detail);
    }
    println!();
    println!("  ★ Neither one is a miracle, and this tool will not pretend otherwise. Upscaling adds");
    println!("    frames by rendering fewer pixels. Frame generation adds frames by inventing them,");
    println!("    and it can look wrong on fast motion — that is the technology, not this tool.");
    0
}

// ── play: the game path — the strongest card renders, every other card works the tail ──
fn main_card(db: &Json) -> Option<Json> {
    let gpus = gpus_of(db);
    let panel: Vec<Json> = gpus.iter().filter(|g| has_output(g)).cloned().collect();
    let pool: Vec<Json> = if !panel.is_empty() { panel } else { gpus.clone() };
    let mut best: Option<&Json> = None;
    for c in &pool {
        if best.is_none() || score(c) > score(best.unwrap()) {
            best = Some(c);
        }
    }
    best.cloned()
}

// ── ORIGIN — the mark. Every run prints it; break it and the tool says so, loudly, forever.
// Hiding a trace in the code is an old tradition, and it works here because this is not a comment —
// it is a CHECK. A comment gets deleted by accident; a check that reads its own notice can only be
// removed ON PURPOSE. The honest limit: a determined person can rip the whole check out. What it
// guarantees is that taking the credit is a decision, not an oversight.
const ORIGIN_NOTICE: &str = "Spiritual Successor by Sophia Marie DeClue — github.com/Sophia-Thickums/spiritual-successor";
const ORIGIN_SEAL: &str = "74c06104c43a4c80";
const ORIGIN_TRUE: &str = "Spiritual Successor by Sophia Marie DeClue — github.com/Sophia-Thickums/spiritual-successor";

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

/// `os.path.abspath` — absolute, lexically normalised, WITHOUT touching the filesystem.
fn abspath(p: &str) -> String {
    match std::path::absolute(p) {
        Ok(a) => a.to_string_lossy().to_string(),
        Err(_) => p.to_string(),
    }
}

/// `os.path.dirname` — the lexical parent, "" when there is none.
fn dirname(p: &str) -> String {
    match p.rfind('/') {
        None => String::new(),
        Some(0) => "/".to_string(),
        Some(i) => p[..i].to_string(),
    }
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

// ★ THE ENCODER DRIVER IS NOT ALWAYS radeonsi (fix 2026-10-11, found by the outside review).
// `LIBVA_DRIVER_NAME=radeonsi` was hard-coded at both launch sites, so a spare card that is not AMD was
// handed AMD's VAAPI driver. With `--work` the job then failed and printed MISSING; with a test load it
// failed SILENTLY, because ffmpeg's own output goes to /dev/null. That made two README claims untrue in
// practice — "different makers, different ages" and "AMD and Intel work today" — for the one half of the
// machine the offload is about. The driver is now chosen from the card's OWN kernel driver, and a card
// with no VAAPI encoder we know of is SKIPPED AND SAID OUT LOUD rather than quietly doing nothing.
const VA_DRIVERS: [(&str, &str); 3] = [("amdgpu", "radeonsi"), ("i915", "iHD"), ("xe", "iHD")];

/// `(driver, None)` if this card can encode, or `(None, why not)`. Never a guess, never a default.
fn va_driver(g: &Json) -> (Option<String>, Option<String>) {
    let d = g
        .get("driver")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_lowercase();
    for (k, v) in VA_DRIVERS {
        if d == k {
            return (Some(v.to_string()), None);
        }
    }
    let shown = if d.is_empty() { "unknown".to_string() } else { d };
    (
        None,
        Some(format!(
            "no VAAPI encoder this tool knows of (kernel driver: {})",
            shown
        )),
    )
}

/// A SYNTHETIC load: ffmpeg's OWN generated test pattern (testsrc2), piped to /dev/null. The encoder
/// really runs — that is why the counters move — but the input is a test card and the output is
/// discarded. A LOAD proves a card can be DRIVEN; a JOB produces something. --work is the job.
fn start_test_load(node: &str, va: &str) -> std::io::Result<Child> {
    Command::new("ffmpeg")
        .args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=1920x1080:rate=60",
            "-vaapi_device",
            &format!("/dev/dri/{}", node),
            "-vf",
            "format=nv12,hwupload",
            "-c:v",
            "h264_vaapi",
            "-f",
            "null",
            "-",
        ])
        .env("LIBVA_DRIVER_NAME", va)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
}

/// True if this is a Proton/Wine launch, where the mount-namespace wall MUST be skipped.
///
/// ★ MEASURED 2026-10-10: `unshare -r` maps our uid to 0 inside, and Wine/Proton REFUSE TO RUN AS
/// ROOT — the launch dies with `bin_steam.sh: Error: Cannot run as root user` before the game ever
/// starts. So on any Proton title (i.e. most Windows games, which is most of the audience) our
/// second wall breaks the launch outright. Detect it and fall back to the pin alone, which is the
/// load-bearing half anyway, and SAY SO rather than silently weakening the wall.
///
/// ★ NO ENV LOOKUP HERE. An earlier version also checked STEAM_COMPAT_DATA_PATH, which meant a
/// stray variable left in a shell silently disabled the wall for NATIVE games too — the same
/// ambient-state trap that let SPIRIT_DEVICES poison a whole session. Decide on the ARGV, which
/// is what actually describes the launch.
/// A REAL job on a spare card, to a real file — the WHOLE input, or one SEGMENT of it.
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
    va: &str,
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
        .env("LIBVA_DRIVER_NAME", va)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;
    Ok((out, ch))
}

/// The card's short name, as the role table prints it.
fn gpu_name(g: &Json) -> String {
    let mut n = g.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string();
    if n.is_empty() {
        n = g.get("card").and_then(|v| v.as_str()).unwrap_or("?").to_string();
    }
    for cut in ["[", "(", "  "] {
        if let Some(i) = n.find(cut) { if i > 0 { n = n[..i].to_string(); } }
    }
    n = n.trim_end().to_string();
    if n.len() > 22 { n = n[..22].to_string(); }
    n
}

fn is_wine_target(cmd: &[String]) -> bool {
    for a in cmd {
        let b = Path::new(a)
            .file_name()
            .map(|s| s.to_string_lossy().to_lowercase())
            .unwrap_or_default();
        if b.ends_with(".exe") || b.contains("proton") || b == "wine" || b.starts_with("wine") {
            return true;
        }
    }
    false
}

/// The Steam install that has a `steamapps` directory, or None. Never guessed from a path shape.
fn steam_root() -> Option<String> {
    let home = env::var("HOME").unwrap_or_default();
    for c in [
        format!("{}/.steam/steam", home),
        format!("{}/.local/share/Steam", home),
    ] {
        if Path::new(&format!("{}/steamapps", c)).is_dir() {
            return Some(c);
        }
    }
    None
}

/// Python `re.search(r"steamapps/common/([^/]+)/", s)` — the folder name after `steamapps/common/`.
fn steamapps_common_folder(s: &str) -> Option<String> {
    let needle = "steamapps/common/";
    let mut from = 0;
    while let Some(rel) = s[from..].find(needle) {
        let start = from + rel + needle.len();
        let rest = &s[start..];
        if let Some(end) = rest.find('/') {
            if end > 0 {
                return Some(rest[..end].to_string());
            }
        }
        from = start;
    }
    None
}

/// Python `re.search(r'"installdir"\s*"([^"]+)"', txt)` — the value of the `installdir` key.
fn installdiry(txt: &str) -> Option<String> {
    let needle = "\"installdir\"";
    let i = txt.find(needle)?;
    let rest = txt[i + needle.len()..].trim_start();
    let rest = rest.strip_prefix('"')?;
    let end = rest.find('"')?;
    if end == 0 {
        return None;
    }
    Some(rest[..end].to_string())
}

/// The Steam appid that owns this launch, resolved through the manifest's `installdir`.
///
/// ★ NOT PARSED OFF THE PATH SPELLING. The manifest is the surface that owns the mapping; returns
/// None rather than a guess — an appid invented here would create the wrong directory.
fn appid_for_cmd(cmd: &[String]) -> Option<String> {
    let steam = steam_root()?;
    let apps = format!("{}/steamapps", steam);
    let mut wanted: Vec<String> = Vec::new();
    for a in cmd {
        if let Some(f) = steamapps_common_folder(a) {
            if !wanted.contains(&f) {
                wanted.push(f);
            }
        }
    }
    if wanted.is_empty() {
        return None;
    }
    let mut manifests: Vec<PathBuf> = Vec::new();
    if let Ok(rd) = fs::read_dir(&apps) {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if name.starts_with("appmanifest_") && name.ends_with(".acf") {
                manifests.push(Path::new(&apps).join(&name));
            }
        }
    }
    manifests.sort();
    for mf in manifests {
        let txt = match fs::read_to_string(&mf) {
            Ok(t) => t,
            Err(_) => continue,
        };
        if let Some(v) = installdiry(&txt) {
            if wanted.contains(&v) {
                let base = mf.file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
                let inner = base
                    .strip_prefix("appmanifest_")
                    .unwrap_or(&base)
                    .strip_suffix(".acf")
                    .unwrap_or(&base)
                    .to_string();
                return Some(inner);
            }
        }
    }
    None
}

/// Create `steamapps/compatdata/<appid>/` when it is absent, before a Proton launch.
///
/// ★ Proton's `setup_prefix()` locks a file INSIDE `compatdata/<appid>/`; if the DIRECTORY does not
/// exist, Proton dies with a bare FileNotFoundError it cannot create its way out of. One
/// `create_dir_all` is the whole fix; a no-op for a title that has run before.
fn ensure_compatdata(cmd: &[String]) {
    if !is_wine_target(cmd) {
        return;
    }
    let appid = match appid_for_cmd(cmd) {
        Some(a) => a,
        None => return,
    };
    let steam = match steam_root() {
        Some(s) => s,
        None => return,
    };
    let d = Path::new(&steam).join("steamapps").join("compatdata").join(&appid);
    if d.join("pfx").is_dir() {
        return; // a real prefix is already there; touch nothing
    }
    let made = !d.is_dir();
    // ★ THE ERROR TEXT IS DELIBERATELY NOT INTERPOLATED HERE. Each language spells its own runtime's
    // error differently (Rust an io::Error Display, Python an OSError, C++ strerror), so including it
    // makes this the one branch the three builds CANNOT agree on — and a read-only Steam directory or a
    // full disk reaches it. Same words in every build instead, naming the thing to check.
    if fs::create_dir_all(&d).is_err() {
        println!("  prefix: could not create {}", d.to_string_lossy());
        println!("          (Proton needs this directory for its prefix; check that you can write to it)");
        return;
    }
    if made {
        println!("  prefix: FIRST-EVER LAUNCH of appid {} — created {}", appid, d.to_string_lossy());
        println!("          (Proton's own prefix setup crashes on a missing compatdata dir; it cannot");
        println!("           create the directory it needs, because its lock file lives inside it)");
    }
}

fn hidden_argv(cmd: &[String], main: &Json, db: &Json) -> Vec<String> {
    if is_wine_target(cmd) {
        return cmd.to_vec();
    }
    let mut hide: Vec<String> = Vec::new();
    for g in &gpus_of(db) {
        if card_of(g) == card_of(main) {
            continue;
        }
        let card = card_of(g);
        if !card.is_empty() {
            hide.push(card);
        }
        if let Some(n) = g.get("render_node").and_then(|v| v.as_str()) {
            if !n.is_empty() {
                hide.push(n.to_string());
            }
        }
    }
    if !hide.is_empty() && which("unshare").is_some() {
        // ★ A WALL THAT FAILS SILENTLY IS NOT A WALL. This used to end every bind with `|| true`, so a
        // failed hide said nothing at all and the game could still see the other cards while the run
        // looked identical to a good one. It now says so, on stderr, at the moment it happens.
        // (Found by the outside review, 2026-10-11.)
        let script = "hide=\"$1\"; shift; for n in $hide; do if [ -e \"/dev/dri/$n\" ]; then mount --bind /dev/null \"/dev/dri/$n\" 2>/dev/null || echo \"  wall: could NOT hide /dev/dri/$n - the game can still see this card\" >&2; fi; done; exec \"$@\"";
        let mut v: Vec<String> = vec![
            "unshare".into(),
            "-rm".into(),
            "sh".into(),
            "-c".into(),
            script.into(),
            "sh".into(),
            hide.join(" "),
        ];
        v.extend(cmd.iter().cloned());
        return v;
    }
    cmd.to_vec()
}

fn spawn_argv(argv: &[String], env_pins: &[(OsString, OsString)]) -> i32 {
    if argv.is_empty() {
        return 0;
    }
    let mut c = Command::new(&argv[0]);
    c.args(&argv[1..]);
    for (k, v) in env_pins {
        c.env(k, v);
    }
    match c.status() {
        Ok(st) => st.code().unwrap_or(0),
        Err(_) => 1,
    }
}

fn play(db: &Json, cmd: &[String], do_offload: bool, scale: Option<f64>, framegen: Option<i64>,
        overlay: bool, work: &str, work_outdir: &str, split: bool, only_spares: &str) -> i32 {
    if cmd.is_empty() {
        eprintln!("usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] \
[--work FILE [--work-outdir DIR] [--split]] [--spare NODE,...] -- <game command...>");
        return 1;
    }
    let cmd: Vec<String> = cmd.to_vec();
    let main = match main_card(db) {
        Some(m) => m,
        None => return 1,
    };
    let mut spares: Vec<Json> = gpus_of(db)
        .into_iter()
        .filter(|g| card_of(g) != card_of(&main))
        .collect();
    if !only_spares.is_empty() {
        // ★ --spare NODE[,NODE...] — the human names the cards that work. Everything else is left
        // alone entirely, not merely idle. A real session runs for hours unattended, and an old card
        // on a marginal link is the human's risk to take, not a default we assume for them.
        let want: Vec<&str> = only_spares
            .split([',', ' '])
            .filter(|w| !w.is_empty())
            .collect();
        let kept: Vec<Json> = spares
            .iter()
            .filter(|g| {
                let n = g.get("render_node").and_then(|v| v.as_str()).unwrap_or("");
                want.iter().any(|w| n.rsplit('/').next().unwrap_or(n) == *w)
            })
            .cloned()
            .collect();
        if kept.is_empty() {
            println!("  spare: REFUSED — none of {} is a spare card here. The game would run with no",
                     only_spares);
            println!("         cards working beside it.");
            return 1;
        }
        let kepts: Vec<String> = kept
            .iter()
            .map(|g| {
                let n = g.get("render_node").and_then(|v| v.as_str()).unwrap_or("");
                n.rsplit('/').next().unwrap_or(n).to_string()
            })
            .collect();
        println!("  spare: {} of {} card(s) allowed to work: {}", kept.len(), spares.len(), kepts.join(", "));
        spares = kept;
    }

    println!("spirit play — the game on the strongest card, every other card working the tail");
    println!();
    let l = main.get("link").unwrap();
    println!(
        "{}",
        format!(
            "  MAIN  (renders)  {} {} x{} {}  {} GB/s",
            ljust(&card_of(&main), 6),
            ljust(&head(main.get("name").and_then(|v| v.as_str()).unwrap_or(""), 30), 30),
            l.get("max_width").and_then(|v| v.as_i64()).map(|v| v.to_string()).unwrap_or_else(|| "None".into()),
            l.get("max_gen").and_then(|v| v.as_str()).unwrap_or("None"),
            opt_float_str(l.get("gbps_max").and_then(|v| v.as_f64()))
        )
    );
    for g in &spares {
        println!(
            "{}",
            format!(
                "  SPARE (offload)  {} {} node /dev/dri/{}",
                ljust(&card_of(g), 6),
                ljust(&head(g.get("name").and_then(|v| v.as_str()).unwrap_or(""), 30), 30),
                g.get("render_node").and_then(|v| v.as_str()).unwrap_or("?")
            )
        );
    }

    let bdf = main.get("bdf").and_then(|v| v.as_str()).unwrap_or("");
    let vendor = main.get("vendor").and_then(|v| v.as_str()).unwrap_or("");
    let device = main.get("device").and_then(|v| v.as_str()).unwrap_or("");
    let pci = format!("pci-{}", bdf);
    let vk = format!("{}:{}!", tail2(vendor), tail2(device));
    let mut env_pins: Vec<(OsString, OsString)> = vec![
        ("DRI_PRIME".into(), pci.clone().into()),
        ("MESA_VK_DEVICE_SELECT".into(), vk.clone().into()),
    ];
    println!();
    println!("  pin: MESA_VK_DEVICE_SELECT={}   DRI_PRIME={}", vk, pci);

    // UPSCALING: validate and report. The gamescope wrapper is applied AFTER the mount namespace
    // is built (below), so gamescope stays OUTSIDE the namespace.
    if let Some(s) = scale {
        let (gs_state, gs_detail) = gamescope_state();
        let native = native_res(db);
        if gs_state != "READY" {
            println!("  scale: REFUSED — {}", gs_detail);
            return 1;
        }
        let native = match native {
            Some(n) => n,
            None => {
                println!("  scale: REFUSED — could not read the native mode of the monitor card's output");
                return 1;
            }
        };
        let (rw, rh) = render_res(native, s);
        println!(
            "  scale: render {}x{} -> present {}x{} via gamescope FSR 1.0  ({:.0}% of the pixels)",
            rw, rh, native.0, native.1, 100.0 * s * s
        );
    }

    // FRAME GENERATION: drive lsfg-vk if it is here. We never ship or modify it.
    if let Some(n) = framegen {
        let (st, detail) = lsfg_state();
        // ★ "READY" was renamed to "INSTALLED" when this check moved to the Steam manifest.
        // A rename that misses one caller is exactly how a working feature reports itself broken.
        if st != "INSTALLED" {
            println!("  framegen: UNAVAILABLE — {}", detail);
            println!("            (launching without it; spirit boost explains the state)");
        } else {
            let (prof, note) = ensure_lsfg_profile(n);
            match prof {
                Some(p) => {
                    env_pins.push(("LSFGVK_PROFILE".into(), p.clone().into()));
                    if scale.is_some() || overlay {
                        // ★ lsfg-vk's own troubleshooting: under gamescope with vsync pacing,
                        // set ENABLE_GAMESCOPE_WSI=0 or frame generation does not engage at all.
                        // This applies whenever gamescope is in play — including --overlay alone, with no upscaling.
                        env_pins.push(("ENABLE_GAMESCOPE_WSI".into(), "0".into()));
                    }
                    println!(
                        "  framegen: {}x via lsfg-vk, profile {} — {}",
                        n,
                        py_repr_str(&p),
                        note
                    );
                }
                None => {
                    println!("  framegen: UNAVAILABLE — {}", note);
                }
            }
        }
    }

    // ★ ARM THE WITNESS. Only meaningful with frame gen on; harmless otherwise, and we say so
    // plainly if it is missing rather than reporting a frame-gen claim we could not measure.
    let mut fg_count_path: Option<String> = None;
    if framegen.is_some() {
        let (cst, cdet) = counter_state();
        if cst == "READY" {
            let path = std::env::temp_dir().join(format!("spirit-presents-{}.txt", std::process::id()));
            let path_s = path.to_string_lossy().to_string();
            let _ = fs::remove_file(&path);
            env_pins.push(("SPIRIT_PRESENT_COUNT".into(), path_s.clone().into()));
            fg_count_path = Some(path_s);
            println!("  witness: counting images presented to the compositor (summary at exit)");
        } else {
            println!("  witness: UNAVAILABLE — {}", cdet);
        }
    }

    if is_wine_target(&cmd) {
        println!("  wall: Proton/Wine detected — skipping the mount namespace (Wine refuses to run as");
        println!("        root, and `unshare -r` makes us root); the exclusive pin still applies.");
        // ★ Before the launch, make sure Proton has somewhere to put its prefix. A first-ever
        // launch otherwise dies inside Proton with a bare FileNotFoundError on a lock file in a
        // directory Proton cannot create. Cheap, idempotent, and it speaks only when it mattered.
        ensure_compatdata(&cmd);
    }

    let mut procs: Vec<Child> = Vec::new();
    let mut proc_nodes: Vec<String> = Vec::new(); // parallel to procs — which card each test load went to
    let mut died_early: Vec<(String, i32)> = Vec::new(); // node -> rc, loads already gone at game exit
    let mut job_outputs: Vec<(String, String)> = Vec::new();
    let mut job_expect: f64 = -1.0;
    // ★ WHICH SPARES CAN ACTUALLY ENCODE, decided from each card's own kernel driver, before anything
    // is launched. A card we cannot drive is left out and NAMED — the alternative is an ffmpeg that
    // dies instantly with its output going to /dev/null, which looks exactly like a card doing work.
    let mut encoders: Vec<(String, String)> = Vec::new(); // [(render_node, va_driver)]
    let mut skipped: Vec<(String, String, String)> = Vec::new(); // [(render_node_or_card, name, reason)]
    for g in &spares {
        let node = g.get("render_node").and_then(|v| v.as_str()).unwrap_or("");
        let (va, why) = va_driver(g);
        if va.is_some() && !node.is_empty() {
            encoders.push((base_of(node), va.unwrap()));
        } else {
            let key = if node.is_empty() { card_of(g) } else { base_of(node) };
            skipped.push((key, gpu_name(g), why.unwrap_or_else(|| "no render node".to_string())));
        }
    }
    if do_offload && which("ffmpeg").is_some() {
        for (node, nm, why) in &skipped {
            println!("  spare: SKIPPED {} ({}) — {}", node, nm, why);
        }
        let targets: Vec<(String, String)> = encoders.clone();
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
            // ★ BESIDE THE INPUT, which is what --work-outdir's own help text has always promised.
            // It used to default to the CURRENT DIRECTORY, so `spirit play --work film.mkv -- game`
            // left the job's output wherever you happened to be standing. (Found by the outside
            // review, 2026-10-11: the usage text and the code had disagreed the whole time.)
            let outdir: String = if !work_outdir.is_empty() {
                abspath(work_outdir)
            } else {
                let d = dirname(&abspath(work));
                if d.is_empty() {
                    env::current_dir()
                        .map(|p| p.to_string_lossy().to_string())
                        .unwrap_or_default()
                } else {
                    d
                }
            };
            // ★ A FAILED mkdir MUST REFUSE THE JOB (fix 2026-10-11). This used to be `let _ =`, which
            // swallowed the failure and launched anyway — so a job ran into a directory that did not
            // exist and turned up later as a MISSING output, instead of being refused up front the way
            // the other two builds refuse it. And no io::Error text in the message: each language
            // spells its own error differently, which is the one thing this branch cannot share.
            if std::fs::create_dir_all(&outdir).is_err() {
                println!("  work: REFUSED — cannot use {}", outdir);
                println!("          (the job needs a directory it can write to; check the path)");
                return 1;
            }
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
                    "  ★ SPLIT JOB: {} is {:.1}s — divided into {} slices across {} cards, one each.",
                    Path::new(work)
                        .file_name()
                        .map(|s| s.to_string_lossy().to_string())
                        .unwrap_or_else(|| work.to_string()),
                    total,
                    n,
                    n
                );
                for (i, (node, va)) in targets.iter().enumerate() {
                    let ss = i as f64 * span;
                    let dur = if i < n - 1 { span } else { total - ss };
                    match start_work(node, work, &outdir, &stem, i as i64, ss, dur, va) {
                        Ok((out, ch)) => {
                            println!(
                                "      {:<12} slice {}  [{:6.1}s .. {:6.1}s] -> {}",
                                node, i + 1, ss, ss + dur, base_of(&out)
                            );
                            procs.push(ch);
                            proc_nodes.push(node.clone());
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
                for (node, va) in &targets {
                    if let Ok((out, ch)) = start_work(node, work, &outdir, &stem, -1, 0.0, 0.0, va) {
                        job_outputs.push((node.clone(), out));
                        procs.push(ch);
                        proc_nodes.push(node.clone());
                    }
                }
                if !procs.is_empty() {
                    println!(
                        "  ★ JOB: transcoding {}.mp4 on {} spare card(s) — real input, real output. \
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
            for (node, va) in &encoders {
                if let Ok(ch) = start_test_load(node, va) {
                    procs.push(ch);
                    proc_nodes.push(node.clone());
                }
            }
            if !procs.is_empty() {
                println!("  cards: {} spare card(s) running a SYNTHETIC TEST LOAD", procs.len());
                println!("         (ffmpeg's own testsrc2 test pattern -> /dev/null). This proves the");
                println!("         cards can be DRIVEN; it is not a job and produces nothing.");
                println!("         For real work: spirit play --work <file> [--work-outdir DIR] -- <game>");
            }
        }
    }
    println!();
    println!("  launching…");
    println!();
    let _ = std::io::stdout().flush();

    // ★ Build the hidden game argv FIRST, then wrap it in gamescope. `unshare -r` maps our uid to
    // root, so the ROOT-OWNED /tmp/.X11-unix presents as the overflow uid inside the namespace and a
    // nested gamescope dies at xwayland/sockets.c:83 followed by "Failed to create Xwayland server".
    // gamescope MUST be the OUTER wrapper; only the game goes inside the namespace. Measured 2026-10-10.
    let mut inner = hidden_argv(&cmd, &main, db);
    if overlay && scale.is_none() {
        let (gs_state, gs_detail) = gamescope_state();
        if gs_state != "READY" || native_res(db).is_none() {
            println!("  overlay: REFUSED - gamescope not ready ({})", gs_detail);
            return 1;
        }
    }
    if scale.is_some() || overlay {
        let nat = native_res(db).unwrap_or((0, 0));
        let (rw, rh) = match scale {
            Some(s) => render_res(nat, s),
            None => nat,
        };
        let mut gs: Vec<String> = vec![
            // -w/-h = the GAME's render size; -W/-H = the OUTPUT (were swapped: --scale was a no-op)
            "gamescope".into(),
            "-w".into(),
            rw.to_string(),
            "-h".into(),
            rh.to_string(),
            "-W".into(),
            nat.0.to_string(),
            "-H".into(),
            nat.1.to_string(),
            "-f".into(),
        ];
        if scale.is_some() {
            gs.push("-F".into());
            gs.push("fsr".into());
            gs.push("--fsr-sharpness".into());
            gs.push("2".into());
        }
        if overlay {
            // THE PRESENT-COUNTER. mangoapp runs INSIDE gamescope as a transparent overlay, so its
            // FPS readout is the COMPOSITOR's present rate - the only number frame generation moves.
            // The game's own counter reports frames IT rendered, which frame gen does not change.
            gs.push("--mangoapp".into());
            println!("  overlay: mangoapp inside gamescope - on-screen FPS is the COMPOSITOR's present rate");
        }
        gs.push("--".into());
        gs.extend(inner);
        inner = gs;
    }
    let t_game = std::time::Instant::now();
    let rc = spawn_argv(&inner, &env_pins);
    // A TEST LOAD IS A LOAD AND DIES WITH THE GAME. A JOB IS NOT: killing a half-finished transcode
    // leaves a truncated file and calls it done. Jobs are WAITED FOR, then each output is checked
    // for size and duration ON DISK — the artifact, never the child's exit code.
    if !work.is_empty() {
        for p in procs.iter_mut() {
            let _ = p.wait();
        }
    } else {
        // ★ ASK EACH LOAD WHETHER IT IS STILL ALIVE, BEFORE KILLING IT (fix 2026-10-11, from the
        // outside review). THE TAIL used to say every spare was "alive for N s of the session",
        // using the GAME's elapsed time — and nothing ever polled the children. An ffmpeg that
        // died at second zero (a card handed the wrong VA driver, say) was therefore reported as
        // having run the whole session. A check that can only agree with you is a mirror, not a
        // check: this one never asked the question, so it could not fail.
        died_early.clear();
        for (p, node) in procs.iter_mut().zip(proc_nodes.iter()) {
            if let Ok(Some(st)) = p.try_wait() {
                died_early.push((node.clone(), st.code().unwrap_or(-1)));
            }
        }
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
            println!("  ── SPLIT CHECK {}", "─".repeat(55));
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
    let elapsed = f64::max(t_game.elapsed().as_secs_f64(), 1e-6);

    // ★ THE TAIL — what the OTHER cards actually did. A feature you cannot see is indistinguishable
    // from one that is broken, and the loads are silent by design (ffmpeg's output goes to /dev/null).
    // So say it plainly, per card, and say "alive for" rather than claiming work was done.
    if !procs.is_empty() || !job_outputs.is_empty() {
        // Python builds `names` as a dict keyed on the node basename, so a later spare wins a collision.
        let mut names: Vec<(String, String)> = Vec::new();
        for g in &spares {
            let r = g.get("render_node").and_then(|v| v.as_str()).unwrap_or("");
            names.push((base_of(r), gpu_name(g)));
        }
        let name_of = |node: &str| -> String {
            names
                .iter()
                .rev()
                .find(|(k, _)| k == node)
                .map(|(_, v)| v.clone())
                .unwrap_or_else(|| "?".to_string())
        };
        println!();
        println!("  ── THE TAIL — what the other cards did ───────────────────────────────");
        if !job_outputs.is_empty() {
            for (node, out) in &job_outputs {
                let ok = std::path::Path::new(out).is_file();
                println!("  {:<12} {:<22} REAL JOB  {}", node, name_of(node),
                         if ok { out.clone() } else { "FAILED — no file".to_string() });
            }
        } else {
            for g in &spares {
                let r = g.get("render_node").and_then(|v| v.as_str()).unwrap_or("");
                let node = base_of(r);
                if node.is_empty() { continue; }
                let nm = name_of(&node);
                if encoders.iter().any(|(n, _)| n == &node) {
                    match died_early.iter().find(|(n, _)| n == &node).map(|(_, rc)| *rc) {
                        None => println!("  {:<12} {:<22} SYNTHETIC TEST LOAD — ran for the whole session ({:.0} s),",
                                         node, nm, elapsed),
                        Some(rc) => {
                            // ★ Say what was SEEN, not what was hoped: this one was already gone before the
                            // game exited, so it did not run the session however long the game lasted.
                            println!("  {:<12} {:<22} SYNTHETIC TEST LOAD — EXITED EARLY (rc={}, before the game",
                                     node, nm, rc);
                            println!("  {:<12} {:<22}   exited): it did NOT run for the session.", "", "");
                        }
                    }
                    println!("  {:<12} {:<22} produced nothing (a job needs --work FILE)", "", "");
                } else {
                    let why = skipped
                        .iter()
                        .find(|(n2, _, _)| n2 == &node)
                        .map(|(_, _, w)| w.clone())
                        .unwrap_or_else(|| "not used".to_string());
                    println!("  {:<12} {:<22} SKIPPED — {}", node, nm, why);
                }
            }
        }
    }
    if let Some(ref fg_path) = fg_count_path {
        let n = read_counter(fg_path);
        println!();
        println!("  ── WITNESS ─────────────────────────────────────────────────────────");
        println!("  images presented to the compositor : {}", n);
        println!("  wall time                          : {:.1} s", elapsed);
        println!("  presented rate                     : {:.1} /s", n as f64 / elapsed);
        println!("  ★ This is the number frame generation moves, and the ONLY number it moves.");
        println!("    The game's own FPS counter cannot see it — that counts frames the GAME");
        println!("    rendered, which frame gen does not change. Expect the game's figure to be");
        println!("    LOWER with frame gen on: it is now paying for the interpolation.");
        println!("    Honest limit: an image handed to the compositor is not proof it was scanned");
        println!("    out, and no counter can tell you whether the interpolated frames look right.");
    }
    if rc == 0 {
        0
    } else {
        rc
    }
}

// ── arg parsing / main ────────────────────────────────────────────────────────────────────
// ★ `play` is hand-parsed (it has to pass an arbitrary game command through untouched), so
// neither an argparse parser nor `play --help` exists for it — the flag used to fall through to
// `rest` and be EXECUTED as the game. Printed here, byte-for-byte, so all three builds can share it.
const PLAY_USAGE: &str = r#"usage: spirit play [--no-offload] [--scale F] [--framegen N] [--overlay] [--work FILE [--work-outdir DIR] [--split]] [--spare NODE,...] -- <game command...>

Play a game on the strongest card, with every other card doing real work beside it.

options:
  -h, --help            show this help message and exit
  --no-offload          run the game alone; no other card does work
  --scale F             render at F of native and let gamescope upscale it (0.3-1.0)
  --framegen N          frame generation at Nx, via lsfg-vk (2, 3 or 4)
  --overlay             show MangoHud over the game, via mangoapp
  --work FILE           transcode FILE on the spare cards while you play (a real job)
  --work-outdir DIR     where a job's output goes (default: beside FILE)
  --split               divide ONE job across the spare cards instead of one job each
  --spare NODE,...      only use these render nodes, e.g. renderD130,renderD131

  A job is not stopped with the game. Every output is checked on disk afterwards: a
  half-finished transcode reported as done is the failure this tool exists not to make."#;

// ★ `ui` — the local page. ONE Python file, shared by all three builds rather than three
// reimplementations of an HTTP server; this build finds it and hands off. Same lookup order and the
// same failure line in every build, so `spirit ui` behaves identically whoever compiled what.
const UI_USAGE: &str = r#"usage: spirit ui [-h] [--port PORT] [--no-open] [--print-url]

Open a local page for this tool: your cards and what each one is for, whether
upscaling and frame generation are available here, and a box to launch your game.

It listens on 127.0.0.1 only, so nothing outside this machine can reach it.

options:
  -h, --help        show this help message and exit
  --port PORT       serve on this port instead of a free one
  --no-open         do not open a browser
  --print-url       print the URL and keep running"#;

/// Beside the running program, then up to four directories above it (a C++ binary in `cpp/` or a
/// Rust one in `rust/target/release/`), then where install.sh puts it. Mirrors spirit.py's `_ui_path`.
fn ui_path() -> Option<PathBuf> {
    let mut cands: Vec<PathBuf> = Vec::new();
    if let Ok(selfp) = std::fs::read_link("/proc/self/exe") {
        let mut cur = selfp.parent().map(|p| p.to_path_buf());
        for _ in 0..5 {
            match cur {
                Some(d) => {
                    cands.push(d.join("spirit_ui.py"));
                    cur = d.parent().map(|p| p.to_path_buf());
                }
                None => break,
            }
        }
    }
    if let Some(h) = env::var_os("HOME") {
        cands.push(PathBuf::from(h).join(".local/share/spirit/spirit_ui.py"));
    }
    cands.into_iter().find(|p| p.is_file())
}

fn ui(args: &[String]) -> i32 {
    if args.iter().any(|a| a == "-h" || a == "--help") {
        println!("{}", UI_USAGE);
        return 0;
    }
    match ui_path() {
        Some(p) => {
            let mut cmd = Command::new("python3");
            cmd.arg(p);
            for a in args {
                cmd.arg(a);
            }
            match cmd.status() {
                Ok(st) => st.code().unwrap_or(1),
                Err(_) => 1,
            }
        }
        None => {
            eprintln!(
                "spirit ui: spirit_ui.py was not found. Run install.sh, or put spirit_ui.py beside \
                 spirit.py."
            );
            1
        }
    }
}

fn print_help(cmd: Option<&str>) {
    match cmd {
        None => {
            println!("usage: spirit [-h] {{probe,roles,plan,doctor,origin,boost,fg}} ...");
            println!();
            println!("Spiritual Successor — put every GPU to work");
            println!("commands: probe · roles · plan · doctor · fg · boost · run · play · ui  (`run` and `play` take a command after `--`)");
            println!();
            println!("positional arguments:");
            println!("  {{probe,roles,plan,doctor,origin,boost,fg}}");
            println!();
            println!("options:");
            println!("  -h, --help            show this help message and exit");
        }
        Some("probe") => {
            println!("usage: spirit probe [-h] [--json]");
            println!();
            println!("options:");
            println!("  -h, --help  show this help message and exit");
            println!("  --json");
        }
        Some("roles") => {
            println!("usage: spirit roles [-h] [--profile PROFILE] [--json]");
            println!();
            println!("options:");
            println!("  -h, --help         show this help message and exit");
            println!("  --profile PROFILE");
            println!("  --json");
        }
        Some("plan") => {
            println!("usage: spirit plan [-h] [--workload WORKLOAD] [--vram-mib VRAM_MIB]");
            println!();
            println!("options:");
            println!("  -h, --help           show this help message and exit");
            println!("  --workload WORKLOAD");
            println!("  --vram-mib VRAM_MIB  VRAM the workload needs on its card");
        }
        Some("doctor") => {
            println!("usage: spirit doctor [-h]");
            println!();
            println!("options:");
            println!("  -h, --help  show this help message and exit");
        }
        Some("boost") => {
            println!("usage: spirit boost [-h]");
            println!();
            println!("options:");
            println!("  -h, --help  show this help message and exit");
        }
        Some("fg") => {
            println!("usage: spirit fg [-h] [--res RES] [--mode {{present-on-secondary,round-trip}}]");
            println!();
            println!("options:");
            println!("  -h, --help            show this help message and exit");
            println!("  --res RES             resolution@refresh, e.g. 1440p60 / 4k120 / 1080p144");
            println!("  --mode {{present-on-secondary,round-trip}}");
            println!("                        one-way (display on the frame-gen card) or round trip");
        }
        Some(_) => {}
    }
}

fn sub_usage(cmd: &str) -> &'static str {
    match cmd {
        "probe" => "usage: spirit probe [-h] [--json]",
        "roles" => "usage: spirit roles [-h] [--profile PROFILE] [--json]",
        "plan" => "usage: spirit plan [-h] [--workload WORKLOAD] [--vram-mib VRAM_MIB]",
        "doctor" => "usage: spirit doctor [-h]",
        "origin" => "usage: spirit origin [-h] [--check]",
        "fg" => "usage: spirit fg [-h] [--res RES] [--mode {present-on-secondary,round-trip}]",
        "boost" => "usage: spirit boost [-h]",
        _ => "usage: spirit [-h] {probe,roles,plan,doctor,origin,boost,fg} ...",
    }
}

fn err_top(msg: &str) -> i32 {
    eprintln!("usage: spirit [-h] {{probe,roles,plan,doctor,origin,boost,fg}} ...");
    eprintln!("spirit: error: {}", msg);
    2
}

fn err_sub(cmd: &str, msg: &str) -> i32 {
    eprintln!("{}", sub_usage(cmd));
    eprintln!("spirit {}: error: {}", cmd, msg);
    2
}

fn main() {
    let argv: Vec<String> = env::args().skip(1).collect();
    let code = dispatch(&argv);
    std::process::exit(code);
}

fn dispatch(argv: &[String]) -> i32 {
    // ★ THE MARK, ON EVERY RUN — printed first, exactly as the Python `main()` does before argparse
    // ever sees the argv. Skipped only for --json (a machine is reading) and for `origin` itself.
    if !argv.iter().any(|a| a == "--json") && !argv.iter().any(|a| a == "origin") {
        origin_cmd(false);
    }

    if argv.is_empty() {
        print_help(None);
        return 1;
    }
    if argv[0] == "-h" || argv[0] == "--help" {
        print_help(None);
        return 0;
    }

    if argv[0] == "origin" {
        let check = argv.iter().any(|a| a == "--check");
        return origin_cmd(check);
    }

    if argv[0] == "ui" {
        return ui(&argv[1..]);
    }

    // `run` — hand-parsed
    if argv[0] == "run" {
        let mut role: Option<String> = None;
        let mut card: Option<String> = None;
        let mut rest: Vec<String> = Vec::new();
        let mut i = 1;
        while i < argv.len() {
            if argv[i] == "--role" {
                role = argv.get(i + 1).cloned();
                i += 2;
            } else if argv[i] == "--card" {
                card = argv.get(i + 1).cloned();
                i += 2;
            } else if argv[i] == "--" {
                rest = argv[i + 1..].to_vec();
                break;
            } else {
                rest = argv[i..].to_vec();
                break;
            }
        }
        if role.is_none() || card.is_none() {
            eprintln!("usage: spirit run --role <role> --card <card> [-- <cmd>...]");
            return 1;
        }
        return run(&probe(), &role.unwrap(), &card.unwrap(), &rest);
    }

    // `play` — hand-parsed
    if argv[0] == "play" {
        let mut offload = true;
        let mut rest: Vec<String> = Vec::new();
        let mut scale: Option<f64> = None;
        let mut framegen: Option<i64> = None;
        let mut overlay = false;
        let mut work = String::new();
        let mut only_spares = String::new();
        let mut work_outdir = String::new();
        let mut split = false;
        let mut i = 1;
        while i < argv.len() {
            if argv[i] == "--no-offload" {
                offload = false;
                i += 1;
            } else if argv[i] == "--spare" {
                match argv.get(i + 1) {
                    Some(v) => { only_spares = v.clone(); }
                    None => {
                        eprintln!("spirit play: --spare needs render nodes, e.g. --spare renderD130,renderD131");
                        return 2;
                    }
                }
                i += 2;
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
            } else if argv[i] == "--scale" {
                match argv.get(i + 1).and_then(|s| py_float(s)) {
                    Some(x) => scale = Some(x),
                    None => {
                        eprintln!("spirit play: --scale needs a number, e.g. --scale 0.7");
                        return 2;
                    }
                }
                i += 2;
            } else if argv[i] == "--framegen" {
                match argv.get(i + 1).and_then(|s| py_int(s)) {
                    Some(x) => framegen = Some(x),
                    None => {
                        eprintln!("spirit play: --framegen needs 2, 3 or 4");
                        return 2;
                    }
                }
                i += 2;
            } else if argv[i] == "--overlay" {
                overlay = true;
                i += 1;
            } else if argv[i] == "-h" || argv[i] == "--help" {
                // ★ Without this, `play --help` fell through to `rest` and was executed as the game.
                println!("{}", PLAY_USAGE);
                return 0;
            } else if argv[i] == "--" {
                rest = argv[i + 1..].to_vec();
                break;
            } else {
                rest = argv[i..].to_vec();
                break;
            }
        }
        if let Some(s) = scale {
            if !(0.3 <= s && s <= 1.0) {
                eprintln!("spirit play: --scale must be between 0.3 and 1.0");
                return 2;
            }
        }
        if let Some(n) = framegen {
            if n != 2 && n != 3 && n != 4 {
                eprintln!("spirit play: --framegen must be 2, 3 or 4");
                return 2;
            }
        }
        return play(&probe(), &rest, offload, scale, framegen, overlay, &work, &work_outdir, split,
                    &only_spares);
    }

    let cmd = argv[0].as_str();
    if !matches!(cmd, "probe" | "roles" | "plan" | "doctor" | "origin" | "fg" | "boost") {
        return err_top(&format!(
            "argument cmd: invalid choice: '{}' (choose from 'probe', 'roles', 'plan', 'doctor', 'origin', 'boost', 'fg')",
            cmd
        ));
    }
    let mut json = false;
    let mut profile = "game".to_string();
    let mut workload = "1440p60-framegen".to_string();
    let mut vram_mib: Option<i64> = None;
    let mut res = "1440p60".to_string();
    let mut mode = "present-on-secondary".to_string();
    let mut unrecognized: Vec<String> = Vec::new();

    let mut j = 1;
    while j < argv.len() {
        let a = argv[j].as_str();
        // a flag that needs a value: error like argparse ("expected one argument") if absent
        macro_rules! value_of {
            ($flag:expr) => {{
                if j + 1 >= argv.len() {
                    return err_sub(cmd, &format!("argument {}: expected one argument", $flag));
                }
                j += 1;
                argv[j].clone()
            }};
        }
        match cmd {
            "probe" => match a {
                "--json" => json = true,
                "-h" | "--help" => {
                    print_help(Some(cmd));
                    return 0;
                }
                _ => unrecognized.push(a.to_string()),
            },
            "roles" => match a {
                "--json" => json = true,
                "--profile" => profile = value_of!("--profile"),
                "-h" | "--help" => {
                    print_help(Some(cmd));
                    return 0;
                }
                _ => unrecognized.push(a.to_string()),
            },
            "plan" => match a {
                "--workload" => workload = value_of!("--workload"),
                "--vram-mib" => {
                    let v = value_of!("--vram-mib");
                    match v.parse::<i64>() {
                        Ok(x) => vram_mib = Some(x),
                        Err(_) => {
                            return err_sub(
                                cmd,
                                &format!("argument --vram-mib: invalid int value: '{}'", v),
                            )
                        }
                    }
                }
                "-h" | "--help" => {
                    print_help(Some(cmd));
                    return 0;
                }
                _ => unrecognized.push(a.to_string()),
            },
            "doctor" => match a {
                "-h" | "--help" => {
                    print_help(Some(cmd));
                    return 0;
                }
                _ => unrecognized.push(a.to_string()),
            },
            "boost" => match a {
                "-h" | "--help" => {
                    print_help(Some(cmd));
                    return 0;
                }
                _ => unrecognized.push(a.to_string()),
            },
            "fg" => match a {
                "--res" => res = value_of!("--res"),
                "--mode" => {
                    let m = value_of!("--mode");
                    if m != "present-on-secondary" && m != "round-trip" {
                        return err_sub(
                            cmd,
                            &format!(
                                "argument --mode: invalid choice: '{}' (choose from 'present-on-secondary', 'round-trip')",
                                m
                            ),
                        );
                    }
                    mode = m;
                }
                "-h" | "--help" => {
                    print_help(Some(cmd));
                    return 0;
                }
                _ => unrecognized.push(a.to_string()),
            },
            _ => unreachable!(),
        }
        j += 1;
    }

    if !unrecognized.is_empty() {
        return err_top(&format!("unrecognized arguments: {}", unrecognized.join(" ")));
    }

    let db = probe();
    match cmd {
        "fg" => fg(&db, &res, &mode),
        "doctor" => doctor(&db, false),
        "boost" => boost(&db),
        "plan" => plan(&db, &workload, vram_mib),
        "roles" => {
            if json {
                let rp = assign_roles(&db, &profile);
                println!("{}", json_dump(&Json::Arr(rp)));
            } else {
                print_roles(&db, &profile);
            }
            0
        }
        "probe" => {
            if json {
                println!("{}", json_dump(&db));
            } else {
                print_table(&db);
            }
            0
        }
        _ => {
            print_help(None);
            1
        }
    }
}
