// SPDX-License-Identifier: GPL-3.0-only
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use unicode_width::UnicodeWidthStr;

struct Report(std::path::PathBuf);
impl Report {
    fn create(contents: &str) -> Self {
        let path = std::env::temp_dir().join(format!(
            "silkie-features-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .unwrap()
            .write_all(contents.as_bytes())
            .unwrap();
        Self(path)
    }
}
impl Drop for Report {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

fn visible(text: &str) -> String {
    let mut result = String::new();
    let mut rest = text;
    while let Some(start) = rest.find("\x1b]8;") {
        result.push_str(&rest[..start]);
        rest = &rest[start..];
        let end = rest.find("\x1b\\").expect("complete OSC 8 frame") + 2;
        rest = &rest[end..];
    }
    result.push_str(rest);
    result.replace('\r', "")
}

#[test]
fn features_cli_reads_the_live_tty_width_again_after_every_resize() {
    let report = Report::create(
        "Silkie runs a Linux shell in this browser tab.\nCommands: ls cat ssh silkie-sprint silkie-scramble\n  - No unrestricted host files or desktop windows.\n\x1b]8;;silkie:tailscale\x1b\\Connect to Tailscale\x1b]8;;\x1b\\\nDONE\n",
    );
    let mut master = 0;
    let mut slave = 0;
    assert_eq!(
        unsafe {
            libc::openpty(
                &mut master,
                &mut slave,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            )
        },
        0
    );
    let mut master = unsafe { File::from_raw_fd(master) };
    let slave = unsafe { File::from_raw_fd(slave) };
    for columns in [24, 65, 93, 12] {
        let dimensions = libc::winsize {
            ws_row: 20,
            ws_col: columns,
            ws_xpixel: 0,
            ws_ypixel: 0,
        };
        assert_eq!(
            unsafe { libc::ioctl(slave.as_raw_fd(), libc::TIOCSWINSZ as _, &dimensions) },
            0
        );
        let mut child = Command::new(env!("CARGO_BIN_EXE_silkie-console-session"))
            .arg("--features")
            .arg(&report.0)
            .stdin(Stdio::null())
            .stdout(Stdio::from(slave.try_clone().unwrap()))
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(15);
        let mut output = Vec::new();
        while !output.ends_with(b"DONE\r\n") && !output.ends_with(b"DONE\n") {
            let remaining = deadline.saturating_duration_since(Instant::now());
            assert!(!remaining.is_zero(), "features printer did not finish");
            let mut poll = libc::pollfd {
                fd: master.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            };
            assert!(
                unsafe { libc::poll(&mut poll, 1, remaining.as_millis() as i32) } > 0,
                "features printer stalled"
            );
            let mut bytes = [0; 4096]; // Read chunk, never an output limit.
            let count = master.read(&mut bytes).unwrap();
            assert!(count > 0);
            output.extend_from_slice(&bytes[..count]);
        }
        assert!(child.wait().unwrap().success());
        let text = visible(std::str::from_utf8(&output).unwrap());
        assert!(
            text.lines()
                .all(|line| line.width() <= usize::from(columns)),
            "{columns}: {text:?}"
        );
        if columns >= 24 {
            assert!(text.contains("silkie-scramble"));
        }
        if columns >= 65 {
            assert!(text.lines().any(|line| line.width() > 24));
        }
        assert!(
            std::str::from_utf8(&output)
                .unwrap()
                .contains("silkie:tailscale")
        );
        assert_eq!(
            text.chars()
                .filter(|ch| !ch.is_whitespace())
                .collect::<String>(),
            "SilkierunsaLinuxshellinthisbrowsertab.Commands:lscatsshsilkie-sprintsilkie-scramble-Nounrestrictedhostfilesordesktopwindows.ConnecttoTailscaleDONE"
        );
    }
}

#[test]
fn features_cli_accepts_redirected_stdout_without_changing_terminal_modes() {
    let report = Report::create(&"word ".repeat(500));
    let output = Command::new(env!("CARGO_BIN_EXE_silkie-console-session"))
        .arg("--features")
        .arg(&report.0)
        .output()
        .unwrap();
    assert!(output.status.success());
    let text = String::from_utf8(output.stdout).unwrap();
    assert!(text.lines().all(|line| line.width() <= 80));
    assert_eq!(text.split_whitespace().count(), 500);
}
