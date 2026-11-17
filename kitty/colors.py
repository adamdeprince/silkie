#!/usr/bin/env python
# License: GPLv3 Copyright: 2024, Kovid Goyal <kovid at kovidgoyal.net>

import os
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Literal, Optional, TypedDict, cast

from .config import parse_config
from .constants import config_dir, config_dir_files_ignored
from .fast_data_types import (
    COLOR_LAYER_GLOBAL,
    COLOR_LAYER_OVERRIDE,
    COLOR_LAYER_SESSION,
    Color,
    ColorProfile,
    get_boss,
    get_options,
    glfw_get_system_color_theme,
    patch_color_profiles,
    patch_global_colors,
    set_os_window_chrome,
)
from .options.types import Options, all_colors, nullable_colors, special_colors
from .rgb import color_as_int, color_from_int
from .types import run_once
from .typing_compat import WindowType

ColorsSpec = dict[str, Optional[int]]
TransparentBackgroundColors = tuple[tuple[Color, float], ...]
ColorSchemes = Literal['light', 'dark', 'no_preference']
Colors = tuple[ColorsSpec, TransparentBackgroundColors]
# None for the transparent background colors means they were not specified and
# should be left unchanged, as opposed to () which means clear them.
ParsedColors = tuple[ColorsSpec, TransparentBackgroundColors | None]


def color_profile_signature(profile: ColorProfile) -> dict[str, Any]:
    # Compare effective colors, rather than whether the application has an override.
    colors = {k: v for k, v in profile.as_dict().items() if k.startswith('color') or k == 'transparent_background_colors'}
    colors.update(profile.basic_colors())
    for attr in ('cursor_color', 'cursor_text_color', 'highlight_fg', 'highlight_bg', 'visual_bell_color'):
        colors[attr] = getattr(profile, attr)
    return colors


def scheme_colors(scheme: str) -> ColorsSpec:
    'Black on white for the light scheme, white on black for the dark one'
    fg, bg = (0, 0xFFFFFF) if scheme == 'light' else (0xFFFFFF, 0)
    return {
        'foreground': fg,
        'background': bg,
        'cursor': fg,
        'cursor_text_color': bg,
        'selection_foreground': bg,
        'selection_background': fg,
        'active_tab_foreground': bg,
        'active_tab_background': fg,
        'inactive_tab_foreground': fg,
        'inactive_tab_background': bg,
        'tab_bar_background': bg,
        'tab_bar_margin_color': bg,
    }


def color_scheme_spec(opts: Options) -> ColorsSpec | None:
    """
    The colors forced by the color_scheme option, or None when the colors
    follow the system color scheme. E-ink mode is per OS window, see
    eink_color_spec().
    """
    scheme = opts.color_scheme
    if scheme == 'auto':
        return None
    return scheme_colors(scheme)


def eink_color_spec(opts: Options) -> ColorsSpec:
    """
    The colors of an OS window on a display that shows only gray: black on
    white, or white on black with an explicit color_scheme dark. They are
    shown over every color layer, including the colors of windows, sessions
    and programs, which come back when the window moves to another display.
    """
    return scheme_colors('dark' if opts.color_scheme == 'dark' else 'light')


def tab_bar_spec_from_options(opts: Options) -> ColorsSpec:
    'The colors of the tab bar from the options, as they are without e-ink colors'
    return {
        'active_tab_foreground': color_as_int(opts.active_tab_foreground),
        'active_tab_background': color_as_int(opts.active_tab_background),
        'inactive_tab_foreground': color_as_int(opts.inactive_tab_foreground),
        'inactive_tab_background': color_as_int(opts.inactive_tab_background),
        'tab_bar_background': color_as_int(opts.tab_bar_background) if opts.tab_bar_background else None,
    }


