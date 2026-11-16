// SPDX-License-Identifier: GPL-3.0-only
//! Run the production launcher between an actual shell and an actual PTY.
use silkie_console_session::acknowledgement;
use std::fs::File;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

struct Console {
    master: File,
    slave: File,
    shell: Child,
    output: Vec<u8>,
    application_group: Option<i32>,
    initial: libc::termios,
    handoffs: usize,
    directory: PathBuf,
}

impl Console {
    fn start(program: &str) -> Self {
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
        let master = unsafe { File::from_raw_fd(master) };
        let slave = unsafe { File::from_raw_fd(slave) };
        let directory = std::env::temp_dir().join(format!(
            "silkie-console-test-{}-{}-{}",
            std::process::id(),
            slave.as_raw_fd(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&directory).unwrap();
        let mut initial = unsafe { std::mem::zeroed() };
        assert_eq!(
            unsafe { libc::tcgetattr(slave.as_raw_fd(), &mut initial) },
            0
        );
        let script = r#"
            trap 'printf "\nOUTER_WINCH\n"' WINCH
            "$1" /bin/sh -c "$2"
            status=$?
            printf '\nEXIT=%s\n' "$status"
            IFS= read -r line
            printf '\nTAIL=%s\n' "$line"
            IFS= read -r shutdown
            printf '\nDONE\n'
        "#;
        let mut command = Command::new(env!("CARGO_BIN_EXE_silkie-console-session"));
        command
            .env("SILKIE_CONSOLE_STATE", directory.join("foreground"))
            .args([
                "--shell",
                "/bin/sh",
                "-c",
                script,
                "shell",
                env!("CARGO_BIN_EXE_silkie-console-session"),
                program,
            ])
            .stdin(Stdio::from(slave.try_clone().unwrap()))
            .stdout(Stdio::from(slave.try_clone().unwrap()))
            .stderr(Stdio::from(slave.try_clone().unwrap()));
        unsafe {
            command.pre_exec(|| {
                if libc::setsid() < 0 || libc::ioctl(0, libc::TIOCSCTTY as _, 0) < 0 {
                    return Err(io::Error::last_os_error());
                }
                Ok(())
            });
        }
        let shell = command.spawn().unwrap();
        Self {
            master,
            slave,
            shell,
            output: Vec::new(),
            application_group: None,
            initial,
            handoffs: 0,
            directory,
        }
    }

    fn text(&self) -> String {
        String::from_utf8_lossy(&self.output).into_owned()
    }

    fn until(&mut self, predicate: impl Fn(&str) -> bool) {
        // A test deadline detects deadlocks; production handoff has no timeout.
        let deadline = Instant::now() + Duration::from_secs(15);
        while !predicate(&self.text()) {
            let remaining = deadline.saturating_duration_since(Instant::now());
            assert!(!remaining.is_zero(), "PTY deadline: {}", self.text());
            let mut poll = libc::pollfd {
                fd: self.master.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            };
            assert!(
                unsafe { libc::poll(&mut poll, 1, remaining.as_millis() as i32) } > 0,
                "PTY poll deadline: {}",
                self.text()
            );
            let mut bytes = [0; 4096]; // Stream chunk; accumulated output grows.
            let count = self.master.read(&mut bytes).unwrap();
            assert!(count > 0, "PTY closed: {}", self.text());
            self.output.extend_from_slice(&bytes[..count]);
        }
    }

    fn ready(&mut self) -> (i32, i32) {
        self.until(|text| {
            text.contains("READY:")
                && text
                    .split("READY:")
                    .nth(1)
                    .is_some_and(|tail| tail.contains('\n'))
        });
        let line = self
            .text()
            .split("READY:")
            .nth(1)
            .unwrap()
            .lines()
            .next()
            .unwrap()
            .trim()
            .to_owned();
        let (child, launcher) = line.split_once(':').unwrap();
        let child = child.parse().unwrap();
        self.application_group = Some(child);
        (child, launcher.parse().unwrap())
    }

    fn raw(&self) -> bool {
        let mut attrs = unsafe { std::mem::zeroed() };
        assert_eq!(
            unsafe { libc::tcgetattr(self.slave.as_raw_fd(), &mut attrs) },
            0
        );
        attrs.c_lflag & (libc::ICANON | libc::ECHO) == 0
    }

    fn ack(&mut self) {
        self.ack_with_tail("typeahead preserved\n");
    }

    fn ack_with_tail(&mut self, following: &str) {
        let index = self.handoffs + 1;
        self.until(|text| {
            text.split("\x1b]777;browser-input;v=1:op=handoff:id=")
                .nth(index)
                .is_some_and(|tail| tail.contains("\x1b\\"))
        });
        let text = self.text();
        let tail = text
            .split("\x1b]777;browser-input;v=1:op=handoff:id=")
            .nth(index)
            .unwrap();
        let id: u32 = tail.split("\x1b\\").next().unwrap().parse().unwrap();
        assert!(id > 0 && id <= 0x7fff_ffff);
        // An unrelated response and old partial report must not end the wait;
        // the exact ACK must leave the following shell input queued intact.
        let wrong = if id == 1 { 2 } else { 1 };
        self.master
            .write_all(
                format!(
                    ";3u{}{}{following}",
                    acknowledgement(wrong),
                    acknowledgement(id)
                )
                .as_bytes(),
            )
            .unwrap();
        self.handoffs += 1;
    }

    fn finish(&mut self, status: i32) {
        self.until(|text| text.contains("TAIL=typeahead preserved"));
        assert!(
            self.text().contains(&format!("EXIT={status}")),
            "{}",
            self.text()
        );
        let mut restored: libc::termios = unsafe { std::mem::zeroed() };
        assert_eq!(
            unsafe { libc::tcgetattr(self.slave.as_raw_fd(), &mut restored) },
            0
        );
        assert_eq!(restored.c_iflag, self.initial.c_iflag);
        assert_eq!(restored.c_oflag, self.initial.c_oflag);
        assert_eq!(restored.c_cflag, self.initial.c_cflag);
        // The kernel may set PENDIN while recooking raw queued typeahead into
        // canonical input. It is transient queue state, not a changed setting.
        assert_eq!(
            restored.c_lflag & !libc::PENDIN,
            self.initial.c_lflag & !libc::PENDIN
        );
        assert_eq!(restored.c_cc, self.initial.c_cc);
        self.master.write_all(b"done\n").unwrap();
        self.until(|text| text.contains("DONE"));
        assert_eq!(self.shell.wait().unwrap().code(), Some(0));
        self.application_group = None;
    }
}

impl Drop for Console {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.directory);
        if let Some(group) = self.application_group {
            unsafe {
                libc::kill(-group, libc::SIGKILL);
            }
        }
        let _ = self.shell.kill();
        let _ = self.shell.wait();
    }
}

