// License: GPLv3 Copyright: 2026, Adam DePrince

package utils

import (
	"os"
	"testing"
)

func TestSilkieEnvPrecedence(t *testing.T) {
	check := func(expected string, expected_found bool) {
		t.Helper()
		if actual, found := SilkieEnv("TEST_PRECEDENCE"); actual != expected || found != expected_found {
			t.Fatalf("SilkieEnv: got %#v %v, want %#v %v", actual, found, expected, expected_found)
		}
	}
	t.Setenv("SILKIE_TEST_PRECEDENCE", "")
	t.Setenv("KITTY_TEST_PRECEDENCE", "")
	os.Unsetenv("SILKIE_TEST_PRECEDENCE")
	os.Unsetenv("KITTY_TEST_PRECEDENCE")
	check("", false)
	t.Setenv("KITTY_TEST_PRECEDENCE", "kitty")
	check("kitty", true)
	t.Setenv("SILKIE_TEST_PRECEDENCE", "silkie")
	check("silkie", true)
	// An empty SILKIE_ value is still set, and overrides an inherited KITTY_ one
	t.Setenv("SILKIE_TEST_PRECEDENCE", "")
	check("", true)
}