def sync_eink_colors(os_window_ids: Iterable[int] | None = None) -> None:
    """
    Show the colors of a display that shows only gray in the OS windows on one
    and the ordinary colors in the others, see os_window_eink_state(). Only
    colors that change are redrawn and told to programs.
    """
    from .fast_data_types import os_window_eink_state, set_os_window_eink_chrome
    boss = get_boss()
    opts = get_options()
    spec = eink_color_spec(opts)
    wanted = None if os_window_ids is None else frozenset(os_window_ids)
    states: dict[int, bool] = {}

    def is_gray(os_window_id: int) -> bool:
        if (ans := states.get(os_window_id)) is None:
            ans = states[os_window_id] = os_window_eink_state(os_window_id)[1]
        return ans

    for w in tuple(boss.all_windows):
        os_window_id = getattr(w, 'os_window_id', 0)
        if wanted is not None and os_window_id not in wanted:
            continue
        cp = w.screen.color_profile
        bg_before = cp.default_bg
        if cp.set_eink_colors(spec if is_gray(os_window_id) else None):
            if cp.default_bg != bg_before:
                boss.default_bg_changed_for(w.id)
            w.notify_child_of_color_change()
            w.refresh()
    for tm in tuple(boss.all_tab_managers):
        if wanted is not None and tm.os_window_id not in wanted:
            continue
        gray = is_gray(tm.os_window_id)
        tm.tab_bar.patch_colors(spec if gray else tab_bar_spec_from_options(opts))
        tm.tab_bar.layout()
        tm.mark_tab_bar_dirty()
        if (t := tm.active_tab) is not None:
            t.relayout_borders()
        set_os_window_eink_chrome(tm.os_window_id, spec if gray else None)


class BackgroundImageOptions(TypedDict, total=False):
    background_image: Sequence[str] | None
    background_image_layout: str | None
    background_image_linear: bool | None
    background_tint: float | None
    background_tint_gaps: float | None


class ThemeFile(Enum):
    dark = 'dark-theme.auto.conf'
    light = 'light-theme.auto.conf'
    no_preference = 'no-preference-theme.auto.conf'


theme_file_schemes: dict[str, ColorSchemes] = {ThemeFile.dark.value: 'dark', ThemeFile.light.value: 'light', ThemeFile.no_preference.value: 'no_preference'}
# The built-in light theme, a file in the kitty package, see the automatic_light_theme option
builtin_light_theme_name = 'builtin-light-theme.conf'


@run_once
def builtin_light_theme_text() -> str:
    from .constants import read_kitty_resource

    return read_kitty_resource(builtin_light_theme_name).decode('utf-8')


def configured_colors_are_default(opts: Options) -> bool:
    'Whether the colors in opts, as loaded from kitty.conf, are all the default ones'
    from kitty.options.types import defaults

    return all(getattr(opts, name) == getattr(defaults, name) for name in all_colors)


@dataclass
class AutoTheme:
    # mtime is -1 when there is no theme file for this color scheme
    mtime: int = -1
    spec: ColorsSpec = field(default_factory=dict)
    tbc: TransparentBackgroundColors = ()
    background_image_options: BackgroundImageOptions = field(default_factory=lambda: BackgroundImageOptions())


