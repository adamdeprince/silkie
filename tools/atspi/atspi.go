// License: GPLv3

// Package atspi exports Silkie's terminal pane snapshots on the accessibility bus.
package atspi

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"slices"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/kovidgoyal/dbus"
	"github.com/kovidgoyal/dbus/introspect"
)

const (
	prefix     = "org.a11y.atspi."
	root       = dbus.ObjectPath("/org/a11y/atspi/accessible/root")
	null       = dbus.ObjectPath("/org/a11y/atspi/null")
	cachePath  = dbus.ObjectPath("/org/a11y/atspi/cache")
	properties = "org.freedesktop.DBus.Properties"
)

type reference struct {
	Bus  string
	Path dbus.ObjectPath
}

type Role struct {
	Start, End int32
	Name       string
}

type Pane struct {
	ID          uint64
	Title, Text string
	Focused     bool
	Caret       int32
	Selections  [][2]int32
	Cells       [][][2]int32
	Rows        [][]int32
	Roles       []Role
	Frame       [4]int32
}

type Window struct {
	ID      uint64
	Title   string
	Focused bool
	Origin  [2]int32
	Panes   []Pane
}

type announcement struct {
	Pane      uint64
	Text      string
	Assertive bool
}

type update struct {
	Windows      *[]Window
	Announcement *announcement
}

type node struct {
	parent   dbus.ObjectPath
	children []dbus.ObjectPath
	title    string
	role     uint32
	focused  bool
	origin   [2]int32
	pane     *Pane
	runes    []rune
}

type bridge struct {
	mu      sync.RWMutex
	conn    *dbus.Conn
	bus     string
	desktop reference
	id      int32
	nodes   map[dbus.ObjectPath]node
}

func winPath(id uint64) dbus.ObjectPath {
	return dbus.ObjectPath(fmt.Sprintf("%s/window_%d", root, id))
}
func panePath(id uint64) dbus.ObjectPath { return dbus.ObjectPath(fmt.Sprintf("%s/pane_%d", root, id)) }

func incomingPath(m dbus.Message) dbus.ObjectPath {
	path, _ := m.Headers[dbus.FieldPath].Value().(dbus.ObjectPath)
	return path
}

func (b *bridge) ref(path dbus.ObjectPath) reference {
	if path == "" || path == null {
		return reference{"", null}
	}
	return reference{b.bus, path}
}

func (b *bridge) node(m dbus.Message) node {
	b.mu.RLock()
	defer b.mu.RUnlock()
	return b.nodes[incomingPath(m)]
}

func (b *bridge) signal(path dbus.ObjectPath, name, detail string, first, second int32, value any) {
	_ = b.conn.Emit(path, prefix+name, detail, first, second, dbus.MakeVariant(value), map[string]dbus.Variant{})
}

func (b *bridge) update(windows []Window) {
	next := map[dbus.ObjectPath]node{}
	app := node{title: "Silkie", role: 75}
	for _, w := range windows {
		wp := winPath(w.ID)
		frame := node{parent: root, title: w.Title, role: 23, focused: w.Focused, origin: w.Origin}
		for _, p := range w.Panes {
			pp := panePath(p.ID)
			next[pp] = node{parent: wp, title: p.Title, role: 60, focused: p.Focused, origin: w.Origin, pane: &p, runes: []rune(p.Text)}
			frame.children = append(frame.children, pp)
		}
		next[wp] = frame
		app.children = append(app.children, wp)
	}
	next[root] = app
	b.mu.Lock()
	old := b.nodes
	b.nodes = next
	b.mu.Unlock()

	for path, before := range old {
		after, present := next[path]
		for i, child := range before.children {
			if !slices.Contains(after.children, child) {
				b.signal(path, "Event.Object.ChildrenChanged", "remove", int32(i), 0, b.ref(child))
			}
		}
		if before.focused && (!present || !after.focused) {
			if before.role == 23 {
				b.signal(path, "Event.Window.Deactivate", "", 0, 0, "")
			} else {
				b.signal(path, "Event.Object.StateChanged", "focused", 0, 0, "")
			}
		}
		if !present {
			_ = b.conn.Emit(cachePath, prefix+"Cache.RemoveAccessible", b.ref(path))
		}
	}
	for path, after := range next {
		before := old[path]
		if before.role == 0 {
			b.mu.RLock()
			item := b.cacheItem(path, after)
			b.mu.RUnlock()
			_ = b.conn.Emit(cachePath, prefix+"Cache.AddAccessible", item)
		}
		for i, child := range after.children {
			if !slices.Contains(before.children, child) {
				b.signal(path, "Event.Object.ChildrenChanged", "add", int32(i), 0, b.ref(child))
			}
		}
		if after.focused && !before.focused && after.role == 23 {
			b.signal(path, "Event.Window.Activate", "", 0, 0, "")
		}
	}
	for path, after := range next {
		if after.pane == nil {
			continue
		}
		before := old[path]
		if after.focused && !before.focused {
			b.signal(path, "Event.Object.StateChanged", "focused", 1, 0, "")
		}
		if before.pane == nil {
			continue
		}
		if before.pane.Text != after.pane.Text {
			start, removed, inserted := textDiff(before.runes, after.runes)
			if len(removed) != 0 {
				b.signal(path, "Event.Object.TextChanged", "delete", start, int32(len(removed)), string(removed))
			}
			if len(inserted) != 0 {
				b.signal(path, "Event.Object.TextChanged", "insert", start, int32(len(inserted)), string(inserted))
			}
		}
		if before.pane.Caret != after.pane.Caret {
			b.signal(path, "Event.Object.TextCaretMoved", "", after.pane.Caret, 0, "")
		}
		if !slices.Equal(before.pane.Selections, after.pane.Selections) {
			b.signal(path, "Event.Object.TextSelectionChanged", "", 0, 0, "")
		}
	}
}

