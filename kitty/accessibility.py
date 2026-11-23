from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from typing import TYPE_CHECKING, NotRequired, TypedDict

if TYPE_CHECKING:
    from .fast_data_types import Line, Screen

# The row and column are relative to the viewport. The internal IDs identify
# the placement even when it has no protocol IDs or spans several placeholders.
ImageDescription = tuple[float, float, int, int, int, int, tuple[str, str, str, str] | None]

PURPOSE_LABELS = {
    'essential': 'Essential image',
    'informative': 'Informative image',
    'decorative': 'Decorative image',
    '': 'Unclassified image',
}


def image_review_text(images: Iterable[ImageDescription], include_decorative: bool = False) -> str:
    entries: list[str] = []
    seen: set[tuple[int, int]] = set()
    for row, column, image, placement, _, _, annotation in sorted(images, key=lambda item: item[:4]):
        key = image, placement
        if key in seen:
            continue
        seen.add(key)
        purpose, name, description, details = annotation or ('', '', '', '')
        if purpose == 'decorative' and not include_decorative:
            continue
        kind = PURPOSE_LABELS[purpose]
        position = f'{kind}, row {int(row) + 1}, column {int(column) + 1}.'
        entry = [name, position, description]
        if details:
            entry.extend(('Details', details))
        entries.append('\n'.join(part for part in entry if part))
    if entries:
        return '\n\n'.join(entries)
    return 'No images are visible.' if include_decorative else 'No non-decorative images are visible.'


@dataclass(frozen=True, slots=True)
class TextAnnotation:
    start: tuple[int, int]
    end: tuple[int, int]
    role: str
    label: str
    source: tuple[tuple[str, ...], ...]


def raw_cells(line: 'Line', first: int, last: int) -> tuple[str, ...]:
    return tuple(line.accessible_cell(x) for x in range(first, last))


class TextAnnotations:
    def __init__(self) -> None:
        self.ranges: dict[int, TextAnnotation] = {}
        self.grid: tuple[int, int, int, int] | None = None

    def prune(self, screen: 'Screen') -> None:
        grid = screen.lines, screen.columns, screen.alternate_screen_entered_at, screen.alternate_screen_exited_at
        if self.grid != grid:
            self.ranges.clear()
            self.grid = grid
        for key, annotation in tuple(self.ranges.items()):
            if annotation.source != self.cells(screen, annotation.start, annotation.end):
                del self.ranges[key]

    @staticmethod
    def cells(screen: 'Screen', start: tuple[int, int], end: tuple[int, int]) -> tuple[tuple[str, ...], ...]:
        return tuple(
            raw_cells(screen.line(y), start[1] if y == start[0] else 0, end[1] if y == end[0] else screen.columns)
            for y in range(start[0], end[0] + 1)
        )

    def set(self, screen: 'Screen', key: int, start: tuple[int, int], end: tuple[int, int], role: str, label: str) -> str:
        if (not (0 <= start[0] <= end[0] < screen.lines) or
                not (0 <= start[1] < screen.columns and 0 <= end[1] <= screen.columns) or end <= start):
            return 'EINVAL'
        if start[0] < end[0] and end[1] == 0:
            end = end[0] - 1, screen.columns
        self.prune(screen)
        source = self.cells(screen, start, end)
        for other, annotation in tuple(self.ranges.items()):
            if annotation.start < end and start < annotation.end:
                del self.ranges[other]
        self.ranges[key] = TextAnnotation(start, end, role, label, source)
        return 'OK'

    def clear(self, key: int) -> str:
        return 'OK' if self.ranges.pop(key, None) is not None else 'ENOENT'


class PaneText(TypedDict):
    text: str
    rows: list[list[int]]
    cells: NotRequired[list[list[tuple[int, int]]]]
    caret: int
    selections: list[tuple[int, int]]
    roles: list[tuple[int, int, str]]


