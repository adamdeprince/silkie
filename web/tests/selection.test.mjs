import assert from 'node:assert/strict';
import test from 'node:test';
import {TextSelection} from '../selection.mjs';

function selection(lines, rows = 3) {
    const history = lines.length - rows;
    return new TextSelection(offset => [8, rows, 0, 0, true, false,
        lines.slice(history - offset, history - offset + rows).map(line => [...line.padEnd(8)].map(text => [text])), history]);
}
test('selection copies in either direction, preserving Unicode and trimming screen padding', () => {
    const text = selection(['hello', 'café', 'third']);
    text.start(0, 1); text.extend(1, 3);
    assert.equal(text.text(), 'ello\ncafé');
    text.start(1, 3); text.extend(0, 1);
    assert.equal(text.text(), 'ello\ncafé');
    assert(text.contains(1, 2)); assert(!text.contains(1, 5));
});
test('scrollback selection spans all retained rows without an arbitrary selection limit', () => {
    const lines = Array.from({length: 20000}, (_, i) => String(i));
    const text = selection(lines); text.selectAll();
    assert.equal(text.text(), lines.join('\n'));
    text.clear(); assert.equal(text.text(), '');
});