func textDiff(before, after []rune) (int32, []rune, []rune) {
	start, a, z := 0, len(before), len(after)
	for start < min(a, z) && before[start] == after[start] {
		start++
	}
	for a > start && z > start && before[a-1] == after[z-1] {
		a--
		z--
	}
	return int32(start), before[start:a], after[start:z]
}

type accessible struct{ b *bridge }

func (a accessible) GetRole(m dbus.Message) (uint32, *dbus.Error) { return a.b.node(m).role, nil }
func (a accessible) GetRoleName(m dbus.Message) (string, *dbus.Error) {
	return map[uint32]string{75: "application", 23: "frame", 60: "terminal"}[a.b.node(m).role], nil
}
func (a accessible) GetLocalizedRoleName(m dbus.Message) (string, *dbus.Error) {
	return a.GetRoleName(m)
}
func statesFor(n node) []uint32 {
	if n.role == 0 {
		return []uint32{1 << 6, 0}
	} // defunct
	states := uint32(1<<8 | 1<<24 | 1<<25 | 1<<30) // enabled, sensitive, showing, visible
	if n.pane != nil {
		states |= 1<<11 | 1<<17 // focusable, multiline
		if n.focused {
			states |= 1 << 12
		}
	} else if n.focused {
		states |= 1 << 1
	} // active frame
	return []uint32{states, 0}
}
func (a accessible) GetState(m dbus.Message) ([]uint32, *dbus.Error) {
	return statesFor(a.b.node(m)), nil
}
func (a accessible) GetChildren(m dbus.Message) ([]reference, *dbus.Error) {
	children := a.b.node(m).children
	ans := make([]reference, len(children))
	for i, child := range children {
		ans[i] = a.b.ref(child)
	}
	return ans, nil
}
func (a accessible) GetChildAtIndex(m dbus.Message, idx int32) (reference, *dbus.Error) {
	children := a.b.node(m).children
	if idx >= 0 && int(idx) < len(children) {
		return a.b.ref(children[idx]), nil
	}
	return a.b.ref(null), nil
}
func (a accessible) GetIndexInParent(m dbus.Message) (int32, *dbus.Error) {
	path := incomingPath(m)
	a.b.mu.RLock()
	defer a.b.mu.RUnlock()
	return int32(slices.Index(a.b.nodes[a.b.nodes[path].parent].children, path)), nil
}
func (a accessible) GetApplication() (reference, *dbus.Error) { return a.b.ref(root), nil }
func interfacesFor(role uint32) []string {
	ans := []string{prefix + "Accessible"}
	switch role {
	case 75:
		ans = append(ans, prefix+"Application")
	case 23:
		ans = append(ans, prefix+"Component")
	case 60:
		ans = append(ans, prefix+"Component", prefix+"Text")
	}
	return ans
}
func (a accessible) GetInterfaces(m dbus.Message) ([]string, *dbus.Error) {
	return interfacesFor(a.b.node(m).role), nil
}
func (a accessible) GetAttributes() (map[string]string, *dbus.Error) { return map[string]string{}, nil }