def pane_text(screen: 'Screen', annotations: TextAnnotations | None = None) -> PaneText:
    by_row: dict[int, list[TextAnnotation]] = {}
    if annotations is not None:
        annotations.prune(screen)
        for annotation in sorted(annotations.ranges.values(), key=lambda item: item.start):
            first, last = annotation.start[0] + screen.scrolled_by, annotation.end[0] + screen.scrolled_by
            for y in range(max(0, first), min(screen.lines, last + 1)):
                by_row.setdefault(y, []).append(annotation)

    rows: list[list[int]] = []
    lines: list[str] = []
    roles: list[tuple[int, int, str]] = []
    offset = 0
    directions = {}
    for y in range(screen.lines):
        line = screen.visual_line(y)
        if hints := line.reading_directions():
            directions[y] = hints
        cells = list(raw_cells(line, 0, screen.columns))
        last = len(cells)
        while last and cells[last - 1] == '\0':
            last -= 1
        tab_remaining = 0
        for i, cell in enumerate(cells):
            if cell.startswith('\t'):
                tab_remaining = ord(cell[1]) if len(cell) > 1 else 0
                cells[i] = '\t'
            elif tab_remaining and cell == ' ':
                tab_remaining -= 1
                cells[i] = ''
            else:
                tab_remaining = 0
                if cell == '\0':
                    cells[i] = ' ' if i < last else ''

        local_roles: list[tuple[int, int, str]] = []
        buffer_row = y - screen.scrolled_by
        for annotation in by_row.get(y, ()):
            first = annotation.start[1] if buffer_row == annotation.start[0] else 0
            end = annotation.end[1] if buffer_row == annotation.end[0] else len(cells)
            if annotation.label or annotation.role == 'decorative':
                cells[first:end] = [''] * (end - first)
                if annotation.role != 'decorative' and buffer_row == annotation.start[0]:
                    cells[first] = annotation.label
            if annotation.role != 'decorative':
                local_roles.append((first, end, annotation.role))

        columns = [offset]
        for cell in cells:
            offset += len(cell)
            columns.append(offset)
        for first, last, role in local_roles:
            if columns[first] < columns[last]:
                roles.append((columns[first], columns[last], role))
        rows.append(columns)
        lines.append(''.join(cells))
        offset += 1  # Row separator in the exposed text

    caret_row = screen.cursor.y + screen.scrolled_by
    caret = -1
    if screen.cursor_visible and 0 <= caret_row < len(rows):
        caret = rows[caret_row][min(screen.cursor.x, screen.columns)]
    selections: list[tuple[int, int]] = []
    for y, first, last in screen.accessible_selection():
        start, end = rows[y][first], rows[y][last]
        if start != end:
            if selections and first == 0 and selections[-1][1] + 1 == start:
                selections[-1] = selections[-1][0], end
            else:
                selections.append((start, end))
    snapshot: PaneText = {'text': '\n'.join(lines), 'rows': rows, 'caret': caret, 'selections': selections, 'roles': roles}
    if directions or screen.flow:
        apply_reading_order(snapshot, screen, directions)
    return snapshot


