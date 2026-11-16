// SPDX-License-Identifier: GPL-3.0-only
use std::io::{self, Read};

pub mod formatting;

pub fn handoff_query(id: u32) -> String {
    format!("\x1b]777;browser-input;v=1:op=handoff:id={id}\x1b\\")
}

pub fn acknowledgement(id: u32) -> String {
    format!("\x1b]777;browser-input;v=1:op=ack:id={id}\x1b\\")
}

/// Consume exactly through this handoff's acknowledgement, retaining no input
/// history and leaving every byte after the acknowledgement unread. Reports
/// from the finished application can be fragmented, stale, or arbitrarily long.
pub fn read_acknowledgement(reader: &mut impl Read, id: u32) -> io::Result<()> {
    let pattern = acknowledgement(id).into_bytes();
    // KMP's storage depends on the actual acknowledgement, not the input stream.
    let mut prefix = vec![0; pattern.len()];
    let mut matched = 0;
    for index in 1..pattern.len() {
        while matched > 0 && pattern[index] != pattern[matched] {
            matched = prefix[matched - 1];
        }
        if pattern[index] == pattern[matched] {
            matched += 1;
        }
        prefix[index] = matched;
    }
    matched = 0;
    loop {
        // This is deliberately a single-byte stream read: a bulk read could
        // consume the next shell command, which must remain in the tty queue.
        let mut byte = [0];
        match reader.read(&mut byte) {
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "terminal closed before acknowledging input handoff",
                ));
            }
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error),
        }
        while matched > 0 && byte[0] != pattern[matched] {
            matched = prefix[matched - 1];
        }
        if byte[0] == pattern[matched] {
            matched += 1;
            if matched == pattern.len() {
                return Ok(());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn exact_nonce_and_terminator_leave_following_shell_input_unread() {
        let input = format!(
            ";3u{}{}\x1b]777;browser-input;v=1:op=ack:id=123x\x1b\\{}printf 'ready'\n",
            acknowledgement(12),
            acknowledgement(1234),
            acknowledgement(123)
        );
        let mut reader = Cursor::new(input.into_bytes());
        read_acknowledgement(&mut reader, 123).unwrap();
        let mut tail = String::new();
        reader.read_to_string(&mut tail).unwrap();
        assert_eq!(tail, "printf 'ready'\n");
    }

    #[test]
    fn fragmented_and_interrupted_reads_match_overlapping_prefixes() {
        struct Fragmented {
            bytes: Cursor<Vec<u8>>,
            interrupted: bool,
        }
        impl Read for Fragmented {
            fn read(&mut self, into: &mut [u8]) -> io::Result<usize> {
                self.interrupted = !self.interrupted;
                if self.interrupted {
                    return Err(io::ErrorKind::Interrupted.into());
                }
                assert_eq!(into.len(), 1);
                self.bytes.read(into)
            }
        }
        let mut reader = Fragmented {
            bytes: Cursor::new(
                format!("\x1b\x1b]777;browser-input;v=1:op=a{}", acknowledgement(1)).into_bytes(),
            ),
            interrupted: false,
        };
        read_acknowledgement(&mut reader, 1).unwrap();
    }

    #[test]
    fn sustained_stale_reports_do_not_accumulate_or_truncate() {
        let stale = acknowledgement(2147483646).into_bytes();
        let mut reader =
            Cursor::new(stale.repeat(100_000)).chain(Cursor::new(acknowledgement(2147483647)));
        read_acknowledgement(&mut reader, 2147483647).unwrap();
    }

    #[test]
    fn incomplete_ack_is_an_error_and_does_not_accept_bel() {
        for input in [
            "".to_owned(),
            acknowledgement(1).replace("\x1b\\", "\x07"),
            acknowledgement(2),
        ] {
            assert_eq!(
                read_acknowledgement(&mut Cursor::new(input), 1)
                    .unwrap_err()
                    .kind(),
                io::ErrorKind::UnexpectedEof
            );
        }
    }
}
