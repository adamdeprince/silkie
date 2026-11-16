// SPDX-License-Identifier: GPL-3.0-only
//! Width-aware plain text and OSC 8 output for the guest's startup report.
use std::io::{self, Write};
use unicode_segmentation::UnicodeSegmentation;
use unicode_width::UnicodeWidthStr;

#[derive(Clone, Copy)]
struct Piece<'a> {
    text: &'a str,
    columns: usize,
    escape: bool,
}

struct Word<'a> {
    pieces: Vec<Piece<'a>>,
    columns: usize,
}

// Return complete escape sequences as indivisible zero-width pieces. An
// unfinished sequence leaves the following text visible instead of hiding it.
fn escape_len(text: &str) -> usize {
    let bytes = text.as_bytes();
    if bytes.first() != Some(&0x1b) {
        return 0;
    }
    match bytes.get(1) {
        Some(b']' | b'P' | b'_' | b'^' | b'X') => {
            let mut at = 2;
            while at < bytes.len() {
                if bytes[at] == 7 {
                    return at + 1;
                }
                if bytes[at] == 0x1b && bytes.get(at + 1) == Some(&b'\\') {
                    return at + 2;
                }
                at += 1;
            }
            1
        }
        Some(b'[') => bytes
            .iter()
            .enumerate()
            .skip(2)
            .find_map(|(at, byte)| (0x40..=0x7e).contains(byte).then_some(at + 1))
            .unwrap_or(1),
        Some(byte) if byte.is_ascii() => 2,
        _ => 1,
    }
}

fn words(text: &str) -> Vec<Word<'_>> {
    let mut words = Vec::new();
    let mut word = Word {
        pieces: Vec::new(),
        columns: 0,
    };
    let mut at = 0;
    while at < text.len() {
        let escape = escape_len(&text[at..]);
        if escape > 0 {
            word.pieces.push(Piece {
                text: &text[at..at + escape],
                columns: 0,
                escape: true,
            });
            at += escape;
            continue;
        }
        let end = text[at..]
            .find('\x1b')
            .map_or(text.len(), |offset| at + offset);
        for grapheme in text[at..end].graphemes(true) {
            if grapheme.chars().all(char::is_whitespace) {
                if !word.pieces.is_empty() {
                    words.push(word);
                    word = Word {
                        pieces: Vec::new(),
                        columns: 0,
                    };
                }
            } else {
                let columns = UnicodeWidthStr::width(grapheme);
                word.columns += columns;
                word.pieces.push(Piece {
                    text: grapheme,
                    columns,
                    escape: false,
                });
            }
        }
        at = end;
    }
    if !word.pieces.is_empty() {
        words.push(word);
    }
    words
}

struct Printer<'a, W> {
    output: &'a mut W,
    hyperlink: Option<String>,
    columns: usize,
    occupied: bool,
}

impl<W: Write> Printer<'_, W> {
    fn indentation(&mut self, spaces: usize) -> io::Result<()> {
        for _ in 0..spaces {
            self.output.write_all(b" ")?;
        }
        self.columns = spaces;
        self.occupied = false;
        Ok(())
    }

    fn newline(&mut self, indent: usize) -> io::Result<()> {
        if self.hyperlink.is_some() {
            self.output.write_all(b"\x1b]8;;\x1b\\")?;
        }
        self.output.write_all(b"\n")?;
        self.indentation(indent)?;
        if let Some(link) = &self.hyperlink {
            self.output.write_all(link.as_bytes())?;
        }
        Ok(())
    }

    fn piece(&mut self, piece: Piece<'_>) -> io::Result<()> {
        self.output.write_all(piece.text.as_bytes())?;
        if piece.escape {
            if let Some(body) = piece.text.strip_prefix("\x1b]8;") {
                let body = body
                    .strip_suffix("\x1b\\")
                    .or_else(|| body.strip_suffix('\x07'))
                    .unwrap_or(body);
                if let Some((_, uri)) = body.split_once(';') {
                    self.hyperlink = (!uri.is_empty()).then(|| piece.text.to_owned());
                }
            }
        } else {
            self.columns += piece.columns;
            self.occupied |= piece.columns > 0;
        }
        Ok(())
    }
}

/// Reflow each source line as a paragraph. Preserve blank lines, hanging bullet
/// and label indents, Unicode graphemes, and clickable OSC 8 labels. An overlong
/// word breaks only when it cannot fit on a fresh line at the requested width.
/// At one column a two-column grapheme necessarily occupies its own wider line;
/// its text is preserved rather than dropped or replaced.
pub fn print_wrapped(output: &mut impl Write, report: &str, columns: usize) -> io::Result<()> {
    if columns == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "terminal width must be positive",
        ));
    }
    let mut printer = Printer {
        output,
        hyperlink: None,
        columns: 0,
        occupied: false,
    };
    for line in report.split_terminator('\n') {
        let line = line.strip_suffix('\r').unwrap_or(line);
        let content = line.trim_start_matches([' ', '\t']);
        let indent = line[..line.len() - content.len()]
            .chars()
            .fold(
                0usize,
                |at, ch| if ch == '\t' { at + 8 - at % 8 } else { at + 1 },
            );
        let words = words(content);
        if words.is_empty() {
            printer.newline(0)?;
            continue;
        }
        let first_plain = words[0]
            .pieces
            .iter()
            .filter(|piece| !piece.escape)
            .map(|piece| piece.text)
            .collect::<String>();
        let hanging = if first_plain == "-"
            || first_plain == "*"
            || (first_plain.ends_with(':') && words.len() > 1)
        {
            indent + words[0].columns + 1
        } else {
            indent
        };
        let longest = words.iter().map(|word| word.columns).max().unwrap_or(0);
        // Reduce decorative indentation on narrow terminals so a command that
        // fits the screen still fits intact on a continuation line.
        let room = columns.saturating_sub(longest.min(columns));
        let continuation = hanging.min(room);
        printer.indentation(indent.min(room))?;
        for word in words {
            if printer.occupied {
                if word.columns > columns.saturating_sub(printer.columns.saturating_add(1)) {
                    printer.newline(continuation)?;
                } else {
                    printer.output.write_all(b" ")?;
                    printer.columns += 1;
                }
            }
            for piece in word.pieces {
                if piece.columns > 0
                    && printer.occupied
                    && piece.columns > columns.saturating_sub(printer.columns)
                {
                    printer.newline(continuation)?;
                }
                printer.piece(piece)?;
            }
        }
        printer.newline(0)?;
    }
    Ok(())
}

