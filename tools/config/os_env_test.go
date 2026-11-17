// License: GPLv3 Copyright: 2026, Adam DePrince

package config

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestOSNameVariables(t *testing.T) {
	for _, name := range []string{"SILKIE_OS", "KITTY_OS"} {
		if actual := ExpandVars("${" + name + "}"); actual != kitty_os() {
			t.Fatalf("%s expanded to %#v, want %#v", name, actual, kitty_os())
		}
	}
	// geninclude programs get both names, Python ones included
	tdir := t.TempDir()
	gen := filepath.Join(tdir, "g.py")
	if err := os.WriteFile(gen, []byte("import os\nprint('os', os.environ['SILKIE_OS'], os.environ['KITTY_OS'])\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	conf := filepath.Join(tdir, "a.conf")
	if err := os.WriteFile(conf, []byte("geninclude g.py\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var parsed_lines []string
	p := ConfigParser{LineHandler: func(key, val string) error { parsed_lines = append(parsed_lines, key+" "+val); return nil }}
	if err := p.ParseFiles(conf); err != nil {
		t.Fatal(err)
	}
	if diff := cmp.Diff([]string{"os " + kitty_os() + " " + kitty_os()}, parsed_lines); diff != "" {
		t.Fatalf("Unexpected parsed config values:\n%s", diff)
	}
}