class ThemeColors:
    applied_theme: Literal['light', 'dark', 'no_preference', ''] = ''
    # Whether the applied theme is the built-in light theme rather than a theme file
    applied_builtin_theme: bool = False
    # Whether the built-in light theme may be used, set from the options loaded
    # from kitty.conf by update_builtin_light_theme_allowed()
    builtin_light_theme_allowed: bool = False
    default_colors: ColorsSpec | None = None
    default_background_image_options: BackgroundImageOptions | None = None
    # The colors loaded from the config files, before any theme or color_scheme
    # was applied, restored when the color_scheme option goes back to auto
    configured_colors: Colors | None = None

    def __init__(self) -> None:
        self.themes: dict[ColorSchemes, AutoTheme] = {'dark': AutoTheme(), 'light': AutoTheme(), 'no_preference': AutoTheme()}
        self.builtin_light_theme: ColorsSpec | None = None

    def update_builtin_light_theme_allowed(self, opts: Options) -> None:
        # The built-in light theme is a replacement for the default colors, so
        # it is not used when kitty.conf sets colors of its own, for instance by
        # including a theme. opts must not have had any theme applied to it.
        self.builtin_light_theme_allowed = opts.automatic_light_theme and configured_colors_are_default(opts)

    def get_builtin_light_theme(self) -> ColorsSpec:
        if self.builtin_light_theme is None:
            self.builtin_light_theme = self.parse_colors(builtin_light_theme_text().splitlines())[0]
        return self.builtin_light_theme

    def theme_for(self, scheme: ColorSchemes) -> AutoTheme | None:
        """
        The theme to use for the specified system color scheme, a theme file if
        present, otherwise the built-in light theme for the light scheme, when
        allowed. The built-in theme has an mtime of -1 and changes neither the
        transparent background colors nor the background image.
        """
        theme = self.themes[scheme]
        if theme.mtime > -1:
            return theme
        if scheme == 'light' and self.builtin_light_theme_allowed:
            return AutoTheme(spec=self.get_builtin_light_theme())
        return None

    def get_default_colors(self) -> ColorsSpec:
        if self.default_colors is None:
            from kitty.options.types import defaults, option_names

            ans: ColorsSpec = dict.fromkeys(nullable_colors)

            for name in option_names:
                defval = getattr(defaults, name)
                if isinstance(defval, Color):
                    ans[name] = int(defval)
            for name in special_colors:
                ans[name] = getattr(defaults, name)
            self.default_colors = ans
            self.default_background_image_options: BackgroundImageOptions = {k: getattr(defaults, k) for k in BackgroundImageOptions.__optional_keys__}  # type: ignore

        return self.default_colors

    def parse_colors(self, f: Iterable[str], background_image_options: BackgroundImageOptions | None = None) -> Colors:
        # When parsing the theme file we first apply the default theme so that
        # all colors are reset to default values first. This is needed for themes
        # that don't specify all colors.
        dc_spec = self.get_default_colors()
        if background_image_options is not None and self.default_background_image_options:
            background_image_options.update(self.default_background_image_options)
        spec, tbc = parse_colors((f,), background_image_options)
        ans = dc_spec.copy()
        ans.update(spec)
        return ans, (() if tbc is None else tbc)

    def refresh(self) -> bool:
        "Load new or modified theme files and forget deleted ones. Returns True if any theme changed."
        changed = False
        if config_dir_files_ignored():
            return changed
        present: set[ColorSchemes] = set()
        try:
            entries = list(os.scandir(config_dir))
        except FileNotFoundError:
            entries = []
        for x in entries:
            if (scheme := theme_file_schemes.get(x.name)) is None:
                continue
            theme = self.themes[scheme]
            try:
                mtime = x.stat().st_mtime_ns
                if mtime > theme.mtime:
                    bgo: BackgroundImageOptions = {}
                    with open(x.path) as f:
                        spec, tbc = self.parse_colors(f, bgo)
                    self.themes[scheme] = AutoTheme(mtime, spec, tbc, bgo)
                    changed = True
            except FileNotFoundError:
                continue  # deleted after the directory was scanned
            present.add(scheme)
        for scheme, theme in tuple(self.themes.items()):
            if theme.mtime > -1 and scheme not in present:
                self.themes[scheme] = AutoTheme()
                if self.applied_theme == scheme:
                    self.applied_theme = ''
                changed = True
        return changed

    @property
    def has_applied_theme(self) -> bool:
        return self.applied_theme != '' and (self.applied_builtin_theme or self.themes[self.applied_theme].mtime > -1)

    @property
    def has_theme_file(self) -> bool:
        return any(t.mtime > -1 for t in self.themes.values())

    @property
    def has_dark_theme(self) -> bool:
        return self.themes['dark'].mtime > -1

    @property
    def has_light_theme(self) -> bool:
        return self.themes['light'].mtime > -1

    @property
    def has_no_preference_theme(self) -> bool:
        return self.themes['no_preference'].mtime > -1

    def patch_opts(self, opts: Options, debug_rendering: bool = False) -> None:
        from .utils import log_error

        self.configured_colors = (
            {k: None if (v := getattr(opts, k)) is None else int(v) for k in all_colors},
            opts.transparent_background_colors,
        )
        self.applied_theme, self.applied_builtin_theme = '', False
        self.update_builtin_light_theme_allowed(opts)
        # color_scheme light or dark take precedence over the theme for the
        # system color scheme. They change only the colors they name, so the
        # transparent background colors are kept.
        if (spec := color_scheme_spec(opts)) is not None:
            patch_options_with_color_spec(opts, spec, None)
            patch_global_colors(spec, True)
            return
        if not self.has_theme_file and not self.builtin_light_theme_allowed:
            return  # no need to query the system
        if debug_rendering:
            log_error('Querying system for current color scheme')
        which = glfw_get_system_color_theme()
        if debug_rendering:
            log_error('Current system color scheme:', which)
        if (theme := self.theme_for(which)) is not None:
            is_builtin = theme.mtime < 0
            if is_builtin:
                patch_options_with_color_spec(opts, theme.spec, None)
            else:
                patch_options_with_color_spec(opts, theme.spec, theme.tbc, background_image_options=theme.background_image_options)
            patch_global_colors(theme.spec, True)
            self.applied_theme, self.applied_builtin_theme = which, is_builtin
            if debug_rendering:
                log_error(f'Applied {"built-in " if is_builtin else ""}{self.applied_theme} color theme')

    def on_system_color_scheme_change(self, new_value: ColorSchemes, is_initial_value: bool = False) -> bool:
        if is_initial_value:
            return False
        self.refresh()
        return self.apply_theme(new_value)

    def on_config_reload(self, opts_already_patched: bool = False) -> None:
        # opts_already_patched is True when patch_opts() was called with the
        # options as loaded, as Boss.apply_new_options() does, which decided
        # whether the built-in light theme may be used: the options in effect
        # then have a theme applied, so they cannot tell.
        # Reloading the config resets colors to those from kitty.conf, so apply the
        # theme for the current system color scheme, if any. Theme files are
        # always re-read so that ones created or deleted since the last refresh
        # are picked up.
        self.refresh()
        if not opts_already_patched:
            self.update_builtin_light_theme_allowed(get_options())
        # The colors are now those from the config files, nothing to remove
        self.applied_builtin_theme = False
        if not self.apply_theme(glfw_get_system_color_theme(), notify_on_bg_change=False):
            self.applied_theme = ''

    def apply_theme(self, new_value: ColorSchemes, notify_on_bg_change: bool = True) -> bool:
        from .utils import log_error

        if color_scheme_spec(get_options()) is not None:
            return False
        if (theme := self.theme_for(new_value)) is None:
            if not self.applied_builtin_theme:
                return False
            # Remove the built-in light theme, it is only used when kitty.conf has the
            # default colors, so restore those
            patch_colors(self.get_default_colors(), None, True, notify_on_bg_change=notify_on_bg_change)
            self.applied_theme, self.applied_builtin_theme = '', False
            if get_boss().args.debug_rendering:
                log_error(f'Removed the built-in light color theme for {new_value}')
            return True
        is_builtin = theme.mtime < 0
        if is_builtin:
            patch_colors(theme.spec, None, True, notify_on_bg_change=notify_on_bg_change)
        else:
            patch_colors(theme.spec, theme.tbc, True, notify_on_bg_change=notify_on_bg_change, background_image_options=theme.background_image_options)
        self.applied_theme, self.applied_builtin_theme = new_value, is_builtin
        if get_boss().args.debug_rendering:
            log_error(f'Applied {"built-in " if is_builtin else ""}color theme {new_value}')
        return True

    def apply_color_scheme(self) -> None:
        'Apply a change of the color_scheme option'
        opts = get_options()
        if (spec := color_scheme_spec(opts)) is not None:
            self.applied_theme, self.applied_builtin_theme = '', False
            patch_colors(spec, None, True)
        else:
            # Back to auto: the theme for the system color scheme, if any, otherwise the configured colors
            self.refresh()
            if not self.apply_theme(glfw_get_system_color_theme()) and self.configured_colors is not None:
                self.applied_theme, self.applied_builtin_theme = '', False
                patch_colors(*self.configured_colors, configured=True)
        # The colors of gray displays depend on the scheme
        sync_eink_colors()