type relation struct {
	Kind    uint32
	Targets []reference
}

func (a accessible) GetRelationSet() ([]relation, *dbus.Error) { return []relation{}, nil }

type text struct{ b *bridge }

func sliceText(runes []rune, start, end int32) string {
	length := int32(len(runes))
	if end == -1 {
		end = length
	}
	if start < 0 || start > end || end > length {
		return ""
	}
	return string(runes[start:end])
}

func (t text) GetText(m dbus.Message, start, end int32) (string, *dbus.Error) {
	return sliceText(t.b.node(m).runes, start, end), nil
}
func (t text) GetCharacterAtOffset(m dbus.Message, offset int32) (int32, *dbus.Error) {
	r := t.b.node(m).runes
	if offset >= 0 && int(offset) < len(r) {
		return int32(r[offset]), nil
	}
	return 0, nil
}
func textBounds(runes []rune, offset int32, granularity uint32) (int32, int32) {
	if offset < 0 || int(offset) >= len(runes) {
		return offset, offset
	}
	if granularity == 0 {
		return offset, offset + 1
	}
	isBoundary := func(i int) bool {
		if i <= 0 || i >= len(runes) {
			return true
		}
		switch granularity {
		case 1:
			return !unicode.IsSpace(runes[i]) && (unicode.IsSpace(runes[i-1]) || unicode.IsPunct(runes[i-1]))
		case 2:
			return !unicode.IsSpace(runes[i]) && (runes[i-1] == '\n' || strings.ContainsRune(".!?", runes[i-1]) ||
				(i > 1 && unicode.IsSpace(runes[i-1]) && strings.ContainsRune(".!?", runes[i-2])))
		default:
			return runes[i-1] == '\n'
		}
	}
	first, last := int(offset), int(offset)+1
	for !isBoundary(first) {
		first--
	}
	for !isBoundary(last) {
		last++
	}
	return int32(first), int32(last)
}
func (t text) GetStringAtOffset(m dbus.Message, offset int32, granularity uint32) (string, int32, int32, *dbus.Error) {
	r := t.b.node(m).runes
	first, last := textBounds(r, offset, granularity)
	return sliceText(r, first, last), first, last, nil
}
func boundaryGranularity(boundary uint32) uint32 {
	if boundary > 0 {
		return (boundary + 1) / 2
	}
	return 0
}
func (t text) GetTextAtOffset(m dbus.Message, offset int32, boundary uint32) (string, int32, int32, *dbus.Error) {
	return t.GetStringAtOffset(m, offset, boundaryGranularity(boundary))
}
func (t text) GetTextBeforeOffset(m dbus.Message, offset int32, boundary uint32) (string, int32, int32, *dbus.Error) {
	granularity := boundaryGranularity(boundary)
	first, _ := textBounds(t.b.node(m).runes, offset, granularity)
	return t.GetStringAtOffset(m, first-1, granularity)
}
func (t text) GetTextAfterOffset(m dbus.Message, offset int32, boundary uint32) (string, int32, int32, *dbus.Error) {
	granularity := boundaryGranularity(boundary)
	_, last := textBounds(t.b.node(m).runes, offset, granularity)
	return t.GetStringAtOffset(m, last, granularity)
}
func (t text) GetNSelections(m dbus.Message) (int32, *dbus.Error) {
	if p := t.b.node(m).pane; p != nil {
		return int32(len(p.Selections)), nil
	}
	return 0, nil
}
func (t text) GetSelection(m dbus.Message, i int32) (int32, int32, *dbus.Error) {
	if p := t.b.node(m).pane; p != nil && i >= 0 && int(i) < len(p.Selections) {
		return p.Selections[i][0], p.Selections[i][1], nil
	}
	return -1, -1, nil
}
func (t text) SetCaretOffset(int32) (bool, *dbus.Error)             { return false, nil }
func (t text) AddSelection(int32, int32) (bool, *dbus.Error)        { return false, nil }
func (t text) RemoveSelection(int32) (bool, *dbus.Error)            { return false, nil }
func (t text) SetSelection(int32, int32, int32) (bool, *dbus.Error) { return false, nil }
func (t text) GetDefaultAttributes() (map[string]string, *dbus.Error) {
	return map[string]string{}, nil
}
func (t text) GetDefaultAttributeSet() (map[string]string, *dbus.Error) {
	return map[string]string{}, nil
}
func (t text) GetAttributeRun(m dbus.Message, offset int32, _ bool) (map[string]string, int32, int32, *dbus.Error) {
	n := t.b.node(m)
	first, last := int32(0), int32(len(n.runes))
	if n.pane != nil {
		for _, role := range n.pane.Roles {
			if offset >= role.Start && offset < role.End {
				return map[string]string{"silkie-role": role.Name}, role.Start, role.End, nil
			}
			if offset < role.Start && role.Start < last {
				last = role.Start
			}
			if offset >= role.End && role.End > first {
				first = role.End
			}
		}
	}
	return map[string]string{}, first, last, nil
}
func (t text) GetAttributes(m dbus.Message, offset int32) (map[string]string, int32, int32, *dbus.Error) {
	return t.GetAttributeRun(m, offset, false)
}
func (t text) GetAttributeValue(m dbus.Message, offset int32, name string) (string, *dbus.Error) {
	attributes, _, _, _ := t.GetAttributeRun(m, offset, false)
	return attributes[name], nil
}

