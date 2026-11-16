#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

"""
The bridge between the terminal and its native user interface: Cocoa on macOS and GTK 3 on
Linux. The native code renders the plain data built here and reports what the user did;
all decisions are taken here so that the two front ends behave alike.
"""

from collections.abc import Iterable
from typing import TYPE_CHECKING, Any

from .constants import is_macos
from .fast_data_types import add_timer, get_boss, get_options, native_settings_show, native_settings_update, native_tabs_supported, remove_timer
from .profiles import (
    DEFAULT_PROFILE_ID,
    create_profile,
    delete_profile,
    load_profiles,
    profile_model,
    reset_profile_value,
    set_profile_value,
)
from .settings_model import (
    atomic_write_text,
    key_text,
    monospace_families,
    read_layer,
    reset_global_value,
    set_global_value,
    settings_conf_path,
    settings_pages,
    write_layer,
)

if TYPE_CHECKING:
    from .types import SingleKey


def native_tabs_active() -> bool:
    ' True when tabs are shown by the operating system. Fixed at startup, like the choice of window system. '
    return bool(get_options().native_tabs and native_tabs_supported())


# Menus {{{
def shortcuts_by_action() -> dict[str, 'SingleKey']:
    ans: dict[str, 'SingleKey'] = {}
    for sk, defs in get_options().keyboard_modes[''].keymap.items():
        d = defs[-1]
        if d.definition and d.is_suitable_for_global_shortcut:
            ans.setdefault(' '.join(d.definition.split()), sk)
    return ans


def menu_item(shortcuts: dict[str, 'SingleKey'], title: str, action: str, shortcut_action: str = '', when: str = 'always') -> dict[str, Any]:
    ans: dict[str, Any] = {'title': title, 'action': action, 'when': when}
    if (sk := shortcuts.get(shortcut_action or action)) is not None:
        ans['key'], ans['mods'] = sk.key, sk.mods
        ans['shortcut'] = key_text(sk)
    return ans


SEPARATOR: dict[str, Any] = {'separator': True}


def profile_items(shortcuts: dict[str, 'SingleKey']) -> list[dict[str, Any]]:
    profiles = load_profiles()
    default = get_options().default_profile
    def titled(p: Any) -> str:
        return f'{p.name} (default)' if p.id == default else p.name

    def group(action: str) -> list[dict[str, Any]]:
        return [menu_item(shortcuts, titled(p), f'{action} {p.id}') for p in profiles.values()]

    return [
        {'title': 'New Tab with Profile', 'items': group('new_tab_with_profile')},
        {'title': 'New Window with Profile', 'items': group('new_os_window_with_profile')},
        {'title': 'Change Profile of This Window', 'items': group('set_window_profile')},
        SEPARATOR,
        menu_item(shortcuts, 'Open Profiles…', 'open_settings profiles'),
    ]


def view_items(s: dict[str, 'SingleKey']) -> list[dict[str, Any]]:
    spatial: list[dict[str, Any]] = []
    if is_macos:
        from .spatial import Presentation, controller, requires_xreal
        window = get_boss().active_window
        current = controller.windows.get(window.os_window_id if window else 0, Presentation(output=get_options().spatial_output))
        items = []
        for mode, title in (('off', 'Off'), ('follow', 'Follow Head'), ('anchored', 'Anchored Wall')):
            items.append({**menu_item(s, title, f'set_spatial_mode {mode}', when='window'), 'checked': current.mode == mode,
                          'enabled': not requires_xreal(mode, current.output) or get_options().experimental_xreal_1s})
        items.append(SEPARATOR)
        for output, title in (('sbs', 'Side-by-Side (XREAL)'), ('red-blue', 'Red/Blue Glasses')):
            items.append({**menu_item(s, title, f'set_spatial_output {output}', when='window'), 'checked': current.output == output,
                          'enabled': output != 'sbs' or get_options().experimental_xreal_1s})
        items.extend([SEPARATOR, menu_item(s, 'Recenter', 'recenter_spatial', when='window'),
                      menu_item(s, 'Spatial Display Settings…', 'open_settings spatial')])
        if not get_options().experimental_xreal_1s:
            items.append(menu_item(s, 'XREAL 1S: Experimental and untested…', 'open_settings experimental'))
        if current.mode == 'anchored':
            status = 'Tracking: 3DoF' if current.tracking else 'Tracking unavailable — wall held at center'
            items.extend([SEPARATOR, {'title': status, 'enabled': False}])
        spatial = [{'title': 'Spatial Display', 'items': items}, SEPARATOR]
    return [
        *spatial,
        menu_item(s, 'Make Text Bigger', 'change_font_size all +2.0', when='window'),
        menu_item(s, 'Make Text Smaller', 'change_font_size all -2.0', when='window'),
        menu_item(s, 'Actual Size', 'change_font_size all 0', when='window'),
        SEPARATOR,
        menu_item(s, 'Next Layout', 'next_layout', when='window'),
        SEPARATOR,
        menu_item(s, 'Show Scrollback', 'show_scrollback', when='window'),
        menu_item(s, 'Show Last Command Output', 'show_last_command_output', when='window'),
    ]


