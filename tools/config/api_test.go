// License: GPLv3 Copyright: 2023, Kovid Goyal, <kovid at kovidgoyal.net>

package config

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/go-cmp/cmp"
	"github.com/kovidgoyal/kitty/tools/utils"
)

var _ = fmt.Print

func TestSilkieConfigLoading(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("SILKIE_CONFIG_DIRECTORY", dir)
	t.Setenv("KITTY_PID", "")
	original := utils.ConfigDir
	utils.ConfigDir = func() string { return dir }
	t.Cleanup(func() { utils.ConfigDir = original })
	for name, content := range map[string]string{
		"kitty.conf":  "source kitty\n",
		"silkie.conf": "include extra.conf\n",
		"extra.conf":  "source silkie\n",
		"diff.conf":   "source diff\n",
	} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	var values []string
	handler := func(key, val string) error {
		values = append(values, val)
		return nil
	}
	if err := ReadKittyConfig(handler); err != nil {
		t.Fatal(err)
	}
	if diff := cmp.Diff([]string{"silkie"}, values); diff != "" {
		t.Fatal(diff)
	}
	values = nil
	parser := ConfigParser{LineHandler: handler}
	if err := parser.LoadConfig("diff.conf", nil, nil); err != nil {
		t.Fatal(err)
	}
	if diff := cmp.Diff([]string{"diff"}, values); diff != "" {
		t.Fatal(diff)
	}
}

func TestConfigParsing(t *testing.T) {
	tdir := t.TempDir()
	conf_file := filepath.Join(tdir, "a.conf")
	if err := os.Mkdir(filepath.Join(tdir, "sub"), 0o700); err != nil {
		t.Fatal(err)
	}
	w := func(path string, data []byte) {
		if err := os.WriteFile(path, data, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	w(filepath.Join(tdir, "g.py"), []byte(`
print('gpy 1')
print('gpy 2')
`))
	w(conf_file, []byte(
		`error main
# igno
     \re me
a	one
#: other
include
\ sub/b.conf
b x
include non-exis
\tent
globin
\clude sub/c?.c
   \onf
badline
geninclude g.py
`))
	w(filepath.Join(tdir, "sub/b.conf"), []byte("incb cool\ninclude a.conf"))
	w(filepath.Join(tdir, "sub/c1.conf"), []byte("inc1 cool"))
	w(filepath.Join(tdir, "sub/c2.conf"), []byte("inc2 cool\nenvinclude ENVINCLUDE"))
	w(filepath.Join(tdir, "sub/c.conf"), []byte("inc notcool\nerror sub"))

	var parsed_lines []string
	pl := func(key, val string) error {
		if key == "error" {
			return fmt.Errorf("%s", val)
		}
		parsed_lines = append(parsed_lines, key+" "+val)
		return nil
	}

	p := ConfigParser{LineHandler: pl, override_env: []string{"ENVINCLUDE=env cool\ninclude c.conf"}}
	err := p.ParseFiles(conf_file)
	if err != nil {
		t.Fatal(err)
	}
	if err = p.ParseOverrides("over one", "over two"); err != nil {
		t.Fatal(err)
	}
	diff := cmp.Diff([]string{"a one", "incb cool", "b x", "inc1 cool", "inc2 cool", "env cool", "inc notcool", "gpy 1", "gpy 2", "over one", "over two"}, parsed_lines)
	if diff != "" {
		t.Fatalf("Unexpected parsed config values:\n%s", diff)
	}
	bad_lines := []string{}
	for _, bl := range p.BadLines() {
		bad_lines = append(bad_lines, fmt.Sprintf("%s: %d", filepath.Base(bl.Src_file), bl.Line_number))
	}
	diff = cmp.Diff([]string{"a.conf: 1", "c.conf: 2", "a.conf: 14"}, bad_lines)
	if diff != "" {
		t.Fatalf("Unexpected bad lines:\n%s", diff)
	}
}