/// Query stdout on every invocation: shell startup, fullscreen, and subsequent
/// `silkie-features` commands can all have different terminal dimensions.
pub fn terminal_columns(fd: libc::c_int) -> io::Result<Option<usize>> {
    if unsafe { libc::isatty(fd) } != 1 {
        return Ok(None);
    }
    let mut size: libc::winsize = unsafe { std::mem::zeroed() };
    if unsafe { libc::ioctl(fd, libc::TIOCGWINSZ as _, &mut size) } < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((size.ws_col > 0).then_some(usize::from(size.ws_col)))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn render(input: &str, columns: usize) -> String {
        let mut output = Vec::new();
        print_wrapped(&mut output, input, columns).unwrap();
        String::from_utf8(output).unwrap()
    }

    fn visible(input: &str) -> String {
        let mut output = String::new();
        let mut at = 0;
        while at < input.len() {
            let escape = escape_len(&input[at..]);
            if escape > 0 {
                at += escape;
            } else {
                let ch = input[at..].chars().next().unwrap();
                output.push(ch);
                at += ch.len_utf8();
            }
        }
        output
    }

    #[test]
    fn bullets_and_command_labels_have_hanging_indents() {
        assert_eq!(
            render("  - No unrestricted host files or desktop windows.\n", 28),
            "  - No unrestricted host\n    files or desktop\n    windows.\n"
        );
        let commands = render("Commands: ls, cat, silkie-sprint, silkie-scramble.\n", 24);
        assert!(commands.contains("silkie-sprint,"));
        assert!(commands.contains("silkie-scramble."));
        assert!(commands.lines().all(|line| line.width() <= 24));
    }

    #[test]
    fn real_osc8_urls_are_zero_width_and_links_close_before_wrapped_indentation() {
        let link = "\x1b]8;;silkie:tailscale\x1b\\Connect to Tailscale\x1b]8;;\x1b\\";
        let formatted = render(
            &format!("{link} · \x1b]8;;silkie:ssh-help\x07SSH setup\x1b]8;;\x07\n"),
            13,
        );
        let stripped = visible(&formatted);
        assert!(
            stripped.lines().all(|line| line.width() <= 13),
            "{stripped:?}"
        );
        assert_eq!(
            stripped.split_whitespace().collect::<Vec<_>>().join(" "),
            "Connect to Tailscale · SSH setup"
        );
        assert!(
            formatted.matches("silkie:tailscale").count() > 1,
            "wrapped label must reopen its same target"
        );
        assert!(formatted.contains("\x1b]8;;\x1b\\\n\x1b]8;;silkie:tailscale"));
    }

    #[test]
    fn tiny_terminals_preserve_every_nonwhitespace_character() {
        let original = "  - Try silkie-scramble, silkie-sprint, and ssh.\n\nCommands: ls cat\n";
        for columns in 1..=80 {
            let formatted = render(original, columns);
            assert!(
                formatted.lines().all(|line| line.width() <= columns),
                "{columns}: {formatted:?}"
            );
            assert_eq!(
                formatted
                    .chars()
                    .filter(|ch| !ch.is_whitespace())
                    .collect::<String>(),
                original
                    .chars()
                    .filter(|ch| !ch.is_whitespace())
                    .collect::<String>()
            );
            assert!(formatted.contains("\n\n"));
        }
    }

    #[test]
    fn unicode_combining_wide_and_joined_graphemes_are_not_split() {
        let original = "cafe\u{301} 中文 👩‍💻 family 👨‍👩‍👧‍👦 end";
        for columns in 2..=22 {
            let formatted = render(original, columns);
            assert!(
                formatted.lines().all(|line| line.width() <= columns),
                "{columns}: {formatted:?}"
            );
            assert_eq!(
                formatted
                    .chars()
                    .filter(|ch| !ch.is_whitespace())
                    .collect::<String>(),
                original
                    .chars()
                    .filter(|ch| !ch.is_whitespace())
                    .collect::<String>()
            );
            assert!(formatted.contains("e\u{301}"));
            assert!(formatted.contains("👩‍💻") && formatted.contains("👨‍👩‍👧‍👦"));
        }
        assert!(render("中文", 1).contains('中'));
    }

    #[test]
    fn large_reports_and_long_command_names_grow_without_truncation() {
        let original = format!(
            "{}\n{}",
            "x".repeat(200_000),
            "  - retain every word and hyperlink\n".repeat(10_000)
        );
        let formatted = render(&original, 37);
        assert!(formatted.lines().all(|line| line.width() <= 37));
        assert_eq!(
            formatted.chars().filter(|ch| !ch.is_whitespace()).count(),
            original.chars().filter(|ch| !ch.is_whitespace()).count()
        );
    }
}