def apply_reading_order(snapshot: PaneText, screen: 'Screen', directions: dict[int, bytes]) -> None:
    rows, cols, flow = snapshot['rows'], screen.columns, screen.flow
    nrows = len(rows)
    text = snapshot['text']
    # Character offsets for logical cells. Continuations have zero length.
    offsets = [(row[x], row[x + 1]) for row in rows for x in range(cols)]
    owners = {}
    blank = set()
    for y in range(nrows):
        line = screen.visual_line(y)
        for x in range(cols):
            raw = line.accessible_cell(x)
            if raw == '':
                dx, dy = line.cell_tile_offset(x)
                if (dx or dy) and dx <= x and dy <= y:
                    owners[y * cols + x] = (y - dy) * cols + x - dx
            elif directions and raw == '\0':
                blank.add(y * cols + x)
    if directions:
        steps = ((1, 0), (-1, 0), (0, 1), (0, -1))
        following: dict[int, int | None] = {}
        for y, hints in directions.items():
            for x, hint in enumerate(hints):
                if not hint or rows[y][x] == rows[y][x + 1]:
                    continue
                if hint == 5:  # Explicit end of the accessible text
                    following[y * cols + x] = None
                    continue
                dx, dy = steps[hint - 1]
                if flow & 4:
                    dx, dy = dy, dx
                if flow & 1:
                    dx = -dx
                if flow & 2:
                    dy = -dy
                u, v = x + dx, y + dy
                while 0 <= u < cols and 0 <= v < nrows:
                    target = owners.get(v * cols + u, v * cols + u)
                    if target != y * cols + x:
                        if offsets[target][0] < offsets[target][1] and target not in blank:
                            following[y * cols + x] = target
                        break
                    u, v = u + dx, v + dy

        # Start at heads in logical order, then at cycles without a head.
        # A directed edge back to any emitted character ends the entire read.
        incoming = set(following.values())
        visited: set[int] = set()

        def reading_order() -> Iterator[int]:
            for cycles in (False, True):
                for i, (start, end) in enumerate(offsets):
                    if start == end or i in visited or (not cycles and i in incoming):
                        continue
                    while True:
                        visited.add(i)
                        yield i
                        if i not in following:
                            break
                        target = following[i]
                        if target is None or target in visited:
                            return
                        i = target

        pieces: list[str] = []
        spoken_rows: list[list[int]] = []
        mapped = offsets.copy()
        pos = 0
        previous = -1
        for i in reading_order():
            adjacent = i == previous + 1 and i // cols == previous // cols
            if not adjacent and following.get(previous) != i:
                if pieces:
                    pieces.append('\n')
                    pos += 1
                spoken_rows.append([pos])
            start, end = offsets[i]
            pieces.append(text[start:end])
            mapped[i] = pos, pos + end - start
            pos += end - start
            spoken_rows[-1].append(pos)
            previous = i
        # Empty and continuation cells point to the nearest insertion point in
        # their own line. This also preserves a caret at the wrap boundary.
        caret_row = screen.cursor.y + screen.scrolled_by
        for y in range(nrows):
            boundary = next((mapped[i][0] for i in range(y * cols, (y + 1) * cols) if i in visited), pos)
            for i in range(y * cols, (y + 1) * cols):
                if i not in visited:
                    if offsets[i][0] != offsets[i][1]:
                        boundary = pos  # A character beyond the end of the read
                    mapped[i] = boundary, boundary
                boundary = mapped[i][1]
            if y == caret_row and snapshot['caret'] >= 0:
                x = screen.cursor.x
                snapshot['caret'] = mapped[y * cols + x][0] if x < cols else boundary

        def remap(start: int, end: int) -> list[tuple[int, int]]:
            from bisect import bisect_left
            i = bisect_left(offsets, (start, -1))  # ranges start at cell boundaries
            parts = []
            while i < len(mapped) and offsets[i][0] < end:
                if i in visited:
                    parts.append(mapped[i])
                i += 1
            merged: list[tuple[int, int]] = []
            for a, b in sorted(parts):
                if merged and merged[-1][1] == a:
                    merged[-1] = merged[-1][0], b
                else:
                    merged.append((a, b))
            return merged

        snapshot['roles'] = [(a, b, role) for start, end, role in snapshot['roles'] for a, b in remap(start, end)]
        snapshot['selections'] = sorted(part for start, end in snapshot['selections'] for part in remap(start, end))
        snapshot['text'] = ''.join(pieces)
        snapshot['rows'] = spoken_rows
        offsets = mapped

    # Native accessibility APIs need physical hit rectangles even when
    # successive offsets in the spoken string are in different columns.
    cells = [[(0, 0)] * (nrows if flow & 4 else cols) for _ in range(cols if flow & 4 else nrows)]
    for y in range(nrows):
        for x in range(cols):
            u = cols - 1 - x if flow & 1 else x
            v = nrows - 1 - y if flow & 2 else y
            if flow & 4:
                u, v = v, u
            cells[v][u] = offsets[owners.get(y * cols + x, y * cols + x)]
    snapshot['cells'] = cells