type rectangle struct{ X, Y, Width, Height int32 }

func frame(n node, coords uint32) rectangle {
	if n.pane == nil {
		return rectangle{}
	}
	r := rectangle{n.pane.Frame[0], n.pane.Frame[1], n.pane.Frame[2], n.pane.Frame[3]}
	if coords == 0 {
		r.X += n.origin[0]
		r.Y += n.origin[1]
	}
	return r
}

// Physical cells may have disjoint ranges in the spoken text.
func mappedRangeRect(n node, start, end int32, coords uint32) rectangle {
	f := frame(n, coords)
	var result rectangle
	found := false
	for y, row := range n.pane.Cells {
		for x, span := range row {
			if span[1] <= start || span[0] >= end || span[0] == span[1] {
				continue
			}
			left := f.X + int32(int64(x)*int64(f.Width)/int64(len(row)))
			top := f.Y + int32(int64(y)*int64(f.Height)/int64(len(n.pane.Cells)))
			right := f.X + int32(int64(x+1)*int64(f.Width)/int64(len(row)))
			bottom := f.Y + int32(int64(y+1)*int64(f.Height)/int64(len(n.pane.Cells)))
			if found {
				right = max(right, result.X+result.Width)
				bottom = max(bottom, result.Y+result.Height)
				left = min(left, result.X)
				top = min(top, result.Y)
			}
			found = true
			result = rectangle{left, top, right - left, bottom - top}
		}
	}
	return result
}

func characterRect(n node, offset int32, coords uint32) rectangle {
	if n.pane == nil || len(n.pane.Rows) == 0 || offset < 0 || int(offset) > len(n.runes) {
		return rectangle{}
	}
	if len(n.pane.Cells) != 0 {
		return mappedRangeRect(n, offset, offset+1, coords)
	}
	r := frame(n, coords)
	y := 0
	for y+1 < len(n.pane.Rows) && n.pane.Rows[y+1][0] <= offset {
		y++
	}
	cols := n.pane.Rows[y]
	if len(cols) <= 1 {
		return rectangle{}
	}
	x := 0
	for x+1 < len(cols)-1 && cols[x+1] <= offset {
		x++
	}
	rowCount, colCount := int32(len(n.pane.Rows)), int32(len(cols)-1)
	left, top := int32(x)*r.Width/colCount, int32(y)*r.Height/rowCount
	right, bottom := int32(x+1)*r.Width/colCount, int32(y+1)*r.Height/rowCount
	return rectangle{r.X + left, r.Y + top, right - left, bottom - top}
}
func (t text) GetCharacterExtents(m dbus.Message, offset int32, coords uint32) (int32, int32, int32, int32, *dbus.Error) {
	r := characterRect(t.b.node(m), offset, coords)
	return r.X, r.Y, r.Width, r.Height, nil
}
func (t text) GetRangeExtents(m dbus.Message, start, end int32, coords uint32) (int32, int32, int32, int32, *dbus.Error) {
	n := t.b.node(m)
	if start < 0 || start >= end || int(end) > len(n.runes) {
		return 0, 0, 0, 0, nil
	}
	if n.pane != nil && len(n.pane.Cells) != 0 {
		r := mappedRangeRect(n, start, end, coords)
		return r.X, r.Y, r.Width, r.Height, nil
	}
	a, z := characterRect(n, start, coords), characterRect(n, end-1, coords)
	if a.Y != z.Y {
		f := frame(n, coords)
		return f.X, a.Y, f.Width, z.Y + z.Height - a.Y, nil
	}
	return a.X, a.Y, z.X + z.Width - a.X, a.Height, nil
}
func (t text) GetOffsetAtPoint(m dbus.Message, x, y int32, coords uint32) (int32, *dbus.Error) {
	n := t.b.node(m)
	f := frame(n, coords)
	x, y = x-f.X, y-f.Y
	if x < 0 || y < 0 || x >= f.Width || y >= f.Height || n.pane == nil || len(n.pane.Rows) == 0 {
		return -1, nil
	}
	if len(n.pane.Cells) != 0 {
		row := n.pane.Cells[int(y)*len(n.pane.Cells)/int(f.Height)]
		return row[int(x)*len(row)/int(f.Width)][0], nil
	}
	row := n.pane.Rows[int(y)*len(n.pane.Rows)/int(f.Height)]
	return row[int(x)*(len(row)-1)/int(f.Width)], nil
}

