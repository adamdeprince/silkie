from ctypes import CDLL, CFUNCTYPE, POINTER, c_char_p, c_double, c_int, c_size_t, c_void_p
from functools import cache
from math import isfinite
from os import environ
from time import monotonic
from typing import Callable

from .constants import is_macos


class Gio:
    def __init__(self) -> None:
        self.lib = CDLL('libgio-2.0.so.0')
        self.make_context = self.function('g_main_context_new', c_void_p)
        self.iterate = self.function('g_main_context_iteration', c_int, c_void_p, c_int)
        self.push = self.function('g_main_context_push_thread_default', None, c_void_p)
        self.pop = self.function('g_main_context_pop_thread_default', None, c_void_p)
        self.context = self.make_context()

    def function(self, name: str, result: type | None, *arguments: type):
        function = getattr(self.lib, name)
        function.restype = result
        function.argtypes = arguments
        return function

    def __enter__(self) -> None:
        self.push(self.context)

    def __exit__(self, *_: object) -> None:
        self.pop(self.context)

    def dispatch(self) -> None:
        while self.iterate(self.context, 0):
            pass


def gnome_reader(gio: Gio) -> Callable[[], float]:
    default_source = gio.function('g_settings_schema_source_get_default', c_void_p)
    lookup = gio.function('g_settings_schema_source_lookup', c_void_p, c_void_p, c_char_p, c_int)
    unref = gio.function('g_settings_schema_unref', None, c_void_p)
    create_settings = gio.function('g_settings_new_full', c_void_p, c_void_p, c_void_p, c_char_p)
    get_bool = gio.function('g_settings_get_boolean', c_int, c_void_p, c_char_p)
    get_factor = gio.function('g_settings_get_double', c_double, c_void_p, c_char_p)
    object_unref = gio.function('g_object_unref', None, c_void_p)
    settings = []
    source = default_source()
    if source:
        with gio:
            for name in (b'org.gnome.desktop.a11y.applications', b'org.gnome.desktop.a11y.magnifier'):
                schema = lookup(source, name, 1)
                if schema:
                    settings.append(create_settings(schema, None, None))
                    unref(schema)
    if len(settings) != 2:
        for setting in settings:
            object_unref(setting)
        return lambda: 1.
    applications, magnifier = settings

    def sample() -> float:
        gio.dispatch()
        return get_factor(magnifier, b'mag-factor') if get_bool(applications, b'screen-magnifier-enabled') else 1.

    return sample


def kwin_reader(gio: Gio) -> Callable[[], float]:
    ready = CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)
    bus_get = gio.function('g_bus_get', None, c_int, c_void_p, ready, c_void_p)
    bus_finish = gio.function('g_bus_get_finish', c_void_p, c_void_p, c_void_p)
    call = gio.function('g_dbus_connection_call', None, c_void_p, c_char_p, c_char_p, c_char_p, c_char_p,
                        c_void_p, c_void_p, c_int, c_int, c_void_p, ready, c_void_p)
    finish = gio.function('g_dbus_connection_call_finish', c_void_p, c_void_p, c_void_p, c_void_p)
    child = gio.function('g_variant_get_child_value', c_void_p, c_void_p, c_size_t)
    get_string = gio.function('g_variant_get_string', c_char_p, c_void_p, c_void_p)
    make_string = gio.function('g_variant_new_string', c_void_p, c_char_p)
    make_tuple = gio.function('g_variant_new_tuple', c_void_p, POINTER(c_void_p), c_size_t)
    unref = gio.function('g_variant_unref', None, c_void_p)
    result_type = gio.function('g_variant_type_new', c_void_p, c_char_p)(b'(s)')
    bus, pending, factor, next_query = None, True, 1., 0.
    reports_target = True

    @ready
    def on_bus(_source, result, _data):
        nonlocal bus, pending
        bus = bus_finish(result, None)
        pending = False

    @ready
    def on_information(source, result, _data):
        nonlocal pending, factor, reports_target
        pending = False
        factor = 1.
        value = finish(source, result, None)
        if value:
            payload = child(value, 0)
            info = get_string(payload, None)
            unref(payload)
            unref(value)
            for line in info.splitlines():
                name, _, number = line.partition(b':')
                if name == b'targetZoom':
                    try:
                        factor = float(number)
                    except ValueError:
                        pass
                    break
            else:
                # A loaded effect on an older KWin has neither a live nor a
                # target factor. Avoid polling its diagnostic interface.
                if info.strip():
                    reports_target = False

    with gio:
        bus_get(2, None, on_bus, None)  # G_BUS_TYPE_SESSION

    def sample(_on_bus=on_bus) -> float:  # retain the initial async callback
        nonlocal pending, next_query
        gio.dispatch()
        now = monotonic()
        if bus and reports_target and not pending and now >= next_query:
            pending = True
            next_query = now + (0.25 if factor > 1. else 1.)
            with gio:
                args = make_tuple((c_void_p * 1)(make_string(b'zoom')), 1)
                call(bus, b'org.kde.KWin', b'/Effects', b'org.kde.kwin.Effects', b'supportInformation',
                     args, result_type, 1, 1000, None, on_information, None)  # G_DBUS_CALL_FLAGS_NO_AUTO_START
        return factor

    return sample


@cache
def desktop_zoom_reader() -> Callable[[], float]:
    if is_macos:
        from .fast_data_types import cocoa_desktop_magnification
        return cocoa_desktop_magnification
    desktops = environ.get('XDG_CURRENT_DESKTOP', '').lower().split(':')
    try:
        if 'gnome' in desktops:
            return gnome_reader(Gio())
        if 'kde' in desktops:
            return kwin_reader(Gio())
    except (OSError, AttributeError):
        # The desktop integrations are optional on Linux.
        pass
    return lambda: 1.


def desktop_zoom_factor() -> float:
    factor = desktop_zoom_reader()()
    return factor if factor >= 1. and isfinite(factor) else 1.
