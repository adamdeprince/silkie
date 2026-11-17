// License: GPLv3

package ssh

import (
	"bytes"
	"io"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/google/go-cmp/cmp"
)

func TestSSHKeepaliveArgs(t *testing.T) {
	ours := []string{"-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=4", "-o", "TCPKeepAlive=no"}
	defaults := &SSHConfig{ServerAliveInterval: "0", ServerAliveCountMax: "3"}
	for _, tc := range []struct {
		name     string
		cfg      *SSHConfig
		args     []string
		expected []string
	}{
		{"unset", defaults, nil, ours},
		{"ssh -G failed", nil, nil, ours},
		{"other options", defaults, []string{"-o", "User=x", "-4", "-oPort=22"}, ours},
		{"interval in config", &SSHConfig{ServerAliveInterval: "10", ServerAliveCountMax: "3"}, nil, nil},
		{"interval on command line", defaults, []string{"-o", "ServerAliveInterval=0"}, nil},
		{"interval on command line joined", defaults, []string{"-oserveraliveinterval 0"}, nil},
		{"interval on command line with spaces", defaults, []string{"-o", "ServerAliveInterval = 5"}, nil},
		{"countmax in config", &SSHConfig{ServerAliveInterval: "0", ServerAliveCountMax: "9"}, nil,
			[]string{"-o", "ServerAliveInterval=30", "-o", "TCPKeepAlive=no"}},
		{"countmax on command line", defaults, []string{"-o", "ServerAliveCountMax=3"},
			[]string{"-o", "ServerAliveInterval=30", "-o", "TCPKeepAlive=no"}},
		{"tcpkeepalive on command line", defaults, []string{"-o", "TCPKeepAlive=yes"},
			[]string{"-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=4"}},
		{"a value that mentions the option", defaults, []string{"-o", "SetEnv=X=ServerAliveInterval=1"}, ours},
	} {
		if diff := cmp.Diff(tc.expected, keepalive_args(tc.cfg, tc.args)); diff != "" {
			t.Fatalf("%s: unexpected keepalive args:\n%s", tc.name, diff)
		}
	}
}

func TestSSHConfigDump(t *testing.T) {
	long := strings.Repeat("x", 1<<20) // longer than any fixed line buffer
	text := "user kovid\nhostname example.com\r\nport 2222\nproxyjump none\nproxycommand nc %h %p\n" +
		"serveraliveinterval 0\nserveralivecountmax 3\nremotecommand " + long + "\nbadline\n"
	c := parse_ssh_config_dump(text)
	expected := &SSHConfig{RemoteCommand: long, User: "kovid", Hostname: "example.com", Port: "2222", ProxyCommand: "nc %h %p",
		ServerAliveInterval: "0", ServerAliveCountMax: "3"}
	if diff := cmp.Diff(expected, c); diff != "" {
		t.Fatalf("Unexpected ssh -G parse:\n%s", diff)
	}
	if host_identity(c, "x") == host_identity(&SSHConfig{User: "kovid", Hostname: "example.com", Port: "22"}, "x") {
		t.Fatalf("Different ports must give different identities")
	}
	if host_identity(nil, "a") == host_identity(nil, "b") {
		t.Fatalf("Different hostnames must give different identities when ssh -G failed")
	}
}

func TestSSHMasterPid(t *testing.T) {
	if pid := parse_master_pid("Master running (pid=12345)\r\n"); pid != 12345 {
		t.Fatalf("Got pid: %d", pid)
	}
	if pid := parse_master_pid("Control socket connect(/x): No such file or directory\n"); pid != 0 {
		t.Fatalf("Got pid: %d", pid)
	}
}

