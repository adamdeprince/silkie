// SPDX-License-Identifier: GPL-3.0-only
// Initial portable browser backend. The native CPython/GLFW screen and parser
// remain the desktop implementation. Unicode widths share Silkie's own tables.
use std::fmt::Write;
use std::sync::Arc;
include!(concat!(env!("OUT_DIR"), "/widths.rs"));
const FG: u32 = 0xf7f6f1;
// Event types include press, repeat and release. The guest launcher completes
// an input handoff before returning a foreground application's tty to Bash.
const SUPPORTED_KEYBOARD_FLAGS: u32 = 1 | 2 | 8;
const BG: u32 = 0x11130f;
const ANSI: [u32; 16] = [
    0x11130f, 0xe06c75, 0xa6e22e, 0xe5c07b, 0x61afef, 0xc678dd, 0x41c4d1, 0xc5c8be, 0x62645c,
    0xff8a80, 0xb8ef55, 0xffd68a, 0x8bc5ff, 0xe2a5ff, 0x78e6ed, 0xf7f6f1,
];
fn width(c: char) -> i8 {
    let ch = c as usize;
    match WIDTHS[WIDTH_T2[((WIDTH_T1[ch >> 8] as usize) << 8) + (ch & 255)] as usize] {
        0 => 0,
        2 => 2,
        -1 => -1,
        _ => 1,
    }
}
fn quote(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c < ' ' => {
                write!(out, "\\u{:04x}", c as u32).unwrap();
            }
            c => out.push(c),
        }
    }
    out.push('"');
}
#[derive(Clone, Debug)]
struct ColorStyle {
    fg_brightness: u32,
    bg_brightness: u32,
    fg_gamut: u32,
    bg_gamut: u32,
    bg_alpha: u32,
}
impl Default for ColorStyle {
    fn default() -> Self {
        Self {
            fg_brightness: 100,
            bg_brightness: 100,
            fg_gamut: 0,
            bg_gamut: 0,
            bg_alpha: 100,
        }
    }
}
#[derive(Clone, Debug)]
struct Cell {
    text: String,
    fg: u32,
    bg: u32,
    attr: u32,
    color: ColorStyle,
    link: Option<Arc<str>>,
}
impl Cell {
    fn blank(fg: u32, bg: u32, attr: u32) -> Self {
        Self {
            text: " ".into(),
            fg,
            bg,
            attr,
            color: ColorStyle::default(),
            link: None,
        }
    }
}
#[derive(Clone)]
struct Screen {
    lines: Vec<Vec<Cell>>,
    x: usize,
    y: usize,
    saved: (usize, usize),
    top: usize,
    bottom: usize,
    wrap: bool,
}
impl Screen {
    fn new(cols: usize, rows: usize) -> Self {
        Self {
            lines: vec![vec![Cell::blank(FG, BG, 0); cols]; rows],
            x: 0,
            y: 0,
            saved: (0, 0),
            top: 0,
            bottom: rows - 1,
            wrap: false,
        }
    }
}
#[derive(Clone, Copy, PartialEq)]
enum State {
    Ground,
    Escape,
    Csi,
    Osc,
    Apc,
    Dcs,
    Ignore,
    Charset,
}
pub struct Terminal {
    cols: usize,
    rows: usize,
    screen: Screen,
    primary: Option<Screen>,
    history: Vec<Vec<Cell>>,
    fg: u32,
    bg: u32,
    attr: u32,
    color: ColorStyle,
    link: Option<Arc<str>>,
    state: State,
    sequence: String,
    escape_in_string: bool,
    utf8: Vec<u8>,
    input: Vec<u8>,
    output: String,
    events: Vec<String>,
    modes: u32,
    auto_wrap: bool,
    insert: bool,
    newline_mode: bool,
    default_newline_mode: bool,
    origin: bool,
    cell_width: u32,
    cell_height: u32,
    kitty_flags: u32,
    kitty_stack: Vec<u32>,
}
impl Terminal {
    fn new(cols: usize, rows: usize) -> Self {
        Self {
            cols,
            rows,
            screen: Screen::new(cols, rows),
            primary: None,
            history: vec![],
            fg: FG,
            bg: BG,
            attr: 0,
            color: ColorStyle::default(),
            link: None,
            state: State::Ground,
            sequence: String::new(),
            escape_in_string: false,
            utf8: vec![],
            input: vec![],
            output: String::new(),
            events: vec![],
            modes: 128,
            auto_wrap: true,
            insert: false,
            newline_mode: false,
            default_newline_mode: false,
            origin: false,
            cell_width: 9,
            cell_height: 18,
            kitty_flags: 0,
            kitty_stack: Vec::new(),
        }
    }
    fn blank(&self) -> Cell {
        let mut cell = Cell::blank(self.fg, self.bg, self.attr);
        cell.color = self.color.clone();
        cell
    }
    fn event(&mut self, kind: &str, payload: &str) {
        let mut e = String::from("[");
        quote(&mut e, kind);
        e.push(',');
        quote(&mut e, payload);
        write!(e, ",{},{}]", self.screen.x, self.screen.y).unwrap();
        self.events.push(e);
    }
    fn reply(&mut self, s: &str) {
        self.event("reply", s);
    }
    fn reset(&mut self) {
        self.fg = FG;
        self.bg = BG;
        self.attr = 0;
        self.color = ColorStyle::default();
        self.link = None;
        self.screen = Screen::new(self.cols, self.rows);
        self.primary = None;
        self.modes = 128;
        self.auto_wrap = true;
        self.insert = false;
        self.newline_mode = self.default_newline_mode;
        self.origin = false;
        self.kitty_flags = 0;
        self.kitty_stack.clear();
        self.event("keyboard-scope", "");
        self.event("screen-scope", "primary");
        self.event("reset-graphics", "");
        self.event("reset-input", "");
    }
    fn scroll(&mut self, up: bool, count: usize) {
        let top = self.screen.top;
        let bottom = self.screen.bottom;
        let count = count.min(bottom - top + 1);
        for _ in 0..count {
            if up {
                let line = self.screen.lines.remove(top);
                if top == 0 && bottom == self.rows - 1 && self.primary.is_none() {
                    self.history.push(line);
                }
                self.screen
                    .lines
                    .insert(bottom, vec![self.blank(); self.cols]);
            } else {
                self.screen.lines.remove(bottom);
                self.screen.lines.insert(top, vec![self.blank(); self.cols]);
            }
        }
        self.event(
            "scroll",
            &format!("{}", if up { count as i64 } else { -(count as i64) }),
        );
    }
    fn lf(&mut self) {
        self.screen.wrap = false;
        if self.screen.y == self.screen.bottom {
            self.scroll(true, 1)
        } else {
            self.screen.y = (self.screen.y + 1).min(self.rows - 1)
        }
    }
    fn put(&mut self, c: char) {
        let w = width(c);
        if w == 0 || c == '\u{200d}' {
            let mut x = if self.screen.wrap {
                self.screen.x
            } else {
                self.screen.x.saturating_sub(1)
            };
            if self.screen.lines[self.screen.y][x].attr & 16 != 0 {
                x = x.saturating_sub(1);
            }
            self.screen.lines[self.screen.y][x].text.push(c);
            return;
        }
        if w < 0 {
            return;
        }
        let w = (w as usize).min(2);
        if self.screen.wrap || (w == 2 && self.screen.x + 1 >= self.cols) {
            if self.auto_wrap {
                self.screen.x = 0;
                self.lf();
            }
            self.screen.wrap = false;
        }
        let x = self.screen.x;
        let y = self.screen.y;
        if self.insert {
            let blank = self.blank();
            for _ in 0..w {
                self.screen.lines[y].insert(x, blank.clone());
                self.screen.lines[y].pop();
            }
        }
        // A wide glyph and its trailing cell form one terminal cell span.
        if self.screen.lines[y][x].attr & 16 != 0 && x > 0 {
            self.screen.lines[y][x - 1] = self.blank();
        }
        if x + 1 < self.cols && self.screen.lines[y][x + 1].attr & 16 != 0 {
            self.screen.lines[y][x + 1] = self.blank();
        }
        self.screen.lines[y][x] = Cell {
            text: c.to_string(),
            fg: self.fg,
            bg: self.bg,
            attr: self.attr,
            color: self.color.clone(),
            link: self.link.clone(),
        };
        if w == 2 && x + 1 < self.cols {
            self.screen.lines[y][x + 1] = Cell {
                text: String::new(),
                fg: self.fg,
                bg: self.bg,
                attr: self.attr | 16,
                color: self.color.clone(),
                link: self.link.clone(),
            };
        }
        if x + w >= self.cols {
            self.screen.x = self.cols - 1;
            self.screen.wrap = self.auto_wrap;
        } else {
            self.screen.x = x + w;
        }
    }
    fn control(&mut self, b: u8) {
        match b {
            b'\r' => {
                self.screen.x = 0;
                self.screen.wrap = false
            }
            b'\n' | 0x0b | 0x0c => {
                if self.newline_mode {
                    self.screen.x = 0;
                }
                self.lf();
            }
            8 => {
                self.screen.x = self.screen.x.saturating_sub(1);
                self.screen.wrap = false
            }
            9 => {
                self.screen.x = ((self.screen.x / 8 + 1) * 8).min(self.cols - 1);
                self.screen.wrap = false
            }
            _ => {}
        }
    }
    fn feed(&mut self, data: &[u8]) {
        for &b in data {
            match self.state {
                State::Ground => {
                    if b == 0x1b {
                        if !self.utf8.is_empty() {
                            self.put('\u{fffd}');
                            self.utf8.clear();
                        }
                        self.state = State::Escape;
                    } else if b < 32 || b == 127 {
                        self.control(b)
                    } else if b < 128 && self.utf8.is_empty() {
                        self.put(b as char)
                    } else {
                        self.utf8.push(b);
                        match std::str::from_utf8(&self.utf8) {
                            Ok(s) => {
                                let chars: Vec<char> = s.chars().collect();
                                self.utf8.clear();
                                for c in chars {
                                    self.put(c)
                                }
                            }
                            Err(e) if e.error_len().is_some() => {
                                self.utf8.clear();
                                self.put('\u{fffd}');
                                if b < 128 {
                                    self.put(b as char)
                                }
                            }
                            _ => {}
                        }
                    }
                }
                State::Escape => {
                    self.sequence.clear();
                    self.state = match b {
                        b'[' => State::Csi,
                        b']' => State::Osc,
                        b'_' => State::Apc,
                        b'P' => State::Dcs,
                        b'^' | b'X' => State::Ignore,
                        b'(' | b')' | b'*' | b'+' => State::Charset,
                        b'7' => {
                            self.screen.saved = (self.screen.x, self.screen.y);
                            State::Ground
                        }
                        b'8' => {
                            (self.screen.x, self.screen.y) = self.screen.saved;
                            self.screen.wrap = false;
                            State::Ground
                        }
                        b'D' => {
                            self.lf();
                            State::Ground
                        }
                        b'E' => {
                            self.screen.x = 0;
                            self.lf();
                            State::Ground
                        }
                        b'M' => {
                            if self.screen.y == self.screen.top {
                                self.scroll(false, 1)
                            } else {
                                self.screen.y = self.screen.y.saturating_sub(1)
                            };
                            State::Ground
                        }
                        b'c' => {
                            self.reset();
                            State::Ground
                        }
                        _ => State::Ground,
                    };
                }
                State::Charset => self.state = State::Ground,
                State::Csi => {
                    if b == 0x1b {
                        self.state = State::Escape;
                        continue;
                    }
                    if (0x40..=0x7e).contains(&b) {
                        let seq = std::mem::take(&mut self.sequence);
                        self.state = State::Ground;
                        self.csi(&seq, b);
                    } else if b >= 32 {
                        self.sequence.push(b as char)
                    }
                }
                _ => {
                    if self.escape_in_string {
                        self.escape_in_string = false;
                        if b == b'\\' {
                            self.end_string();
                            continue;
                        }
                        self.sequence.push('\x1b');
                    }
                    if b == 0x1b {
                        self.escape_in_string = true;
                    } else if b == 7 && self.state == State::Osc {
                        self.end_string();
                    } else {
                        self.sequence.push(b as char);
                    }
                }
            }
        }
    }
    fn end_string(&mut self) {
        let seq = std::mem::take(&mut self.sequence);
        let state = self.state;
        self.state = State::Ground;
        self.escape_in_string = false;
        match state {
            State::Apc => self.event("apc", &seq),
            State::Osc => {
                if let Some(rest) = seq.strip_prefix("8;") {
                    if let Some((_, uri)) = rest.split_once(';') {
                        let bytes: Vec<u8> = uri.chars().map(|c| c as u8).collect();
                        self.link = if bytes.is_empty() {
                            None
                        } else {
                            Some(Arc::from(String::from_utf8_lossy(&bytes).as_ref()))
                        };
                    }
                } else {
                    self.event("osc", &seq);
                }
            }
            State::Dcs => {
                if let Some(query) = seq.strip_prefix("$q") {
                    self.reply(&format!("\x1bP0$r{query}\x1b\\"));
                }
            }
            _ => {}
        }
    }
    fn csi(&mut self, seq: &str, final_byte: u8) {
        let private = seq.starts_with('?');
        let leader = seq.chars().next().filter(|c| "?><=!".contains(*c));
        let body = if leader.is_some() { &seq[1..] } else { seq };
        // SGR has colon subparameters, unlike the cursor/mode parameters below.
        // Parse its entire parameter list before changing any attributes.
        if final_byte == b'm' {
            self.sgr(body);
            return;
        }
        let mut nums = Vec::new();
        for part in body.split(';') {
            let number = part.trim_end_matches([' ', '$']);
            if number.is_empty() {
                nums.push(0);
            } else {
                // Overflow is invalid, not an omitted/default parameter.
                let Ok(value) = number.parse::<usize>() else {
                    return;
                };
                nums.push(value);
            }
        }
        let p = |i: usize, default: usize| -> usize {
            nums.get(i).copied().filter(|n| *n > 0).unwrap_or(default)
        };
        let n = p(0, 1);
        let x = self.screen.x;
        let y = self.screen.y;
        let blank = self.blank();
        match final_byte {
            b'A' => self.screen.y = y.saturating_sub(n),
            b'B' | b'e' => self.screen.y = y.saturating_add(n).min(self.rows - 1),
            b'C' | b'a' => self.screen.x = x.saturating_add(n).min(self.cols - 1),
            b'D' => self.screen.x = x.saturating_sub(n),
            b'E' => {
                self.screen.x = 0;
                self.screen.y = y.saturating_add(n).min(self.rows - 1)
            }
            b'F' => {
                self.screen.x = 0;
                self.screen.y = y.saturating_sub(n)
            }
            b'G' | b'`' => self.screen.x = n.saturating_sub(1).min(self.cols - 1),
            b'd' => self.screen.y = n.saturating_sub(1).min(self.rows - 1),
            b'H' | b'f' => {
                self.screen.y = (p(0, 1) - 1)
                    .saturating_add(if self.origin { self.screen.top } else { 0 })
                    .min(if self.origin {
                        self.screen.bottom
                    } else {
                        self.rows - 1
                    });
                self.screen.x = (p(1, 1) - 1).min(self.cols - 1)
            }
            b'J' => {
                let mode = nums[0];
                for row in 0..self.rows {
                    for col in 0..self.cols {
                        if mode == 2
                            || mode == 3
                            || (mode == 0 && (row > y || (row == y && col >= x)))
                            || (mode == 1 && (row < y || (row == y && col <= x)))
                        {
                            self.screen.lines[row][col] = blank.clone();
                        }
                    }
                }
                if mode == 3 {
                    self.history.clear();
                }
                if mode == 2 || mode == 3 {
                    self.event("clear-graphics", "");
                }
            }
            b'K' => {
                let range = match nums[0] {
                    1 => 0..x + 1,
                    2 => 0..self.cols,
                    _ => x..self.cols,
                };
                for col in range {
                    self.screen.lines[y][col] = blank.clone();
                }
            }
            b'X' => {
                for col in x..x.saturating_add(n).min(self.cols) {
                    self.screen.lines[y][col] = blank.clone()
                }
            }
            b'@' => {
                for _ in 0..n.min(self.cols - x) {
                    self.screen.lines[y].insert(x, blank.clone());
                    self.screen.lines[y].pop();
                }
            }
            b'P' => {
                for _ in 0..n.min(self.cols - x) {
                    self.screen.lines[y].remove(x);
                    self.screen.lines[y].push(blank.clone());
                }
            }
            b'L' if y >= self.screen.top && y <= self.screen.bottom => {
                for _ in 0..n.min(self.screen.bottom - y + 1) {
                    self.screen.lines.remove(self.screen.bottom);
                    self.screen.lines.insert(y, vec![blank.clone(); self.cols]);
                }
            }
            b'M' if y >= self.screen.top && y <= self.screen.bottom => {
                for _ in 0..n.min(self.screen.bottom - y + 1) {
                    self.screen.lines.remove(y);
                    self.screen
                        .lines
                        .insert(self.screen.bottom, vec![blank.clone(); self.cols]);
                }
            }
            b'S' => self.scroll(true, n),
            b'T' => self.scroll(false, n),
            b'r' if !private => {
                let top = p(0, 1) - 1;
                let bottom = p(1, self.rows) - 1;
                if top < bottom && bottom < self.rows {
                    self.screen.top = top;
                    self.screen.bottom = bottom;
                    self.screen.x = 0;
                    self.screen.y = if self.origin { top } else { 0 }
                }
            }
            b's' => self.screen.saved = (x, y),
            b'u' if leader.is_none() => {
                (self.screen.x, self.screen.y) = self.screen.saved;
            }
            b'u' if leader == Some('?') => self.reply(&format!("\x1b[?{}u", self.kitty_flags)),
            b'u' if leader == Some('>') => {
                self.kitty_stack.push(self.kitty_flags);
                self.kitty_flags = (nums[0] & SUPPORTED_KEYBOARD_FLAGS as usize) as u32;
                self.event("keyboard-scope", "");
            }
            b'u' if leader == Some('=') => {
                let flags = (nums[0] & SUPPORTED_KEYBOARD_FLAGS as usize) as u32;
                let previous = self.kitty_flags;
                self.kitty_flags = match p(1, 1) {
                    2 => self.kitty_flags | flags,
                    3 => self.kitty_flags & !flags,
                    _ => flags,
                };
                if self.kitty_flags != previous {
                    self.event("keyboard-scope", "");
                }
            }
            b'u' if leader == Some('<') => {
                for _ in 0..n.min(self.kitty_stack.len().saturating_add(1)) {
                    self.kitty_flags = self.kitty_stack.pop().unwrap_or(0);
                }
                self.event("keyboard-scope", "");
            }
            b'm' => self.sgr(body),
            b'h' | b'l' => {
                let set = final_byte == b'h';
                for mode in nums {
                    if private {
                        self.mode(mode, set)
                    } else if mode == 4 {
                        self.insert = set
                    } else if mode == 20 {
                        self.newline_mode = set
                    }
                }
            }
            b'n' => {
                if nums[0] == 5 {
                    self.reply("\x1b[0n")
                } else if nums[0] == 6 {
                    self.reply(&format!("\x1b[{};{}R", y + 1, x + 1))
                }
            }
            b'c' => {
                if leader == Some('>') {
                    self.reply("\x1b[>1;1;0c")
                } else {
                    self.reply("\x1b[?62;4;22c")
                }
            }
            b't' => match nums[0] {
                14 => {
                    if let (Some(height), Some(width)) = (
                        (self.rows as u32).checked_mul(self.cell_height),
                        (self.cols as u32).checked_mul(self.cell_width),
                    ) {
                        self.reply(&format!("\x1b[4;{height};{width}t"));
                    }
                }
                16 => self.reply(&format!("\x1b[6;{};{}t", self.cell_height, self.cell_width)),
                18 => self.reply(&format!("\x1b[8;{};{}t", self.rows, self.cols)),
                _ => {}
            },
            b'p' if seq.ends_with('$') && private => {
                let mode = nums[0];
                let value = if matches!(
                    mode,
                    1 | 6
                        | 7
                        | 25
                        | 1000
                        | 1002
                        | 1003
                        | 1004
                        | 1006
                        | 1016
                        | 1047
                        | 1049
                        | 2004
                        | 2026
                ) {
                    if self.mode_value(mode) { 1 } else { 2 }
                } else {
                    0
                };
                self.reply(&format!("\x1b[?{mode};{value}$y"));
            }
            _ => {}
        }
        if !matches!(
            final_byte,
            b'm' | b'h' | b'l' | b'n' | b'c' | b't' | b'p' | b'u'
        ) {
            self.screen.wrap = false;
        }
    }
    fn mode_value(&self, mode: usize) -> bool {
        match mode {
            1 => self.modes & 1 != 0,
            6 => self.origin,
            7 => self.auto_wrap,
            25 => self.modes & 128 != 0,
            1000 => self.modes & 4 != 0,
            1002 => self.modes & 8 != 0,
            1003 => self.modes & 16 != 0,
            1006 => self.modes & 64 != 0,
            1016 => self.modes & 32 != 0,
            1004 => self.modes & 1024 != 0,
            1047 | 1049 => self.primary.is_some(),
            2004 => self.modes & 2 != 0,
            2026 => self.modes & 512 != 0,
            _ => false,
        }
    }
    fn mode(&mut self, mode: usize, set: bool) {
        let bit = match mode {
            1 => 1,
            25 => 128,
            1000 => 4,
            1002 => 8,
            1003 => 16,
            1006 => 64,
            1016 => 32,
            1004 => 1024,
            2004 => 2,
            2026 => 512,
            _ => 0,
        };
        if set {
            self.modes |= bit
        } else {
            self.modes &= !bit
        }
        match mode {
            6 => {
                self.origin = set;
                self.screen.x = 0;
                self.screen.y = if set { self.screen.top } else { 0 }
            }
            7 => self.auto_wrap = set,
            1047 | 1049 => {
                if set && self.primary.is_none() {
                    self.primary = Some(std::mem::replace(
                        &mut self.screen,
                        Screen::new(self.cols, self.rows),
                    ));
                    self.event("screen-scope", "alternate");
                    self.event("reset-graphics", "");
                } else if !set {
                    if let Some(screen) = self.primary.take() {
                        self.screen = screen;
                        self.event("screen-scope", "primary");
                        self.event("reset-graphics", "");
                    }
                }
            }
            1048 => {
                if set {
                    self.screen.saved = (self.screen.x, self.screen.y)
                } else {
                    (self.screen.x, self.screen.y) = self.screen.saved;
                }
            }
            _ => {}
        }
    }
    fn sgr(&mut self, body: &str) {
        // Preserve colon groups: their gamut/brightness/alpha values must not
        // become unrelated SGR attributes. Validate before mutating any state.
        let mut groups = Vec::new();
        for group in body.split(';') {
            let mut values = Vec::new();
            for number in group.split(':') {
                if number.is_empty() {
                    values.push(0);
                } else {
                    let Ok(value) = number.parse::<u32>() else {
                        return;
                    };
                    values.push(value);
                }
            }
            groups.push(values);
        }
        let brightness = |percent: u32| -> u32 {
            100 + ((percent.min(1675).saturating_sub(100) + 12) / 25) * 25
        };
        let mut i = 0;
        while i < groups.len() {
            let group = &groups[i];
            let n = group[0];
            if group.len() > 1 {
                match n {
                    56 => self.color.fg_brightness = brightness(group[1]),
                    57 => self.color.bg_brightness = brightness(group[1]),
                    70 => self.color.bg_alpha = group[1].min(100) / 2 * 2,
                    38 | 48 | 58 => {
                        let color = if group.get(1) == Some(&5) {
                            group.get(2).map(|c| (indexed(*c), 0))
                        } else if group.get(1) == Some(&2) {
                            let (start, gamut) = if group.len() >= 6 {
                                (3, if group[2] <= 2 { group[2] } else { 0 })
                            } else {
                                (2, 0)
                            };
                            match (group.get(start), group.get(start + 1), group.get(start + 2)) {
                                (Some(r), Some(g), Some(b)) => Some((
                                    (r.min(&255) << 16) | (g.min(&255) << 8) | b.min(&255),
                                    gamut,
                                )),
                                _ => None,
                            }
                        } else {
                            None
                        };
                        if let Some((rgb, gamut)) = color {
                            self.set_color(n, rgb, gamut);
                        }
                    }
                    _ => {}
                }
                i += 1;
                continue;
            }
            match n {
                0 => {
                    self.fg = FG;
                    self.bg = BG;
                    self.attr = 0;
                    self.color = ColorStyle::default();
                }
                1 => self.attr |= 1,
                2 => self.attr |= 64,
                3 => self.attr |= 2,
                4 => self.attr |= 4,
                7 => self.attr |= 8,
                22 => self.attr &= !(1 | 64),
                23 => self.attr &= !2,
                24 => self.attr &= !4,
                27 => self.attr &= !8,
                30..=37 => self.set_color(38, ANSI[(n - 30) as usize], 0),
                40..=47 => self.set_color(48, ANSI[(n - 40) as usize], 0),
                90..=97 => self.set_color(38, ANSI[(n - 90 + 8) as usize], 0),
                100..=107 => self.set_color(48, ANSI[(n - 100 + 8) as usize], 0),
                39 => self.set_color(38, FG, 0),
                49 => {
                    self.bg = BG;
                    self.color.bg_gamut = 0;
                    self.attr &= !32;
                }
                56 => self.color.fg_brightness = 100,
                57 => self.color.bg_brightness = 100,
                70 => self.color.bg_alpha = 100,
                38 | 48 | 58 => {
                    let value = |offset: usize| {
                        groups
                            .get(i + offset)
                            .and_then(|g| if g.len() == 1 { Some(g[0]) } else { None })
                    };
                    let color = if value(1) == Some(5) {
                        value(2).map(|c| (indexed(c), 2))
                    } else if value(1) == Some(2) {
                        match (value(2), value(3), value(4)) {
                            (Some(r), Some(g), Some(b)) => {
                                Some(((r.min(255) << 16) | (g.min(255) << 8) | b.min(255), 4))
                            }
                            _ => None,
                        }
                    } else {
                        None
                    };
                    if let Some((rgb, consumed)) = color {
                        self.set_color(n, rgb, 0);
                        i += consumed;
                    }
                }
                _ => {}
            }
            i += 1;
        }
    }
    fn set_color(&mut self, which: u32, rgb: u32, gamut: u32) {
        if which == 38 {
            self.fg = rgb;
            self.color.fg_gamut = gamut;
        } else if which == 48 {
            self.bg = rgb;
            self.color.bg_gamut = gamut;
            self.attr |= 32;
        }
    }
    fn resize(&mut self, cols: usize, rows: usize) {
        fn size(screen: &mut Screen, cols: usize, rows: usize) {
            for line in &mut screen.lines {
                line.resize(cols, Cell::blank(FG, BG, 0));
            }
            screen
                .lines
                .resize_with(rows, || vec![Cell::blank(FG, BG, 0); cols]);
            screen.x = screen.x.min(cols - 1);
            screen.y = screen.y.min(rows - 1);
            screen.saved.0 = screen.saved.0.min(cols - 1);
            screen.saved.1 = screen.saved.1.min(rows - 1);
            screen.top = 0;
            screen.bottom = rows - 1;
            screen.wrap = false;
        }
        size(&mut self.screen, cols, rows);
        if let Some(screen) = &mut self.primary {
            size(screen, cols, rows)
        }
        self.cols = cols;
        self.rows = rows;
    }
    fn snapshot(&mut self, offset: usize) {
        self.output.clear();
        let offset = if self.primary.is_none() {
            offset.min(self.history.len())
        } else {
            0
        };
        write!(
            self.output,
            "[{},{},{},{},{},{},[",
            self.cols,
            self.rows,
            self.screen.x,
            self.screen.y,
            self.modes & 128 != 0,
            self.modes
        )
        .unwrap();
        let start = self.history.len() - offset;
        for row in 0..self.rows {
            if row > 0 {
                self.output.push(',')
            }
            self.output.push('[');
            let idx = start + row;
            let line = if idx < self.history.len() {
                &self.history[idx]
            } else {
                &self.screen.lines[idx - self.history.len()]
            };
            for col in 0..self.cols {
                if col > 0 {
                    self.output.push(',')
                }
                if let Some(cell) = line.get(col) {
                    self.output.push('[');
                    quote(&mut self.output, &cell.text);
                    write!(
                        self.output,
                        ",{},{},{},{},{},{},{},{},",
                        cell.fg,
                        cell.bg,
                        cell.attr,
                        cell.color.fg_brightness,
                        cell.color.bg_brightness,
                        cell.color.fg_gamut,
                        cell.color.bg_gamut,
                        cell.color.bg_alpha
                    )
                    .unwrap();
                    if let Some(link) = &cell.link {
                        quote(&mut self.output, link);
                    } else {
                        self.output.push_str("null");
                    }
                    self.output.push(']');
                } else {
                    self.output
                        .push_str("[\" \",16250609,1118991,0,100,100,0,0,100,null]")
                }
            }
            self.output.push(']');
        }
        write!(self.output, "],{}]", self.history.len()).unwrap();
    }
}
fn indexed(c: u32) -> u32 {
    if c < 16 {
        return ANSI[c as usize];
    }
    if c >= 232 {
        let v = (8 + (c.min(255) - 232) * 10).min(255);
        return v * 0x010101;
    }
    let n = c - 16;
    let channel = |n: u32| if n == 0 { 0 } else { 55 + n * 40 };
    (channel(n / 36) << 16) | (channel((n / 6) % 6) << 8) | channel(n % 6)
}
fn valid_size(cols: u32, rows: u32) -> bool {
    cols > 0
        && rows > 0
        && (cols as usize)
            .checked_mul(rows as usize)
            .and_then(|n| n.checked_mul(std::mem::size_of::<Cell>()))
            // Rust allocations must fit Layout's isize::MAX addressability bound.
            .filter(|n| *n <= isize::MAX as usize)
            .is_some()
}
// Handles and pointers are private to one WASM instance. Callers must not retain
// views across calls: Rust allocation can grow WebAssembly linear memory.
#[unsafe(no_mangle)]
pub extern "C" fn terminal_new(cols: u32, rows: u32) -> *mut Terminal {
    if !valid_size(cols, rows) {
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new(Terminal::new(cols as usize, rows as usize)))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_free(ptr: *mut Terminal) {
    if !ptr.is_null() {
        drop(unsafe { Box::from_raw(ptr) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_input(ptr: *mut Terminal, len: usize) -> *mut u8 {
    let t = unsafe { &mut *ptr };
    if t.input
        .try_reserve(len.saturating_sub(t.input.len()))
        .is_err()
    {
        return std::ptr::null_mut();
    }
    t.input.resize(len, 0);
    t.input.as_mut_ptr()
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_feed(ptr: *mut Terminal) -> i32 {
    let t = unsafe { &mut *ptr };
    let data = std::mem::take(&mut t.input);
    t.feed(&data);
    t.input = data;
    0
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_resize(ptr: *mut Terminal, cols: u32, rows: u32) -> i32 {
    if !valid_size(cols, rows) {
        return -1;
    }
    unsafe { &mut *ptr }.resize(cols as usize, rows as usize);
    0
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_snapshot(ptr: *mut Terminal, offset: usize) -> *const u8 {
    let t = unsafe { &mut *ptr };
    t.snapshot(offset);
    t.output.as_ptr()
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_events(ptr: *mut Terminal) -> *const u8 {
    let t = unsafe { &mut *ptr };
    t.output.clear();
    t.output.push('[');
    for (i, e) in t.events.drain(..).enumerate() {
        if i > 0 {
            t.output.push(',')
        }
        t.output.push_str(&e)
    }
    t.output.push(']');
    t.output.as_ptr()
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_output_len(ptr: *mut Terminal) -> usize {
    unsafe { &*ptr }.output.len()
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_modes(ptr: *mut Terminal) -> u32 {
    unsafe { &*ptr }.modes
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_cell_size(ptr: *mut Terminal, w: u32, h: u32) {
    let t = unsafe { &mut *ptr };
    t.cell_width = w;
    t.cell_height = h;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_newline_default(ptr: *mut Terminal, enabled: u32) {
    let t = unsafe { &mut *ptr };
    t.default_newline_mode = enabled != 0;
    t.newline_mode = t.default_newline_mode;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_kitty_flags(ptr: *mut Terminal) -> u32 {
    unsafe { &*ptr }.kitty_flags
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn terminal_keyboard_reset(ptr: *mut Terminal) {
    let t = unsafe { &mut *ptr };
    t.kitty_flags = 0;
    t.kitty_stack.clear();
}

#[cfg(test)]
mod tests {
    use super::*;
    fn text(t: &Terminal, row: usize) -> String {
        t.screen.lines[row]
            .iter()
            .map(|c| c.text.as_str())
            .collect()
    }
    #[test]
    fn shell_control_sequences() {
        let mut t = Terminal::new(12, 3);
        t.feed(b"hello\rworld\x1b[K\r\n\x1b[31mred\x1b[0m");
        assert_eq!(text(&t, 0), "world       ");
        assert_eq!(t.screen.lines[1][0].fg, ANSI[1]);
        t.feed(b"\x1b[1;2HX\x1b[6n");
        assert!(t.events.last().unwrap().contains("1;3R"));
    }
    #[test]
    fn utf8_fragmentation_and_width() {
        let mut t = Terminal::new(8, 2);
        for b in "é界e\u{301}".as_bytes() {
            t.feed(&[*b]);
        }
        assert_eq!(width('é'), 1);
        assert_eq!(width('界'), 2);
        assert_eq!(width('e'), 1);
        assert_eq!(t.screen.x, 4);
        assert_eq!(t.screen.lines[0][3].text, "e\u{301}");
        assert_eq!(t.screen.lines[0][2].attr & 16, 16);
    }
    #[test]
    fn alternate_and_resize() {
        let mut t = Terminal::new(8, 2);
        t.feed(b"main\x1b[?1049hgame");
        assert!(text(&t, 0).starts_with("game"));
        t.resize(10, 3);
        t.feed(b"\x1b[?1049l");
        assert!(text(&t, 0).starts_with("main"));
        assert_eq!(t.screen.lines.len(), 3);
        assert_eq!(t.screen.lines[0].len(), 10);
    }
    #[test]
    fn grows_scrollback_and_sequences() {
        let mut t = Terminal::new(8, 3);
        for _ in 0..10000 {
            t.feed(b"line\r\n");
        }
        assert_eq!(t.history.len(), 9998);
        let payload = "a".repeat(2 * 1024 * 1024);
        t.feed(b"\x1b_G");
        t.feed(payload.as_bytes());
        t.feed(b"\x1b\\");
        assert!(t.events.last().unwrap().len() > payload.len());
        t.snapshot(9998);
        assert_eq!(t.history[0][0].text, "l");
        assert!(t.output.starts_with("[8,3,"));
    }
    #[test]
    fn graphics_and_mode_queries() {
        let mut t = Terminal::new(8, 2);
        t.feed(b"\x1b[?2004h\x1b[?2004$p\x1b[?u\x1b_Ga=q,i=7;AAAA\x1b\\\x1b[c");
        assert!(t.events.iter().any(|x| x.contains("2004;1")));
        assert!(t.events.iter().any(|x| x.contains("a=q,i=7")));
        assert!(t.events.last().unwrap().contains("62;4;22"));
    }
}
