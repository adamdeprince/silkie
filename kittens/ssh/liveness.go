// License: GPLv3

package ssh

// Deciding when a shared connection (ControlMaster) is dead.
//
// Links range from a 2400 baud satellite modem, where a round trip takes
// seconds and the link stalls for several seconds during satellite handover,
// to 100 GbE, where a round trip takes microseconds. No fixed timeout suits
// both: one long enough for the satellite link leaves a window hung for that
// long on a fast link whose master has died, and one short enough for the fast
// link kills healthy masters on the satellite link, forcing a full key exchange
// for every new window. So liveness is decided in two ways:
//
//  1. ssh's own heartbeat. Masters the kitten starts get ServerAliveInterval and
//     ServerAliveCountMax (unless the user configured keepalives), so a master
//     whose connection has died exits on its own and ssh -O check, which is local
//     and instant, then reports that there is no master.
//  2. A timeout derived from measurement. Requests that need a round trip to the
//     server through a master are timed and the round trip time (RTT) of that
//     master is kept in the cache directory. A master is only declared dead
//     when a round trip takes far longer than its measured RTT. With no
//     measurement for a master, no timeout is used and only (1) applies.

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/sys/unix"

	"github.com/kovidgoyal/kitty/tools/utils"
)

// Keepalive settings for masters started by the kitten, used only when the
// user has not configured ServerAliveInterval themselves. ssh sends a
// keepalive request after keepalive_interval seconds in which nothing was
// received from the server and gives up after keepalive_count_max further
// intervals without a reply, so a master exits after about
// keepalive_interval * (keepalive_count_max + 1) = 150 seconds of total
// silence from the server. Any data from the server resets the count, so busy
// connections send no keepalives at all.
//
// Why these values: 150 seconds is far longer than any satellite handover or
// Wi-Fi/cellular roaming stall (seconds), so a slow but healthy link is never
// dropped, while a master whose connection died during suspend or a network
// change is gone within a few minutes even if no measurement exists to detect
// it sooner (detection with a measurement is much faster, see dead_after()).
// One keepalive is about 100 bytes on the wire including TCP/IP headers, so on
// an idle 2400 baud link one every 30 seconds uses about 1.5% of the
// bandwidth, and nothing when the link is busy.
const (
	keepalive_interval  = 30
	keepalive_count_max = 4
)

var ssh_option_on_command_line_pat = sync.OnceValue(func() *regexp.Regexp {
	return regexp.MustCompile(`^(?i)\s*([a-z]+)\s*(?:=|\s)`)
})

// Return whether the ssh command line arguments set the named ssh option with -o
func ssh_option_on_command_line(ssh_args []string, name string) bool {
	pat := ssh_option_on_command_line_pat()
	check := func(val string) bool {
		m := pat.FindStringSubmatch(val + " ")
		return m != nil && strings.EqualFold(m[1], name)
	}
	for i := 0; i < len(ssh_args); i++ {
		a := ssh_args[i]
		switch {
		case a == "-o" && i+1 < len(ssh_args):
			if check(ssh_args[i+1]) {
				return true
			}
			i++
		case strings.HasPrefix(a, "-o"):
			if check(a[2:]) {
				return true
			}
		}
	}
	return false
}

// The ssh options to add to the commands that may start a master, so that it
// exits on its own when its connection dies. Nothing is added when the user
// has configured ServerAliveInterval, in their ssh config (ssh -G reports a
// non-zero value) or on the command line (which may also explicitly turn it
// off with 0). ssh -G cannot distinguish ServerAliveInterval 0 in a config
// file from it not being set at all, so to turn keepalives off for masters the
// kitten starts, use -o ServerAliveInterval=0 on the command line.
func keepalive_args(cfg *SSHConfig, ssh_args []string) []string {
	if ssh_option_on_command_line(ssh_args, "ServerAliveInterval") {
		return nil
	}
	if cfg != nil && cfg.ServerAliveInterval != "" && cfg.ServerAliveInterval != "0" {
		return nil
	}
	ans := []string{"-o", fmt.Sprintf("ServerAliveInterval=%d", keepalive_interval)}
	countmax_set := ssh_option_on_command_line(ssh_args, "ServerAliveCountMax") || (cfg != nil && cfg.ServerAliveCountMax != "" && cfg.ServerAliveCountMax != "3")
	if !countmax_set {
		ans = append(ans, "-o", fmt.Sprintf("ServerAliveCountMax=%d", keepalive_count_max))
	}
	// TCP keepalives are spoofable and with system defaults take hours to
	// notice a dead connection, the encrypted ServerAlive messages replace
	// them. Only turn them off when we are the ones providing keepalives.
	if !ssh_option_on_command_line(ssh_args, "TCPKeepAlive") {
		ans = append(ans, "-o", "TCPKeepAlive=no")
	}
	return ans
}