def native_menu_model() -> list[dict[str, Any]]:
    ' The menu bar. On macOS the application, Shell, Edit, Window and Help menus are built by the Cocoa code. '
    s = shortcuts_by_action()
    ans = [{'title': 'View', 'items': view_items(s)}, {'title': 'Profiles', 'items': profile_items(s)}]
    if is_macos:
        return ans
    return [
        {'title': 'Shell', 'items': [
            menu_item(s, 'New Tab', 'new_tab'),
            menu_item(s, 'New Window', 'new_os_window'),
            menu_item(s, 'Split Pane', 'new_window'),
            menu_item(s, 'Split Pane Vertically', 'split_pane_vertically', when='window'),
            SEPARATOR,
            menu_item(s, 'Close Pane', 'close_window', when='window'),
            menu_item(s, 'Close Tab', 'close_tab', when='window'),
            menu_item(s, 'Close Window', 'close_os_window', when='window'),
            SEPARATOR,
            menu_item(s, 'Reset', 'clear_terminal reset active', when='window'),
            SEPARATOR,
            menu_item(s, 'Settings…', 'open_settings'),
            menu_item(s, 'Edit Config File…', 'edit_config_file'),
            menu_item(s, 'Reload Config', 'load_config_file'),
            SEPARATOR,
            menu_item(s, 'Quit', 'quit'),
        ]},
        {'title': 'Edit', 'items': [
            menu_item(s, 'Copy', 'copy_to_clipboard', when='selection'),
            menu_item(s, 'Paste', 'paste_from_clipboard', when='window'),
            SEPARATOR,
            menu_item(s, 'Find', 'search_scrollback', when='window'),
            SEPARATOR,
            menu_item(s, 'Clear Screen', 'clear_terminal to_cursor active', when='window'),
            menu_item(s, 'Clear Scrollback', 'clear_terminal scrollback active', when='window'),
        ]},
        *ans,
        {'title': 'Window', 'items': [
            menu_item(s, 'Previous Tab', 'previous_tab', when='window'),
            menu_item(s, 'Next Tab', 'next_tab', when='window'),
            menu_item(s, 'Move Tab Backward', 'move_tab_backward', when='window'),
            menu_item(s, 'Move Tab Forward', 'move_tab_forward', when='window'),
            menu_item(s, 'Move Tab to New Window', 'detach_tab', when='window'),
            SEPARATOR,
            menu_item(s, 'Toggle Full Screen', 'toggle_fullscreen', when='window'),
            menu_item(s, 'Toggle Maximized', 'toggle_maximized', when='window'),
        ]},
        {'title': 'Help', 'items': [menu_item(s, 'Online Documentation', 'open_url https://silkie.goblinreactor.com/')]},
    ]
# }}}


