// License: GPLv3 Copyright: 2026, Adam DePrince

package at

import (
	"os"
	"testing"
)

func TestPasswordEnvFallback(t *testing.T) {
	check := func(password_env, expected string, expected_set bool) {
		t.Helper()
		ans, err := get_password("", "", password_env, "if-available")
		if err != nil {
			t.Fatal(err)
		}
		if ans.val != expected || ans.is_set != expected_set {
			t.Fatalf("password from %s: got %#v %v, want %#v %v", password_env, ans.val, ans.is_set, expected, expected_set)
		}
	}
	t.Setenv("SILKIE_RC_PASSWORD", "")
	t.Setenv("KITTY_RC_PASSWORD", "")
	os.Unsetenv("SILKIE_RC_PASSWORD")
	os.Unsetenv("KITTY_RC_PASSWORD")
	check("SILKIE_RC_PASSWORD", "", false)
	t.Setenv("KITTY_RC_PASSWORD", "kitty")
	check("SILKIE_RC_PASSWORD", "kitty", true)
	t.Setenv("SILKIE_RC_PASSWORD", "silkie")
	check("SILKIE_RC_PASSWORD", "silkie", true)
	// An explicitly named variable is read alone
	check("KITTY_RC_PASSWORD", "kitty", true)
	t.Setenv("OTHER_RC_PASSWORD", "other")
	check("OTHER_RC_PASSWORD", "other", true)
}
