// Selection coordinates refer to absolute scrollback rows, not viewport rows.
export class TextSelection {
    constructor(snapshot) { this.snapshot = snapshot; this.clear(); }
    clear() { this.anchor = this.end = null; this.dragged = false; }
    start(line, column) { this.anchor = {line, column}; this.end = {...this.anchor}; this.dragged = false; }
    extend(line, column) {
        if (!this.anchor) return;
        this.end = {line, column};
        this.dragged ||= line !== this.anchor.line || column !== this.anchor.column;
    }
    bounds() {
        if (!this.dragged) return null;
        const a = this.anchor, b = this.end;
        return a.line < b.line || (a.line === b.line && a.column <= b.column) ? [a, b] : [b, a];
    }
    contains(line, column) {
        const bounds = this.bounds(); if (!bounds) return false;
        const [a, b] = bounds;
        return line >= a.line && line <= b.line &&
            (line !== a.line || column >= a.column) && (line !== b.line || column <= b.column);
    }
    selectAll() {
        const frame = this.snapshot(0);
        this.start(0, 0); this.extend(frame[7] + frame[1] - 1, frame[0] - 1);
        this.dragged = true;
    }
    text() {
        const bounds = this.bounds(); if (!bounds) return '';
        const [a, b] = bounds, current = this.snapshot(0), history = current[7];
        const lines = [];
        let frame = null, first = -1;
        for (let row = a.line; row <= b.line; row++) {
            if (!frame || row < first || row >= first + frame[1]) {
                const offset = Math.max(0, history - row);
                frame = this.snapshot(offset); first = history - offset;
            }
            const cells = frame[6][row - first]; if (!cells) break;
            const left = row === a.line ? a.column : 0;
            const right = row === b.line ? b.column + 1 : cells.length;
            lines.push(cells.slice(left, right).map(cell => cell[0]).join('').replace(/ +$/, ''));
        }
        return lines.join('\n');
    }
}