theme_colors = ThemeColors()

# The colors that can be set by a session color scheme, these are the colors
# the ssh kitten sets via escape codes for a color_scheme
session_color_scheme_keys = frozenset(('foreground', 'background', 'cursor', 'selection_foreground', 'selection_background')) | frozenset(
    f'color{i}' for i in range(256)
)
color_schemes: dict[str, ColorSchemes] = {'dark': 'dark', 'light': 'light', 'no_preference': 'no_preference'}
ColorSchemeVariants = dict[ColorSchemes, ColorsSpec]


def parse_color_scheme_variants(data: object) -> ColorSchemeVariants:
    """
    Parse untrusted color scheme variants of the form: {scheme: {color_name: 0xRRGGBB or None}}
    where scheme is one of dark, light or no_preference. Raises ValueError if data is invalid.
    """
    if not isinstance(data, dict):
        raise ValueError('Color scheme variants must be a JSON object')
    ans: ColorSchemeVariants = {}
    for scheme, spec in data.items():
        if (cs := color_schemes.get(scheme)) is None:
            raise ValueError(f'Unknown color scheme: {scheme!r}')
        if not isinstance(spec, dict):
            raise ValueError(f'The colors for the {cs} color scheme must be a JSON object')
        colors: ColorsSpec = {}
        for name, val in spec.items():
            if name not in session_color_scheme_keys:
                raise ValueError(f'Unknown color in the {cs} color scheme: {name!r}')
            if val is None:
                if name not in nullable_colors:
                    raise ValueError(f'The color {name} in the {cs} color scheme cannot be null')
            elif isinstance(val, bool) or not isinstance(val, int) or not 0 <= val <= 0xFFFFFF:
                raise ValueError(f'The color {name} in the {cs} color scheme must be an integer from 0 to 0xffffff')
            colors[name] = val
        ans[cs] = colors
    return ans


