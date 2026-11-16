// SPDX-License-Identifier: GPL-3.0-only
use silkie_console_session::{handoff_query, read_acknowledgement};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};

static CHILD_GROUP: AtomicI32 = AtomicI32::new(0);
static CHILD_HAS_GROUP: AtomicBool = AtomicBool::new(true);
static HANDOFF_SIGNAL: AtomicI32 = AtomicI32::new(0);

#[cfg(target_os = "linux")]
fn errno_pointer() -> *mut libc::c_int {
    unsafe { libc::__errno_location() }
}
#[cfg(target_os = "macos")]
fn errno_pointer() -> *mut libc::c_int {
    unsafe { libc::__error() }
}

fn signal_child(pid: libc::pid_t, has_group: bool, signal: libc::c_int) -> libc::c_int {
    let result = unsafe { libc::kill(if has_group { -pid } else { pid }, signal) };
    // Some virtual kernels accept setpgid yet cannot address that group in
    // kill(). Preserve group delivery on POSIX and fall back to the known
    // child PID on ESRCH. Both calls are async-signal-safe.
    if result < 0 && has_group && unsafe { *errno_pointer() } == libc::ESRCH {
        unsafe { libc::kill(pid, signal) }
    } else {
        result
    }
}

extern "C" fn forward_signal(signal: libc::c_int) {
    // A handler may interrupt waitpid/read; preserve their errno even when
    // forwarding to a child group that has just exited returns ESRCH.
    let errno = errno_pointer();
    let saved_errno = unsafe { *errno };
    let group = CHILD_GROUP.load(Ordering::SeqCst);
    if group > 0 {
        signal_child(group, CHILD_HAS_GROUP.load(Ordering::SeqCst), signal);
    } else {
        HANDOFF_SIGNAL.store(signal, Ordering::SeqCst);
    }
    unsafe {
        *errno = saved_errno;
    }
}

fn cvt(result: libc::c_int) -> io::Result<()> {
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

struct Signals {
    actions: Vec<(libc::c_int, libc::sigaction)>,
    mask: libc::sigset_t,
}

impl Signals {
    fn install() -> io::Result<Self> {
        // Parent performs tty cleanup while the child owns the foreground, so
        // block background-tty signals here. The child restores the old mask.
        let mut mask = unsafe { std::mem::zeroed() };
        let mut blocked = unsafe { std::mem::zeroed() };
        unsafe {
            libc::sigemptyset(&mut blocked);
            libc::sigaddset(&mut blocked, libc::SIGTTOU);
            libc::sigaddset(&mut blocked, libc::SIGTTIN);
        }
        cvt(unsafe { libc::sigprocmask(libc::SIG_BLOCK, &blocked, &mut mask) })?;
        let mut signals = Self {
            actions: Vec::new(),
            mask,
        };
        for signal in [
            libc::SIGINT,
            libc::SIGTERM,
            libc::SIGHUP,
            libc::SIGQUIT,
            libc::SIGTSTP,
        ] {
            let mut action: libc::sigaction = unsafe { std::mem::zeroed() };
            action.sa_sigaction = forward_signal as *const () as usize;
            unsafe {
                libc::sigemptyset(&mut action.sa_mask);
            }
            let mut previous = unsafe { std::mem::zeroed() };
            cvt(unsafe { libc::sigaction(signal, &action, &mut previous) })?;
            signals.actions.push((signal, previous));
        }
        Ok(signals)
    }
}

impl Drop for Signals {
    fn drop(&mut self) {
        for (signal, action) in &self.actions {
            unsafe {
                libc::sigaction(*signal, action, std::ptr::null_mut());
            }
        }
        unsafe {
            libc::sigprocmask(libc::SIG_SETMASK, &self.mask, std::ptr::null_mut());
        }
    }
}

struct Terminal {
    saved: libc::termios,
    raw: libc::termios,
    group: libc::pid_t,
}

impl Terminal {
    fn open() -> io::Result<Self> {
        if unsafe { libc::isatty(0) != 1 || libc::isatty(1) != 1 } {
            return Err(io::Error::other("stdin and stdout must be a terminal"));
        }
        let mut saved = unsafe { std::mem::zeroed() };
        cvt(unsafe { libc::tcgetattr(0, &mut saved) })?;
        let group = unsafe { libc::getpgrp() };
        let foreground = unsafe { libc::tcgetpgrp(0) };
        if foreground < 0 {
            return Err(io::Error::last_os_error());
        }
        if foreground != group {
            return Err(io::Error::other(
                "console session must start in the foreground",
            ));
        }
        let mut raw = saved;
        unsafe {
            libc::cfmakeraw(&mut raw);
        }
        let terminal = Self { saved, raw, group };
        terminal.raw()?;
        Ok(terminal)
    }

    fn raw(&self) -> io::Result<()> {
        cvt(unsafe { libc::tcsetattr(0, libc::TCSANOW, &self.raw) })
    }
    fn restore(&self) -> io::Result<()> {
        cvt(unsafe { libc::tcsetattr(0, libc::TCSANOW, &self.saved) })
    }
    fn foreground(&self, group: libc::pid_t) -> io::Result<()> {
        cvt(unsafe { libc::tcsetpgrp(0, group) })
    }
}

impl Drop for Terminal {
    fn drop(&mut self) {
        let _ = self.foreground(self.group);
        let _ = self.restore();
    }
}

fn nonce() -> io::Result<u32> {
    let mut random = File::open("/dev/urandom")?;
    loop {
        let mut bytes = [0; 4]; // The protocol request id is a positive int31.
        random.read_exact(&mut bytes)?;
        let id = u32::from_ne_bytes(bytes) & 0x7fff_ffff;
        if id != 0 {
            return Ok(id);
        }
    }
}

fn state_path() -> PathBuf {
    std::env::var_os("SILKIE_CONSOLE_STATE")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp/silkie-demo-foreground"))
}

fn publish_target(path: &Path, pid: libc::pid_t) -> io::Result<()> {
    if pid <= 0 {
        return Err(io::Error::other("foreground PID must be positive"));
    }
    loop {
        let temporary = path.with_extension(format!("{}.{}", std::process::id(), nonce()?));
        let mut file = match OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&temporary)
        {
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
            result => result?,
        };
        let result = file
            .write_all(pid.to_string().as_bytes())
            .and_then(|_| fs::rename(&temporary, path));
        if result.is_err() {
            let _ = fs::remove_file(&temporary);
        }
        return result;
    }
}

