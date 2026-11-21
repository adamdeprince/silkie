from collections.abc import Iterable
from dataclasses import dataclass
from typing import TYPE_CHECKING, TypedDict

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
    for y in range(screen.lines):
        cells = list(raw_cells(screen.visual_line(y), 0, screen.columns))
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
    return {'text': '\n'.join(lines), 'rows': rows, 'caret': caret, 'selections': selections, 'roles': roles}