#[test]
fn queued_release_is_never_echoed_and_ack_preserves_the_next_shell_command() {
    let mut console = Console::start(
        "printf 'READY:%s:%s\\n' \"$$\" \"$PPID\"; dd bs=1 count=1 of=/dev/null 2>/dev/null; exit 7",
    );
    console.ready();
    assert!(console.raw());
    console.master.write_all(b"q\x1b[113;1:3u").unwrap();
    console.ack();
    assert!(
        !console.text().contains("113;1:3u"),
        "release was echoed: {}",
        console.text()
    );
    console.finish(7);
}

#[test]
fn launcher_forwards_term_and_performs_the_same_acknowledged_cleanup() {
    let mut console = Console::start(
        "trap 'exit 9' TERM; printf 'READY:%s:%s\\n' \"$$\" \"$PPID\"; while :; do read ignored; done",
    );
    let (_, launcher) = console.ready();
    assert_eq!(unsafe { libc::kill(launcher, libc::SIGTERM) }, 0);
    console.ack();
    console.finish(9);
}

#[test]
fn stopped_child_restores_outer_tty_and_continues_in_raw_foreground_mode() {
    let mut console = Console::start(
        "printf 'READY:%s:%s\\n' \"$$\" \"$PPID\"; dd bs=1 count=1 of=/dev/null 2>/dev/null; exit 0",
    );
    let (child, launcher) = console.ready();
    assert_eq!(unsafe { libc::kill(-child, libc::SIGSTOP) }, 0);
    console.ack_with_tail("");
    let deadline = Instant::now() + Duration::from_secs(15);
    while console.raw() {
        assert!(
            Instant::now() < deadline,
            "launcher failed to restore tty before stopping"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(unsafe { libc::kill(launcher, libc::SIGCONT) }, 0);
    while !console.raw() {
        assert!(
            Instant::now() < deadline,
            "launcher failed to restore raw mode on resume"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
    console.master.write_all(b"q").unwrap();
    console.ack();
    console.finish(0);
}

#[test]
fn resize_notifies_the_foreground_child_without_interrupting_the_waiting_shell() {
    let executable = env!("CARGO_BIN_EXE_silkie-console-session").replace('\'', "'\\''");
    let mut console = Console::start(&format!(
        "trap 'printf CHILD_WINCH\\n' WINCH; printf 'READY:%s:%s\\n' \"$$\" \"$PPID\"; '{executable}' --notify-resize; exit 23"
    ));
    console.ready();
    console.ack();
    console.finish(23);
    assert!(console.text().contains("CHILD_WINCH"), "{}", console.text());
    assert!(
        !console.text().contains("OUTER_WINCH"),
        "{}",
        console.text()
    );
}