fn read_target(path: &Path) -> io::Result<libc::pid_t> {
    let value = fs::read_to_string(path)?;
    value
        .parse::<libc::pid_t>()
        .ok()
        .filter(|pid| *pid > 0)
        .ok_or_else(|| io::Error::other("invalid foreground PID"))
}

struct ForegroundTarget {
    path: PathBuf,
    previous: Option<libc::pid_t>,
}
impl ForegroundTarget {
    fn open() -> io::Result<Self> {
        let path = state_path();
        let previous = match read_target(&path) {
            Ok(pid) => Some(pid),
            Err(error) if error.kind() == io::ErrorKind::NotFound => None,
            Err(error) => return Err(error),
        };
        Ok(Self { path, previous })
    }
    fn select(&self, pid: libc::pid_t) -> io::Result<()> {
        publish_target(&self.path, pid)
    }
    fn restore(&self) -> io::Result<()> {
        if let Some(pid) = self.previous {
            publish_target(&self.path, pid)
        } else {
            match fs::remove_file(&self.path) {
                Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
                result => result,
            }
        }
    }
}
impl Drop for ForegroundTarget {
    fn drop(&mut self) {
        let _ = self.restore();
    }
}

struct TerminalInput;
impl Read for TerminalInput {
    fn read(&mut self, into: &mut [u8]) -> io::Result<usize> {
        if HANDOFF_SIGNAL.load(Ordering::SeqCst) != 0 {
            return Err(io::Error::other("input handoff interrupted by signal"));
        }
        let count = unsafe { libc::read(0, into.as_mut_ptr().cast(), into.len()) };
        if count < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(count as usize)
        }
    }
}

struct RunningChild {
    child: std::process::Child,
    pid: libc::pid_t,
    reaped: bool,
    has_group: bool,
}

fn handoff(terminal: &Terminal) -> io::Result<()> {
    terminal.foreground(terminal.group)?;
    terminal.raw()?;
    let id = nonce()?;
    let mut output = io::stdout().lock();
    output.write_all(handoff_query(id).as_bytes())?;
    output.flush()?;
    read_acknowledgement(&mut TerminalInput, id)
}

impl Drop for RunningChild {
    fn drop(&mut self) {
        CHILD_GROUP.store(0, Ordering::SeqCst);
        if !self.reaped {
            signal_child(self.pid, self.has_group, libc::SIGKILL);
            loop {
                match self.child.wait() {
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                    _ => break,
                }
            }
        }
    }
}