# Settings window {{{
class SettingsController:
    reload_timer: int = 0

    def model(self, page: str = '') -> dict[str, Any]:
        pages = settings_pages()
        pages.insert(2, profile_model())
        return {
            'pages': pages,
            'families': monospace_families(),
            'select_page': page,
            'notice': self.notice(),
            'default_profile': get_options().default_profile,
        }

    def notice(self) -> str:
        if settings_conf_path() not in get_options().all_config_paths:
            return 'This terminal was started with --config. Changes apply until it is closed, and are read again only by terminals started without it.'
        return ''

    def show(self, page: str = '') -> None:
        native_settings_show(self.model(page), self.on_event)

    def refresh(self) -> None:
        native_settings_update(self.model())

    def schedule_reload(self) -> None:
        # Typing in a field reports every change, do not reload the config for each of them
        if self.reload_timer:
            remove_timer(self.reload_timer)
        self.reload_timer = add_timer(self.do_reload, 0.3, False)

    def do_reload(self, timer_id: int | None = None) -> None:
        self.reload_timer = 0
        paths = tuple(get_options().all_config_paths)
        if settings_conf_path() not in paths:
            paths += (settings_conf_path(),)
        get_boss().load_config_file(*paths)
        update_native_menus()

    def on_event(self, verb: str, *args: str) -> str:
        ' Called by the native code. The result is an error message to show the user, empty if there is none. '
        handler = getattr(self, f'on_{verb}', None)
        if handler is None:
            return f'Unknown request: {verb}'
        return str(handler(*args) or '')

    def profile(self, profile_id: str) -> Any:
        return load_profiles().get(profile_id)

    def on_set(self, scope: str, name: str, value: str) -> str:
        if scope == 'global':
            err = set_global_value(name, value)
        else:
            p = self.profile(scope.partition(':')[2])
            if p is None:
                return 'This profile no longer exists'
            err = set_profile_value(p, name, value)
            if name == 'name' and not err:
                self.refresh()
                update_native_menus()
                return ''
        if not err:
            self.schedule_reload()
        return err

    def on_reset(self, scope: str, name: str) -> None:
        if scope == 'global':
            reset_global_value(name)
        elif (p := self.profile(scope.partition(':')[2])) is not None:
            reset_profile_value(p, name)
        self.schedule_reload()
        self.refresh()

    def on_adjust(self, name: str, value: str, direction: str) -> str:
        from .settings_model import stepped_value
        return stepped_value(name, value, int(direction))

    def on_profile_new(self, name: str = 'New Profile') -> None:
        create_profile(name or 'New Profile')
        self.refresh()
        update_native_menus()

    def on_profile_duplicate(self, profile_id: str) -> None:
        if (p := self.profile(profile_id)) is not None:
            create_profile(f'{p.name} Copy', copy_of=p)
            self.refresh()
            update_native_menus()

    def on_profile_delete(self, profile_id: str) -> str:
        if not delete_profile(profile_id):
            return 'The default profile cannot be deleted'
        if get_options().default_profile == profile_id:
            set_default_profile(DEFAULT_PROFILE_ID)
        self.schedule_reload()
        self.refresh()
        update_native_menus()
        return ''

    def on_profile_set_default(self, profile_id: str) -> None:
        set_default_profile(profile_id)
        get_options().default_profile = profile_id
        self.schedule_reload()
        self.refresh()
        update_native_menus()

    def on_profile_open(self, profile_id: str, where: str = 'tab') -> None:
        boss = get_boss()
        if where == 'tab':
            boss.new_tab_with_profile(profile_id)
        else:
            boss.new_os_window_with_profile(profile_id)


def set_default_profile(profile_id: str) -> None:
    path = settings_conf_path()
    layer = read_layer(path)
    layer['default_profile'] = [profile_id]
    write_layer(path, layer)


settings_controller = SettingsController()


def open_settings(page: str = '') -> None:
    settings_controller.show(page)


def update_native_menus() -> None:
    from .fast_data_types import native_menu_update

    native_menu_update(native_menu_model())


__all__: Iterable[str] = ('atomic_write_text', 'native_tabs_active', 'open_settings', 'update_native_menus', 'native_menu_model')
# }}}