class SessionColorSchemes:
    """
    The color schemes of sessions running in a window, such as from the ssh
    kitten. Each session has variants for the dark, light and no preference
    system color schemes and is associated with the depth of the color stack
    when it was set, it is removed when the color stack is popped below that
    depth. The variant matching the system color scheme of the innermost
    session is applied to the session color layer of the window.
    """

    def __init__(self) -> None:
        self.sessions: list[tuple[int, ColorSchemeVariants]] = []

    def __bool__(self) -> bool:
        return bool(self.sessions)

    def add(self, color_stack_depth: int, variants: ColorSchemeVariants) -> None:
        self.sessions = [x for x in self.sessions if x[0] < color_stack_depth]
        self.sessions.append((color_stack_depth, variants))

    def color_stack_popped(self, color_stack_depth: int) -> bool:
        before = len(self.sessions)
        self.sessions = [x for x in self.sessions if x[0] <= color_stack_depth]
        return len(self.sessions) != before

    def clear(self) -> bool:
        had_sessions = bool(self.sessions)
        self.sessions = []
        return had_sessions

    def copy_from(self, other: 'SessionColorSchemes') -> None:
        # The copy is not associated with any color stack entry in this window
        self.sessions = [(0, other.sessions[-1][1])] if other.sessions else []

    def apply(self, cp: ColorProfile, scheme: ColorSchemes) -> None:
        cp.reset_layer(COLOR_LAYER_SESSION)
        if self.sessions and (spec := self.sessions[-1][1].get(scheme)):
            patch_color_profiles(spec, None, (cp,), COLOR_LAYER_SESSION)


@run_once
def all_color_related_conf_keys() -> frozenset[str]:
    return all_colors | frozenset(BackgroundImageOptions.__optional_keys__) | frozenset({'transparent_background_colors'})


def parse_colors(
    args: Iterable[str | Iterable[str]],
    background_image_options: BackgroundImageOptions | None = None,
    allow_reading_conf_files: bool = True,
) -> ParsedColors:
    from kitty.options.parse import parse_conf_item

    colors: dict[str, Color | None | int] = {}
    nullable_color_map: dict[str, int | None] = {}
    special_color_map: dict[str, int] = {}
    transparent_background_colors: TransparentBackgroundColors | None = None
    allowed = all_color_related_conf_keys()
    for spec in args:
        conf: dict[str, Any] = {}
        if isinstance(spec, str):
            k, sep, v = spec.partition('=')
            if sep == '=':
                k, v = k.strip(), v.strip()
                if k in allowed:
                    parse_conf_item(k, v, conf)
            elif allow_reading_conf_files:
                with open(os.path.expanduser(spec), encoding='utf-8', errors='replace') as f:
                    conf = parse_config(f)
        elif allow_reading_conf_files:
            conf = parse_config(spec)
        transparent_background_colors = conf.pop('transparent_background_colors', transparent_background_colors)
        if background_image_options is not None:
            bio: dict[str, Any] = cast(dict[str, Any], background_image_options)
            for key in BackgroundImageOptions.__optional_keys__:
                if key in conf:
                    bio[key] = conf[key]
        colors.update(conf)
    for k in nullable_colors:
        q = colors.pop(k, False)
        if q is not False:
            val = int(q) if isinstance(q, Color) else None
            nullable_color_map[k] = val
    for k in special_colors:
        sq = colors.pop(k, None)
        if isinstance(sq, int):
            special_color_map[k] = sq
    ans: dict[str, int | None] = {k: int(v) for k, v in colors.items() if isinstance(v, Color)}
    ans.update(nullable_color_map)
    ans.update(special_color_map)
    return ans, transparent_background_colors