type component struct{ b *bridge }

func (c component) GetExtents(m dbus.Message, coords uint32) (rectangle, *dbus.Error) {
	return frame(c.b.node(m), coords), nil
}
func (c component) GetPosition(m dbus.Message, coords uint32) (int32, int32, *dbus.Error) {
	r := frame(c.b.node(m), coords)
	return r.X, r.Y, nil
}
func (c component) GetSize(m dbus.Message) (int32, int32, *dbus.Error) {
	r := frame(c.b.node(m), 1)
	return r.Width, r.Height, nil
}
func (c component) Contains(m dbus.Message, x, y int32, coords uint32) (bool, *dbus.Error) {
	r := frame(c.b.node(m), coords)
	return x >= r.X && y >= r.Y && x-r.X < r.Width && y-r.Y < r.Height, nil
}
func (c component) GrabFocus() (bool, *dbus.Error)     { return false, nil }
func (c component) GetLayer() (uint32, *dbus.Error)    { return 3, nil } // widget
func (c component) GetMDIZOrder() (int16, *dbus.Error) { return 0, nil }
func (c component) GetAlpha() (float64, *dbus.Error)   { return 1, nil }
func (c component) GetAccessibleAtPoint(m dbus.Message, x, y int32, coords uint32) (reference, *dbus.Error) {
	c.b.mu.RLock()
	defer c.b.mu.RUnlock()
	for _, child := range c.b.nodes[incomingPath(m)].children {
		r := frame(c.b.nodes[child], coords)
		if x >= r.X && y >= r.Y && x-r.X < r.Width && y-r.Y < r.Height {
			return c.b.ref(child), nil
		}
	}
	return c.b.ref(null), nil
}

type application struct{}

func (application) GetLocale(uint32) (string, *dbus.Error) { return os.Getenv("LANG"), nil }

type cacheItem struct {
	Object, Application, Parent reference
	Index, Children             int32
	Interfaces                  []string
	Name                        string
	Role                        uint32
	Description                 string
	States                      []uint32
}

func (b *bridge) cacheItem(path dbus.ObjectPath, n node) cacheItem {
	parent := b.ref(n.parent)
	if path == root {
		parent = b.desktop
	}
	return cacheItem{
		Object: b.ref(path), Application: b.ref(root), Parent: parent,
		Index: int32(slices.Index(b.nodes[n.parent].children, path)), Children: int32(len(n.children)),
		Interfaces: interfacesFor(n.role), Name: n.title, Role: n.role, States: statesFor(n),
	}
}

type cache struct{ b *bridge }

func (c cache) GetItems() ([]cacheItem, *dbus.Error) {
	c.b.mu.RLock()
	defer c.b.mu.RUnlock()
	ans := make([]cacheItem, 0, len(c.b.nodes))
	for path, n := range c.b.nodes {
		ans = append(ans, c.b.cacheItem(path, n))
	}
	return ans, nil
}

type props struct{ b *bridge }