// Round trip time estimation, as done by TCP for its retransmission timer
// (RFC 6298), with times in seconds
type rtt_estimate struct {
	Srtt    float64 `json:"srtt"`
	Rttvar  float64 `json:"rttvar"`
	Samples uint64  `json:"samples"`
}

func (e rtt_estimate) add_sample(d time.Duration) rtt_estimate {
	r := d.Seconds()
	if r < 0 {
		r = 0
	}
	if e.Samples == 0 {
		e.Srtt, e.Rttvar = r, r/2
	} else {
		e.Rttvar = 0.75*e.Rttvar + 0.25*math.Abs(e.Srtt-r)
		e.Srtt = 0.875*e.Srtt + 0.125*r
	}
	if e.Samples < math.MaxUint64 {
		e.Samples++
	}
	return e
}

// TCP's retransmission timeout for the estimate: the delay after which TCP
// itself concludes that a packet was lost
func (e rtt_estimate) rto() float64 {
	return e.Srtt + 4*e.Rttvar
}

// A master is declared dead when a round trip through it takes this many
// times longer than the retransmission timeout of its measured RTT. TCP
// retransmits a lost packet after one RTO and then backs off exponentially, so
// a live connection that loses packets for a while, as happens during a
// satellite handover, can take 1+2+4 = 7 RTOs to recover from three
// consecutive losses. 10 RTOs covers that with margin, while still detecting a
// dead master on a LAN in well under a second (each round trip measured here
// includes starting an ssh process, a few milliseconds). The cost of waiting
// too long is a window that stays blank a little longer; the cost of a false
// positive is a full key exchange and authentication for the new window, which
// on a slow link takes many round trips, so the margin is deliberately large.
const dead_master_rto_multiple = 10

// Convert seconds to a Duration, returning false if it does not fit
func seconds_to_duration(s float64) (time.Duration, bool) {
	ns := s * float64(time.Second)
	if math.IsNaN(ns) || ns < 0 || ns >= math.MaxInt64 {
		return 0, false
	}
	return time.Duration(ns), true
}

func (e rtt_estimate) dead_after() (time.Duration, bool) {
	if e.Samples == 0 {
		return 0, false
	}
	return seconds_to_duration(e.rto() * dead_master_rto_multiple)
}

// The timeout after which a master that has not responded to a round trip is
// declared dead. override is the shared_connection_timeout option: negative
// for automatic, 0 to never time out, otherwise a number of seconds. Returns
// false when there is no timeout.
func master_timeout(override float64, est rtt_estimate) (time.Duration, bool) {
	switch {
	case override == 0:
		return 0, false
	case override > 0:
		return seconds_to_duration(override)
	default:
		return est.dead_after()
	}
}

// Parse the value of the shared_connection_timeout option
func seconds_or_auto(val string) (float64, error) {
	val = strings.TrimSpace(val)
	if strings.EqualFold(val, "auto") {
		return -1, nil
	}
	ans, err := strconv.ParseFloat(val, 64)
	if err != nil {
		return 0, fmt.Errorf("%#v is not auto or a number of seconds", val)
	}
	if math.IsNaN(ans) || ans < 0 {
		return 0, fmt.Errorf("%#v is not auto or a non-negative number of seconds", val)
	}
	if math.IsInf(ans, 1) {
		ans = 0 // never time out
	}
	return ans, nil
}

var master_pid_pat = sync.OnceValue(func() *regexp.Regexp {
	return regexp.MustCompile(`\(pid=(\d+)\)`)
})

