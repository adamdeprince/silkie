// License: GPLv3

package atspi

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"os"
	"testing"
	"time"

	"github.com/kovidgoyal/dbus"
)

func TestTextNavigation(t *testing.T) {
	old, newText := []rune("a😀 old\n"), []rune("a😀 saved\n")
	start, deleted, inserted := textDiff(old, newText)
	if start != 3 || string(deleted) != "ol" || string(inserted) != "save" {
		t.Fatalf("diff: %d %q %q", start, deleted, inserted)
	}
	start, end := textBounds(newText, 3, 3)
	if actual := sliceText(newText, start, end); actual != string(newText) {
		t.Fatalf("line: %q", actual)
	}
	if actual := sliceText(newText, 1, 2); actual != "😀" {
		t.Fatalf("Unicode range: %q", actual)
	}
}

type testRegistry struct {
	embedded chan reference
	self     reference
}

func (r testRegistry) Embed(app reference) (reference, *dbus.Error) {
	r.embedded <- app
	return r.self, nil
}

func TestAccessibilityBus(t *testing.T) {
	addr := os.Getenv("SILKIE_TEST_A11Y_BUS")
	if addr == "" {
		t.Skip("set SILKIE_TEST_A11Y_BUS to an isolated D-Bus session")
	}
	t.Setenv("AT_SPI_BUS_ADDRESS", addr)
	conn, err := dbus.Connect(addr)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if _, err := conn.RequestName(prefix+"Registry", dbus.NameFlagDoNotQueue); err != nil {
		t.Fatal(err)
	}
	embeds := make(chan reference, 1)
	if err := conn.Export(testRegistry{embeds, reference{conn.Names()[0], root}}, root, prefix+"Socket"); err != nil {
		t.Fatal(err)
	}
	inReader, inWriter := io.Pipe()
	outReader, outWriter := io.Pipe()
	done := make(chan error, 1)
	go func() {
		done <- Run(inReader, outWriter)
		outWriter.Close()
	}()
	defer inWriter.Close()
	defer outReader.Close()

	ready := make(chan string, 1)
	go func() { line, _ := bufio.NewReader(outReader).ReadString('\n'); ready <- line }()
	var app reference
	select {
	case app = <-embeds:
	case err := <-done:
		t.Fatalf("bridge ended before registering: %v", err)
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	select {
	case reply := <-ready:
		if reply != "ready\n" {
			t.Fatalf("startup reply: %q", reply)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}

	signals := make(chan *dbus.Signal, 32)
	conn.Signal(signals)
	defer conn.RemoveSignal(signals)
	if err := conn.AddMatchSignal(dbus.WithMatchInterface(prefix+"Event.Object"), dbus.WithMatchObjectPath(panePath(2))); err != nil {
		t.Fatal(err)
	}
	payload := json.NewEncoder(inWriter)
	pane := Pane{ID: 2, Title: "Shell", Focused: true, Text: "a😀 saved", Caret: 2,
		Selections: [][2]int32{{1, 2}}, Rows: [][]int32{{0, 1, 1, 2, 3, 4, 5, 6, 7, 8}},
		Roles: []Role{{3, 8, "status"}}, Frame: [4]int32{0, 0, 90, 20}}
	windows := []Window{{ID: 1, Title: "Terminal", Focused: true, Panes: []Pane{pane}}}
	if err := payload.Encode(update{Windows: &windows}); err != nil {
		t.Fatal(err)
	}
	wait := func(member string) *dbus.Signal {
		t.Helper()
		for {
			select {
			case signal := <-signals:
				if signal.Name == prefix+"Event.Object."+member {
					return signal
				}
			case <-ctx.Done():
				t.Fatalf("waiting for %s: %v", member, ctx.Err())
			}
		}
	}
	wait("StateChanged")
	obj := conn.Object(app.Bus, panePath(2))
	var word string
	if err := obj.CallWithContext(ctx, prefix+"Text.GetText", 0, int32(1), int32(2)).Store(&word); err != nil || word != "😀" {
		t.Fatalf("GetText: %q, %v", word, err)
	}
	var first, last int32
	if err := obj.CallWithContext(ctx, prefix+"Text.GetSelection", 0, int32(0)).Store(&first, &last); err != nil || first != 1 || last != 2 {
		t.Fatalf("GetSelection: %d, %d, %v", first, last, err)
	}
	var items []cacheItem
	if err := conn.Object(app.Bus, cachePath).CallWithContext(ctx, prefix+"Cache.GetItems", 0).Store(&items); err != nil || len(items) != 3 {
		t.Fatalf("GetItems: %d, %v", len(items), err)
	}
	for range 64 {
		if err := payload.Encode(update{Announcement: &announcement{2, "File saved", false}}); err != nil {
			t.Fatal(err)
		}
		s := wait("Announcement")
		if v, ok := s.Body[3].(dbus.Variant); !ok || v.Value() != "File saved" {
			t.Fatalf("announcement: %v", s.Body)
		}
	}
	if err := inWriter.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
}
