// License: GPLv3 Copyright: 2026, Adam DePrince

package utils

import (
	"os"
)

// SilkieEnv returns the value of the environment variable SILKIE_<name>, falling back to KITTY_<name>, and whether either
// is set. SILKIE_<name> is the documented name. KITTY_<name> is still read for compatibility with existing scripts written
// for kitty or for older versions of Silkie. SILKIE_<name> wins whenever it is set, even to an empty value, so that it can
// override a KITTY_<name> inherited from the environment.
func SilkieEnv(name string) (string, bool) {
	if ans, found := os.LookupEnv("SILKIE_" + name); found {
		return ans, true
	}
	return os.LookupEnv("KITTY_" + name)
}