func TestSSHRTTEstimate(t *testing.T) {
	approx := func(a, b float64) bool { return math.Abs(a-b) < 1e-9 }
	var e rtt_estimate
	if _, ok := e.dead_after(); ok {
		t.Fatalf("No measurement must mean no timeout")
	}
	e = e.add_sample(2 * time.Second)
	if !approx(e.Srtt, 2) || !approx(e.Rttvar, 1) || e.Samples != 1 {
		t.Fatalf("Unexpected first sample estimate: %#v", e)
	}
	// RTO = 2 + 4*1 = 6s, times the multiple
	if d, ok := e.dead_after(); !ok || d != 60*time.Second {
		t.Fatalf("Unexpected timeout: %s", d)
	}
	e = e.add_sample(time.Second)
	// RFC 6298: rttvar = 3/4*1 + 1/4*|2-1|, srtt = 7/8*2 + 1/8*1
	if !approx(e.Rttvar, 1) || !approx(e.Srtt, 1.875) || e.Samples != 2 {
		t.Fatalf("Unexpected second sample estimate: %#v", e)
	}
	// A fast link gets a short timeout
	var f rtt_estimate
	for range 20 {
		f = f.add_sample(10 * time.Millisecond)
	}
	if d, ok := f.dead_after(); !ok || d > time.Second || d < 100*time.Millisecond {
		t.Fatalf("Unexpected timeout for a fast link: %s", d)
	}
	// The override
	if _, ok := master_timeout(0, e); ok {
		t.Fatalf("0 must mean never time out")
	}
	if d, ok := master_timeout(2.5, rtt_estimate{}); !ok || d != 2500*time.Millisecond {
		t.Fatalf("An explicit timeout must apply even without a measurement: %s %v", d, ok)
	}
	if d, ok := master_timeout(-1, e); !ok || d != func() time.Duration { d, _ := e.dead_after(); return d }() {
		t.Fatalf("auto must use the measurement: %s", d)
	}
	if _, ok := master_timeout(-1, rtt_estimate{}); ok {
		t.Fatalf("auto without a measurement must mean no timeout")
	}
	if _, ok := master_timeout(1e300, e); ok {
		t.Fatalf("A timeout too large for a Duration must mean no timeout")
	}
}

func TestSSHSecondsOrAuto(t *testing.T) {
	for val, expected := range map[string]float64{"auto": -1, " AUTO ": -1, "0": 0, "2.5": 2.5, "30": 30, "inf": 0} {
		if actual, err := seconds_or_auto(val); err != nil || actual != expected {
			t.Fatalf("seconds_or_auto(%#v) = %v, %v", val, actual, err)
		}
	}
	for _, val := range []string{"", "-1", "nan", "never", "5s"} {
		if _, err := seconds_or_auto(val); err == nil {
			t.Fatalf("seconds_or_auto(%#v) did not fail", val)
		}
	}
	// The option, per host, via the ssh.conf parser
	cf := filepath.Join(t.TempDir(), "ssh.conf")
	conf := "shared_connection_timeout 7\nhostname slow\nshared_connection_timeout 0\nhostname bad\nshared_connection_timeout soon\n"
	if err := os.WriteFile(cf, []byte(conf), 0o600); err != nil {
		t.Fatal(err)
	}
	for host, expected := range map[string]float64{"other": 7, "slow": 0} {
		c, bad_lines, err := load_config(host, "", nil, cf)
		if err != nil {
			t.Fatal(err)
		}
		if len(bad_lines) != 1 || bad_lines[0].Line != "shared_connection_timeout soon" {
			t.Fatalf("Expected exactly the bad value to be reported, got: %#v", bad_lines)
		}
		if c.Shared_connection_timeout != expected {
			t.Fatalf("Unexpected shared_connection_timeout for %s: %v", host, c.Shared_connection_timeout)
		}
	}
	if c := NewConfig(); c.Shared_connection_timeout != -1 {
		t.Fatalf("Default must be auto, got: %v", c.Shared_connection_timeout)
	}
	c, _, err := load_config("x", "", []string{"shared_connection_timeout=auto"}, cf)
	if err != nil {
		t.Fatal(err)
	}
	if c.Shared_connection_timeout != -1 {
		t.Fatalf("Override not applied: %v", c.Shared_connection_timeout)
	}
}

