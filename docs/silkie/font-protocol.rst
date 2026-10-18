Font selection protocol
=======================

Silkie extends kitty with font-family, weight and slant selection for terminal
text. These attributes are retained with the cells through scrolling, resizing,
screen copies and ANSI export. Text remains selectable and searchable. The
terminal grid, character widths and cursor advances are unchanged.

Silkie uses Iosevka by default, with Iosevka Slab for the ``serif`` role.
Install the desired families from `Iosevka's releases
<https://github.com/be5invis/Iosevka/releases>`_. Fonts are selected from the
local system; the protocol does not transmit font files. If a family is absent,
normal platform font substitution applies.

For inherited configuration, font features and variable axes, see `kitty's
font manual <https://sw.kovidgoyal.net/kitty/kittens/choose-fonts/>`_. Text size
uses `kitty's text sizing protocol
<https://sw.kovidgoyal.net/kitty/text-sizing-protocol/>`_, independently of this
extension.

Discovery
---------

The :doc:`capabilities-protocol` lists ``font.1``. A program queries its
capabilities using the shared :doc:`extension-framing`::

    <OSC>777;font;v=1:op=query:id=42<ST>

The reply lists configured roles and accepted numeric ranges::

    <OSC>777;font;v=1:op=cap:id=42:caps=1:roles=default,sans,serif:weight_min=100:weight_max=900:slant_min=-1500:slant_max=1500:synthetic_slant=1<ST>

``caps=1`` means selection is supported. Ranges describe requests the terminal
accepts; installed fonts can have fewer weights, in which case the nearest
available weight is used. ``synthetic_slant=1`` identifies renderer synthesis.
The list of roles can change when the user reloads configuration.

Selecting a font
----------------

Selection applies to subsequent text::

    <OSC>777;font;v=1:op=select:role=serif:weight=300:slant=0:italic=1<ST>

Each omitted property retains its current value. The properties are:

``role``
    ``default`` selects ``font_family``. Other names select user-configured
    ``font_role`` entries. Names begin with an ASCII letter and contain ASCII
    letters, digits, underscores, periods or hyphens. An unknown role selects
    ``default``. There is no fixed limit on role names or the number of roles.

``weight``
    An integer from ``100`` to ``900``: Thin, Extra Light, Light, Regular,
    Medium, Semibold, Bold, Extra Bold and Heavy at the multiples of 100.
    Intermediate requests use a variable font's weight axis when available,
    otherwise the nearest static weight. ``0`` restores selection through
    the configured face and the ordinary SGR bold attribute. An explicit
    nonzero weight takes precedence over SGR bold until it is reset.

``slant``
    An integer from ``-1500`` to ``1500``, in hundredths of a degree, using
    OpenType's sign convention. Negative values lean right, positive values
    lean left, and ``0`` is upright. ``auto`` restores the selected face's
    native slant. Synthesis preserves roman or italic letterforms; selecting
    ``italic=1:slant=0`` produces upright italic. Slant does not change cell
    advances. Overhang at a cell or window boundary can be clipped, as with
    ordinary italic text.

``italic``
    ``1`` selects italic letterforms and ``0`` selects roman letterforms.
    This is the same attribute controlled by SGR 3 and SGR 23. It remains
    independent of an explicitly selected slant angle.

Restore the default role, weight and native slant with::

    <OSC>777;font;v=1:op=reset<ST>

This leaves ordinary SGR attributes, including italic, unchanged. SGR 0 and
RIS reset all font-selection attributes. Cursor save/restore and alternate
screen cursor restoration include the selected style. These are text
attributes rather than reporting modes, so ``caps;op=reset`` does not reset
them.

``op=state:id=42`` reads back the requested role, weight and slant using
``op=state`` in the reply, along with ``italic``. Requests are validated
atomically. Unknown fields
are ignored; duplicate keys, malformed fields, unsupported versions and
out-of-range values reject the entire request. Storage grows with the input;
the configured ``escape_limit`` still applies to OSC sequences.

Legacy font slots
-----------------

SGR 10 selects the default role. SGR 11 through 19 select the first nine
configured roles in configuration order. An unassigned slot selects the
default role. These aliases preserve weight and slant, and do not restrict
the number of roles selectable through OSC 777.

CJK text
--------

CJK glyphs use ``cjk_font`` and retain their normal cell widths. With ``auto``,
Silkie prefers the regional `Sarasa Term
<https://github.com/be5invis/Sarasa-Gothic>`_ family, which combines Iosevka
with Source Han Sans, then Noto Sans CJK and Noto Sans Mono CJK, then available
regional system faces and platform glyph fallback. Sarasa and Noto can have
different weight coverage from Iosevka; requests use their available weights.
The same signed slant transformation applies to preferred CJK faces.

``cjk_font_locale`` selects Japanese (``jp``), Korean (``kr``), Simplified
Chinese (``sc``), Traditional Chinese (``tc``) or Hong Kong (``hk``) glyph forms.
``auto`` uses the process locale, defaulting to ``sc`` when unspecified.
Kana prefer Japanese faces and Hangul prefer Korean faces automatically.
Font selection cannot infer the language of a shared Han character from its
code point. Configure the region when the process locale is insufficient.
User ``symbol_map`` entries take precedence; emoji presentation and the
terminal's geometric box drawing keep their inherited behavior.

Example configuration::

    font_family Iosevka
    font_role sans Iosevka
    font_role serif Iosevka Slab
    cjk_font auto
    cjk_font_locale jp

To choose an installed CJK family explicitly::

    cjk_font family="Noto Sans CJK JP"

Relays and multiplexers
-----------------------

A relay that passes bytes through can forward this extension. A multiplexer
that rebuilds the screen must retain and replay the font attributes and only
advertise ``font.1`` when it can do so. Applications query support and retain
ordinary text rendering as their fallback.