// Parse the pid from the output of ssh -O check, returning 0 if not present
func parse_master_pid(output string) int {
	if m := master_pid_pat().FindStringSubmatch(output); m != nil {
		if pid, err := strconv.Atoi(m[1]); err == nil {
			return pid
		}
	}
	return 0
}

// Identifies the destination of a connection, the same things OpenSSH hashes
// for the %C token used in the ControlPath
func host_identity(cfg *SSHConfig, hostname string) string {
	if cfg == nil || cfg.Hostname == "" {
		return "\x00" + hostname
	}
	return strings.Join([]string{cfg.User, cfg.Hostname, cfg.Port, cfg.ProxyJump, cfg.ProxyCommand}, "\x00")
}

// Persistent per master RTT measurements. Each destination has a file holding
// the estimates for its masters keyed by master pid. Estimates are per master
// rather than per destination because each master is its own connection,
// possibly over a different link: a measurement made over fibre must not be
// used for a connection made later over a satellite modem. Entries for
// masters that no longer exist are dropped whenever the file is written, so
// the store holds one small file per destination.
type rtt_store struct {
	dir string
	// Used to check if a master still exists, replaceable for tests
	pid_exists func(int) bool
}

func default_pid_exists(pid int) bool {
	err := unix.Kill(pid, 0)
	return err == nil || errors.Is(err, unix.EPERM)
}

func new_rtt_store() *rtt_store {
	return &rtt_store{dir: filepath.Join(utils.CacheDir(), "ssh-rtt"), pid_exists: default_pid_exists}
}

type rtt_store_file struct {
	Masters map[string]rtt_estimate `json:"masters"`
}

func (s *rtt_store) path(identity string) string {
	h := sha256.Sum256([]byte(identity))
	return filepath.Join(s.dir, hex.EncodeToString(h[:])+".json")
}

func (s *rtt_store) read(identity string) (ans rtt_store_file) {
	data, err := os.ReadFile(s.path(identity))
	if err == nil {
		// A corrupt file is the same as no measurement
		_ = json.Unmarshal(data, &ans)
	}
	if ans.Masters == nil {
		ans.Masters = make(map[string]rtt_estimate)
	}
	return
}

func (s *rtt_store) load(identity string, master_pid int) rtt_estimate {
	if master_pid <= 0 {
		return rtt_estimate{}
	}
	return s.read(identity).Masters[strconv.Itoa(master_pid)]
}

func (s *rtt_store) record(identity string, master_pid int, d time.Duration) (rtt_estimate, error) {
	if master_pid <= 0 {
		return rtt_estimate{}, nil
	}
	f := s.read(identity)
	key := strconv.Itoa(master_pid)
	for k := range f.Masters {
		if k == key {
			continue
		}
		if pid, err := strconv.Atoi(k); err != nil || !s.pid_exists(pid) {
			delete(f.Masters, k)
		}
	}
	ans := f.Masters[key].add_sample(d)
	f.Masters[key] = ans
	data, err := json.Marshal(f)
	if err != nil {
		return ans, err
	}
	if err = os.MkdirAll(s.dir, 0o700); err != nil && !errors.Is(err, fs.ErrExist) {
		return ans, err
	}
	return ans, utils.AtomicWriteFile(s.path(identity), bytes.NewReader(data), 0o600)
}

// Copies the output of a session to the terminal, noting whether there has
// been any. A session that has produced output got it through its master, so
// that master is alive and the session must never be killed.
type output_watcher struct {
	seen atomic.Bool
	done chan struct{}
}

func watch_output(src io.ReadCloser, dest io.Writer) *output_watcher {
	w := &output_watcher{done: make(chan struct{})}
	go func() {
		defer close(w.done)
		defer src.Close()
		buf := make([]byte, 64*1024) // just the copy chunk size, any size works
		write_failed := false
		for {
			n, err := src.Read(buf)
			if n > 0 {
				w.seen.Store(true)
				// After a failed write keep reading, so the session is not
				// blocked on a full pipe, the output has nowhere to go anyway
				if !write_failed {
					_, werr := dest.Write(buf[:n])
					write_failed = werr != nil
				}
			}
			if err != nil {
				return
			}
		}
	}()
	return w
}