func TestSSHRTTStore(t *testing.T) {
	alive := map[int]bool{100: true, 200: true, 300: true}
	s := &rtt_store{dir: filepath.Join(t.TempDir(), "ssh-rtt"), pid_exists: func(pid int) bool { return alive[pid] }}
	id := host_identity(&SSHConfig{User: "u", Hostname: "h", Port: "22"}, "h")
	if e := s.load(id, 100); e.Samples != 0 {
		t.Fatalf("Unexpected measurement in empty store: %#v", e)
	}
	if _, err := s.record(id, 100, 2*time.Second); err != nil {
		t.Fatal(err)
	}
	if _, err := s.record(id, 100, time.Second); err != nil {
		t.Fatal(err)
	}
	// A fresh store object, as used by a later kitten invocation
	s2 := &rtt_store{dir: s.dir, pid_exists: s.pid_exists}
	e := s2.load(id, 100)
	if e.Samples != 2 || math.Abs(e.Srtt-1.875) > 1e-9 {
		t.Fatalf("Measurement not persisted: %#v", e)
	}
	// Another master for the same host has no measurement
	if e := s2.load(id, 200); e.Samples != 0 {
		t.Fatalf("Measurement must be per master: %#v", e)
	}
	// Another host has no measurement
	if e := s2.load(host_identity(&SSHConfig{User: "u", Hostname: "other", Port: "22"}, "other"), 100); e.Samples != 0 {
		t.Fatalf("Measurement must be per host: %#v", e)
	}
	// No master pid, nothing recorded
	if _, err := s2.record(id, 0, time.Second); err != nil {
		t.Fatal(err)
	}
	if e := s2.load(id, 0); e.Samples != 0 {
		t.Fatalf("Measurement recorded without a master: %#v", e)
	}
	// Entries for masters that have exited are dropped on the next write
	if _, err := s2.record(id, 200, time.Second); err != nil {
		t.Fatal(err)
	}
	delete(alive, 100)
	if _, err := s2.record(id, 300, time.Second); err != nil {
		t.Fatal(err)
	}
	f := s2.read(id)
	if _, found := f.Masters["100"]; found || len(f.Masters) != 2 {
		t.Fatalf("Exited master not pruned: %#v", f.Masters)
	}
	// A corrupt file is the same as no measurement and is replaced
	if err := os.WriteFile(s2.path(id), []byte("{garbage"), 0o600); err != nil {
		t.Fatal(err)
	}
	if e := s2.load(id, 300); e.Samples != 0 {
		t.Fatalf("Unexpected measurement from corrupt file: %#v", e)
	}
	if e, err := s2.record(id, 300, time.Second); err != nil || e.Samples != 1 {
		t.Fatalf("Failed to record over corrupt file: %#v %v", e, err)
	}
	if st, err := os.Stat(s2.path(id)); err != nil || st.Mode().Perm() != 0o600 {
		t.Fatalf("Unexpected store file permissions: %v %v", st, err)
	}
}

type failing_writer struct{}

func (failing_writer) Write([]byte) (int, error) { return 0, io.ErrClosedPipe }

func TestSSHOutputWatcher(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	var b bytes.Buffer
	ow := watch_output(r, &b)
	if ow.seen.Load() {
		t.Fatalf("Output seen before any was written")
	}
	data := strings.Repeat("abc", 100000)
	if _, err = w.WriteString(data); err != nil {
		t.Fatal(err)
	}
	w.Close()
	<-ow.done
	if !ow.seen.Load() || b.String() != data {
		t.Fatalf("Output not copied faithfully, seen: %v len: %d", ow.seen.Load(), b.Len())
	}
	// A failing destination does not block the session
	r, w, err = os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	ow = watch_output(r, failing_writer{})
	if _, err = w.WriteString(data); err != nil {
		t.Fatal(err)
	}
	w.Close()
	<-ow.done
	if !ow.seen.Load() {
		t.Fatalf("Output not seen")
	}
}
