# License: GPLv3

import os
import re
import shlex
from collections.abc import Mapping
from ctypes import CFUNCTYPE, c_char_p, c_int, c_size_t, c_void_p, string_at
from pathlib import Path

from . import fast_data_types as fdt
from .constants import is_macos
from .magnification import Gio


def language_tag(locale: str) -> str:
    # POSIX locales can include a codeset or an @modifier. Preserve the
    # language, script and territory where they have a language-tag spelling.
    main, _, modifier = locale.strip().partition('@')
    parts = main.split('.', 1)[0].replace('_', '-').split('-')
    if not re.fullmatch(r'[a-zA-Z]{2,8}', parts[0]) or parts[0].lower() == 'posix':
        return ''
    if not all(re.fullmatch(r'[a-zA-Z0-9]{1,8}', component) for component in parts[1:]):
        return ''
    script = {'latin': 'Latn', 'cyrillic': 'Cyrl'}.get(modifier.lower())
    if script and (len(parts) == 1 or len(parts[1]) != 4):
        parts.insert(1, script)
    return '-'.join([parts[0].lower(), *(
        p.title() if len(p) == 4 and p.isalpha() else p.upper() if len(p) == 2 and p.isalpha() else p.lower()
        for p in parts[1:]
    )])


def primary_language(variables: Mapping[str, str]) -> str:
    for key in ('LANGUAGE', 'LC_ALL', 'LC_MESSAGES', 'LANG'):
        if tag := language_tag(variables.get(key, '').partition(':')[0]):
            return tag
    return ''


def ukui_appearance(style: str, gtk_theme: str) -> tuple[str, bool]:
    style, gtk_theme = style.lower(), gtk_theme.lower()
    if gtk_theme == 'highcontrastinverse':
        return 'dark', True
    if gtk_theme == 'highcontrast':
        return 'light', True
    if style == 'ukui-dark' or (style in ('', 'ukui-default') and gtk_theme == 'ukui-black'):
        return 'dark', False
    if style == 'ukui-light' or (style in ('', 'ukui-default') and gtk_theme == 'ukui-white'):
        return 'light', False
    return 'no_preference', False


def locale_files() -> dict[str, str]:
    for path in ('/etc/locale.conf', '/etc/default/locale'):
        try:
            text = Path(path).read_text()
        except (OSError, UnicodeError):
            continue
        values: dict[str, str] = {}
        for line in text.splitlines():
            key, equals, value = line.partition('=')
            if equals and key.strip() in ('LANGUAGE', 'LC_ALL', 'LC_MESSAGES', 'LANG'):
                try:
                    words = shlex.split(value, comments=True)
                except ValueError:
                    continue
                if words:
                    values[key.strip()] = words[0]
        if values:
            return values
    return {}


