// License: GPLv3 Copyright: 2026, Adam DePrince

package utils

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSilkieConfigPaths(t *testing.T) {
	root := t.TempDir()
	t.Setenv("HOME", root)
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(root, "xdg"))
	t.Setenv("XDG_CONFIG_DIRS", "")
	t.Setenv("SILKIE_CONFIG_DIRECTORY", "")
	t.Setenv("KITTY_CONFIG_DIRECTORY", filepath.Join(root, "kitty-override"))
	write := func(base, name string) string {
		dir := filepath.Join(root, base)
		if err := os.MkdirAll(dir, 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, name), nil, 0o600); err != nil {
			t.Fatal(err)
		}
		return dir
	}
	check := func(name, expected string) {
		t.Helper()
		if actual := ConfigDirForName(name); actual != expected {
			t.Fatalf("config directory for %s: got %s, want %s", name, actual, expected)
		}
	}
	write("xdg/kitty", "kitty.conf")
	write("xdg/silkie", "kitty.conf")
	check("silkie.conf", filepath.Join(root, "xdg/silkie"))
	system := write("system/silkie", "silkie.conf")
	t.Setenv("XDG_CONFIG_DIRS", strings.Repeat(filepath.Join(root, "missing")+":", 1024)+filepath.Dir(system))
	check("silkie.conf", system)
	home := write(".config/silkie", "silkie.conf")
	check("silkie.conf", home)
	xdg := write("xdg/silkie", "silkie.conf")
	check("silkie.conf", xdg)
	write("xdg/silkie", "diff.conf")
	check("diff.conf", xdg)
	t.Setenv("SILKIE_CONFIG_DIRECTORY", "~/custom")
	check("silkie.conf", filepath.Join(root, "custom"))
	check("diff.conf", filepath.Join(root, "custom"))
}