func (p props) GetAll(m dbus.Message, name string) (map[string]dbus.Variant, *dbus.Error) {
	n := p.b.node(m)
	v := dbus.MakeVariant
	ans := map[string]dbus.Variant{"version": v(uint32(1))}
	switch name {
	case prefix + "Accessible":
		parent := p.b.ref(n.parent)
		if incomingPath(m) == root {
			p.b.mu.RLock()
			parent = p.b.desktop
			p.b.mu.RUnlock()
		}
		ans["Name"], ans["Description"], ans["Parent"] = v(n.title), v(""), v(parent)
		ans["ChildCount"], ans["Locale"], ans["AccessibleId"] = v(int32(len(n.children))), v(os.Getenv("LANG")), v(string(incomingPath(m)))
	case prefix + "Text":
		caret := int32(-1)
		if n.pane != nil {
			caret = n.pane.Caret
		}
		ans["CaretOffset"], ans["CharacterCount"] = v(caret), v(int32(len(n.runes)))
	case prefix + "Application":
		p.b.mu.RLock()
		id := p.b.id
		p.b.mu.RUnlock()
		ans["Id"], ans["ToolkitName"], ans["Version"], ans["AtspiVersion"] = v(id), v("Silkie"), v("1"), v("2.1")
	}
	return ans, nil
}

func (p props) Get(m dbus.Message, name, key string) (dbus.Variant, *dbus.Error) {
	ans, _ := p.GetAll(m, name)
	if val, ok := ans[key]; ok {
		return val, nil
	}
	return dbus.Variant{}, dbus.NewError(properties+".UnknownProperty", []any{key})
}

func (p props) Set(m dbus.Message, name, key string, value dbus.Variant) *dbus.Error {
	if id, ok := value.Value().(int32); ok && incomingPath(m) == root && name == prefix+"Application" && key == "Id" {
		p.b.mu.Lock()
		p.b.id = id
		p.b.mu.Unlock()
		return nil
	}
	return dbus.NewError(properties+".PropertyReadOnly", []any{key})
}

func (b *bridge) export() error {
	if err := b.conn.Export(cache{b}, cachePath, prefix+"Cache"); err != nil {
		return err
	}
	ifaces := map[string]any{
		prefix + "Accessible": accessible{b}, prefix + "Text": text{b}, prefix + "Component": component{b},
		prefix + "Application": application{}, properties: props{b},
	}
	var info []introspect.Interface
	for name, object := range ifaces {
		if err := b.conn.ExportSubtree(object, root, name); err != nil {
			return err
		}
		info = append(info, introspect.Interface{Name: name, Methods: introspect.Methods(object)})
	}
	return b.conn.ExportSubtree(introspect.NewIntrospectable(&introspect.Node{Interfaces: info}), root, "org.freedesktop.DBus.Introspectable")
}

func Run(input io.Reader, output io.Writer) error {
	address := os.Getenv("AT_SPI_BUS_ADDRESS")
	if address == "" {
		session, err := dbus.ConnectSessionBus()
		if err != nil {
			return nil
		}
		defer session.Close()
		desktopBus := session.Object("org.a11y.Bus", "/org/a11y/bus")
		for {
			value, err := desktopBus.GetProperty("org.a11y.Status.IsEnabled")
			enabled, known := value.Value().(bool)
			if err != nil || !known || enabled {
				break
			}
			// Orca can be launched after Silkie. Keep this wait in the
			// helper, so no terminal text is collected until AT is enabled.
			time.Sleep(time.Second)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		err = desktopBus.CallWithContext(ctx, "org.a11y.Bus.GetAddress", 0).Store(&address)
		cancel()
		if err != nil || address == "" {
			return nil
		}
	}
	conn, err := dbus.Connect(address)
	if err != nil {
		return err
	}
	defer conn.Close()
	b := bridge{conn: conn, bus: conn.Names()[0], desktop: reference{"", null},
		nodes: map[dbus.ObjectPath]node{root: {title: "Silkie", role: 75}}}
	if err = b.export(); err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	var desktop reference
	err = conn.Object(prefix+"Registry", root).CallWithContext(ctx, prefix+"Socket.Embed", 0, b.ref(root)).Store(&desktop)
	cancel()
	if err != nil {
		return err
	}
	b.mu.Lock()
	b.desktop = desktop
	b.mu.Unlock()
	if _, err := fmt.Fprintln(output, "ready"); err != nil {
		return err
	}

	messages := json.NewDecoder(input)
	for {
		var u update
		if err := messages.Decode(&u); err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			return err
		}
		if u.Windows != nil {
			b.update(*u.Windows)
		}
		if a := u.Announcement; a != nil {
			priority := int32(1)
			if a.Assertive {
				priority = 2
			}
			path := panePath(a.Pane)
			if _, present := b.nodes[path]; !present {
				path = root
			}
			b.signal(path, "Event.Object.Announcement", "", priority, 0, a.Text)
		}
	}
}