class LinuxPreferences:
    def __init__(self, gio: Gio) -> None:
        self.gio = gio
        self.proxies: dict[bytes, int] = {}
        self.callbacks = []
        self.settings: list[tuple[int, bytes]] = []
        self.closed = False
        self.object_unref = gio.function('g_object_unref', None, c_void_p)
        self.unref = gio.function('g_variant_unref', None, c_void_p)
        self.free = gio.function('g_free', None, c_void_p)
        self.property = gio.function('g_dbus_proxy_get_cached_property', c_void_p, c_void_p, c_char_p)
        self.string = gio.function('g_variant_get_string', c_char_p, c_void_p, c_void_p)
        self.child = gio.function('g_variant_get_child_value', c_void_p, c_void_p, c_size_t)
        self.count = gio.function('g_variant_n_children', c_size_t, c_void_p)
        self.get_setting = gio.function('g_settings_get_string', c_void_p, c_void_p, c_char_p)
        self.cancel = gio.function('g_cancellable_cancel', None, c_void_p)
        self.cancellable = gio.function('g_cancellable_new', c_void_p)()

        ready = CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)
        create = gio.function('g_dbus_proxy_new_for_bus', None, c_int, c_int, c_void_p, c_char_p,
                              c_char_p, c_char_p, c_void_p, ready, c_void_p)
        finish = gio.function('g_dbus_proxy_new_for_bus_finish', c_void_p, c_void_p, c_void_p)

        def add_proxy(name: bytes, path: bytes, interface: bytes) -> None:
            @ready
            def created(_source, result, _data):
                if proxy := finish(result, None):
                    if self.closed:
                        self.object_unref(proxy)
                    else:
                        self.proxies[interface] = proxy
            self.callbacks.append(created)
            create(1, 0, None, name, path, interface, self.cancellable, created, None)  # System bus

        with gio:
            add_proxy(b'org.freedesktop.Accounts', f'/org/freedesktop/Accounts/User{os.getuid()}'.encode(),
                      b'org.freedesktop.Accounts.User')
            add_proxy(b'org.freedesktop.locale1', b'/org/freedesktop/locale1', b'org.freedesktop.locale1')

            desktops = (os.environ.get('XDG_CURRENT_DESKTOP') or os.environ.get('DESKTOP_SESSION', '')).lower().split(':')
            if any(desktop.startswith(('ukui', 'kylin')) for desktop in desktops):
                self.add_setting(b'org.ukui.style', b'style-name')
                self.add_setting(b'org.mate.interface', b'gtk-theme')

    def add_setting(self, name: bytes, key: bytes) -> None:
        gio = self.gio
        source = gio.function('g_settings_schema_source_get_default', c_void_p)()
        if not source:
            return
        lookup = gio.function('g_settings_schema_source_lookup', c_void_p, c_void_p, c_char_p, c_int)
        if schema := lookup(source, name, 1):
            try:
                if gio.function('g_settings_schema_has_key', c_int, c_void_p, c_char_p)(schema, key):
                    make = gio.function('g_settings_new_full', c_void_p, c_void_p, c_void_p, c_char_p)
                    if setting := make(schema, None, None):
                        self.settings.append((setting, key))
            finally:
                gio.function('g_settings_schema_unref', None, c_void_p)(schema)

    def preferred_language(self) -> str:
        if user := self.proxies.get(b'org.freedesktop.Accounts.User'):
            if value := self.property(user, b'Language'):
                try:
                    if tag := language_tag(self.string(value, None).decode()):
                        return tag
                finally:
                    self.unref(value)
        if system := self.proxies.get(b'org.freedesktop.locale1'):
            if value := self.property(system, b'Locale'):
                variables: dict[str, str] = {}
                try:
                    for i in range(self.count(value)):
                        child = self.child(value, i)
                        try:
                            key, equals, entry = self.string(child, None).decode().partition('=')
                            if equals:
                                variables[key] = entry
                        finally:
                            self.unref(child)
                finally:
                    self.unref(value)
                if tag := primary_language(variables):
                    return tag
        return primary_language(locale_files()) or primary_language(os.environ)

    def appearance(self) -> tuple[str, bool]:
        values: dict[bytes, str] = {}
        for setting, key in self.settings:
            if value := self.get_setting(setting, key):
                try:
                    values[key] = string_at(value).decode()
                finally:
                    self.free(value)
        return ukui_appearance(values.get(b'style-name', ''), values.get(b'gtk-theme', ''))

    def close(self) -> None:
        self.closed = True
        self.cancel(self.cancellable)
        self.gio.dispatch()
        for proxy in self.proxies.values():
            self.object_unref(proxy)
        for setting, _ in self.settings:
            self.object_unref(setting)
        self.object_unref(self.cancellable)


class DesktopPreferences:
    def __init__(self) -> None:
        self.linux = None
        self.last_appearance = ('no_preference', False)
        if not is_macos:
            try:
                self.linux = LinuxPreferences(Gio())
            except (OSError, AttributeError):
                pass
        self.sync()
        self.timer = fdt.add_timer(self.sync, 0.5)

    def sync(self, _timer: int | None = None) -> None:
        if is_macos:
            tags = (language_tag(value) for value in fdt.cocoa_preferred_languages())
            preferred = ','.join(dict.fromkeys(filter(None, tags)))
        elif self.linux is not None:
            self.linux.gio.dispatch()
            appearance = self.linux.appearance()
            if appearance != self.last_appearance:
                fdt.set_desktop_appearance_fallback(*appearance)
                self.last_appearance = appearance
            preferred = self.linux.preferred_language()
        else:
            preferred = primary_language(locale_files()) or primary_language(os.environ)
        fdt.set_desktop_preferred_languages(preferred or 'und')

    def close(self) -> None:
        fdt.remove_timer(self.timer)
        if self.linux is not None:
            self.linux.close()