def patch_options_with_color_spec(
    opts: Options,
    spec: ColorsSpec,
    transparent_background_colors: TransparentBackgroundColors | None,
    background_image_options: BackgroundImageOptions | None = None,
) -> None:
    for k, v in spec.items():
        if hasattr(opts, k):
            if v is None:
                if k in nullable_colors:
                    setattr(opts, k, None)
            else:
                if k in special_colors:
                    setattr(opts, k, v)
                else:
                    setattr(opts, k, color_from_int(v))
    if transparent_background_colors is not None:
        opts.transparent_background_colors = transparent_background_colors
    if background_image_options is not None:
        for k, bv in background_image_options.items():
            if hasattr(opts, k):
                setattr(opts, k, bv)


def patch_colors(
    spec: ColorsSpec,
    transparent_background_colors: TransparentBackgroundColors | None = None,
    configured: bool = False,
    windows: Sequence[WindowType] | None = None,
    notify_on_bg_change: bool = True,
    background_image_options: BackgroundImageOptions | None = None,
    replace_window_colors: bool = False,
) -> None:
    # When configured is True the global configured colors are changed, colors
    # set in individual windows, by launch --color or by programs running in
    # them, are kept unless replace_window_colors is True. When configured is
    # False, the colors are set as if by the programs running in the windows.
    boss = get_boss()
    opts = get_options()
    if windows is None:
        windows = tuple(boss.all_windows)
    bg_colors_before = {w.id: w.screen.color_profile.default_bg for w in windows}
    colors_before = {w.id: color_profile_signature(w.screen.color_profile) for w in windows if w}
    profiles = tuple(w.screen.color_profile for w in windows if w)
    patch_color_profiles(spec, transparent_background_colors, profiles, COLOR_LAYER_GLOBAL if configured else COLOR_LAYER_OVERRIDE, replace_window_colors)
    if configured:
        patch_options_with_color_spec(opts, spec, transparent_background_colors, background_image_options)
    os_window_ids = set()
    for tm in get_boss().all_tab_managers:
        tm.tab_bar.patch_colors(spec)
        tm.tab_bar.layout()
        tm.mark_tab_bar_dirty()
        t = tm.active_tab
        if t is not None:
            t.relayout_borders()
        os_window_ids.add(tm.os_window_id)
    patch_global_colors(spec, configured)  # changes macos_titlebar_color
    for oswid in os_window_ids:
        set_os_window_chrome(oswid)
    default_bg_changed = 'background' in spec
    notify_bg = notify_on_bg_change and default_bg_changed
    boss = get_boss()
    if background_image_options is not None:
        bg = background_image_options.get('background_image')
        boss.set_background_image(
            bg[0] if bg else None,
            tuple(os_window_ids),
            configured,
            layout=background_image_options.get('background_image_layout'),
            linear_interpolation=background_image_options.get('background_image_linear'),
            tint=background_image_options.get('background_tint'),
            tint_gaps=background_image_options.get('background_tint_gaps'),
        )
    for w in windows:
        if w:
            if notify_bg and w.screen.color_profile.default_bg != bg_colors_before.get(w.id):
                boss.default_bg_changed_for(w.id)
            # Configuration reloads defer notifications until all profile/theme layers are applied.
            if notify_on_bg_change and color_profile_signature(w.screen.color_profile) != colors_before[w.id]:
                w.notify_child_of_color_change()
            w.refresh()
    # The tab bars of OS windows on gray displays keep the colors of such displays
    sync_eink_colors(os_window_ids)