fn run() -> io::Result<i32> {
    let mut args = std::env::args_os().skip(1);
    let executable = args
        .next()
        .ok_or_else(|| io::Error::other("usage: silkie-console-session PROGRAM [ARGS...]"))?;
    if executable == "--features" {
        let path = args
            .next()
            .ok_or_else(|| io::Error::other("--features needs a report file"))?;
        if args.next().is_some() {
            return Err(io::Error::other("--features takes one report file"));
        }
        let report = fs::read_to_string(path)?;
        let columns = silkie_console_session::formatting::terminal_columns(1)?.unwrap_or(80);
        silkie_console_session::formatting::print_wrapped(
            &mut io::stdout().lock(),
            &report,
            columns,
        )?;
        return Ok(0);
    }
    if executable == "--shell" {
        let executable = args
            .next()
            .ok_or_else(|| io::Error::other("--shell needs a program"))?;
        // Establish normal foreground ownership where supported, and publish
        // the shell PID for virtual consoles with incomplete process groups.
        let signals = Signals::install()?;
        let pid = unsafe { libc::getpid() };
        if unsafe { libc::getpgrp() } != pid {
            cvt(unsafe { libc::setpgid(0, 0) })?;
        }
        cvt(unsafe { libc::tcsetpgrp(0, pid) })?;
        publish_target(&state_path(), pid)?;
        // Restore signal dispositions and mask before replacing this process.
        drop(signals);
        return Err(Command::new(executable).args(args).exec());
    }
    if executable == "--notify-resize" {
        if args.next().is_some() {
            return Err(io::Error::other("--notify-resize takes no arguments"));
        }
        // SDK-created commands cannot infer the original console's process
        // from their own tty. Signal its explicitly published foreground PID;
        // virtual process groups do not implement Linux group-kill semantics.
        cvt(unsafe { libc::kill(read_target(&state_path())?, libc::SIGWINCH) })?;
        return Ok(0);
    }
    let signals = Signals::install()?;
    // Raw before spawn is essential: the application's own raw-mode restore
    // then restores raw, so unread release tails are never echoed by the tty.
    let terminal = Terminal::open()?;
    let foreground = ForegroundTarget::open()?;
    let mut command = Command::new(executable);
    command.args(args);
    let child_mask = signals.mask;
    unsafe {
        command.pre_exec(move || {
            cvt(libc::setpgid(0, 0))?;
            cvt(libc::tcsetpgrp(0, libc::getpgrp()))?;
            cvt(libc::sigprocmask(
                libc::SIG_SETMASK,
                &child_mask,
                std::ptr::null_mut(),
            ))?;
            Ok(())
        });
    }
    let child = command.spawn()?;
    let pid =
        i32::try_from(child.id()).map_err(|_| io::Error::other("child process id overflow"))?;
    let has_group = unsafe { libc::getpgid(pid) } == pid;
    let mut child = RunningChild {
        child,
        pid,
        reaped: false,
        has_group,
    };
    CHILD_HAS_GROUP.store(has_group, Ordering::SeqCst);
    CHILD_GROUP.store(pid, Ordering::SeqCst);
    foreground.select(pid)?;
    let early_signal = HANDOFF_SIGNAL.swap(0, Ordering::SeqCst);
    if early_signal != 0 {
        signal_child(pid, has_group, early_signal);
    }
    let status = loop {
        let mut status = 0;
        let result = unsafe { libc::waitpid(pid, &mut status, libc::WUNTRACED) };
        if result < 0 {
            let error = io::Error::last_os_error();
            if error.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            return Err(error);
        }
        if result != pid {
            continue;
        }
        if libc::WIFSTOPPED(status) {
            // Suspending returns control to the shell too. Retire the old
            // input epoch before restoring cooked mode, just as on exit.
            foreground.select(unsafe { libc::getpid() })?;
            handoff(&terminal)?;
            terminal.restore()?;
            foreground.restore()?;
            // Stop unconditionally, including orphaned groups where SIGTSTP
            // would be ignored. The shell can now run `fg`/`bg` normally.
            unsafe {
                libc::raise(libc::SIGSTOP);
            }
            // A background resume must not steal the terminal from its shell.
            loop {
                let foreground = unsafe { libc::tcgetpgrp(0) };
                if foreground < 0 {
                    return Err(io::Error::last_os_error());
                }
                if foreground == terminal.group {
                    break;
                }
                unsafe {
                    libc::raise(libc::SIGSTOP);
                }
            }
            terminal.raw()?;
            terminal.foreground(pid)?;
            foreground.select(pid)?;
            signal_child(pid, has_group, libc::SIGCONT);
            continue;
        }
        if !libc::WIFEXITED(status) && !libc::WIFSIGNALED(status) {
            continue;
        }
        child.reaped = true;
        break std::process::ExitStatus::from_raw(status);
    };
    CHILD_GROUP.store(0, Ordering::SeqCst);
    foreground.select(unsafe { libc::getpid() })?;
    handoff(&terminal)?;
    terminal.restore()?;
    foreground.restore()?;
    Ok(status
        .code()
        .unwrap_or_else(|| 128 + status.signal().unwrap_or(0)))
}

fn main() -> ExitCode {
    match run() {
        Ok(status) => ExitCode::from(status as u8),
        Err(error) => {
            let signal = HANDOFF_SIGNAL.load(Ordering::SeqCst);
            eprintln!("silkie-console-session: {error}");
            ExitCode::from(if signal != 0 { (128 + signal) as u8 } else { 1 })
        }
    }
}
