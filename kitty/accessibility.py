from collections.abc import Iterable

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
