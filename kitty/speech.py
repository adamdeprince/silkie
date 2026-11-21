# License: GPLv3

import re
import unicodedata
from collections import deque
from ctypes import CDLL, c_char_p, c_int, c_void_p, string_at
from dataclasses import dataclass, field
from threading import Condition, Thread
from time import monotonic
from typing import Any

from . import fast_data_types as fdt
from .constants import is_macos

identifier = re.compile(r'[A-Za-z0-9_.-]+')
signed = re.compile(r'-?[0-9]+')
style_fields = frozenset(('voice', 'language', 'rate', 'pitch', 'pitch_range', 'volume', 'punctuation', 'split_caps'))


@dataclass
class SpeechItem:
    kind: str
    text: str = ''
    voice: str = ''
    language: str = ''
    rate: int = 0
    pitch: int = 0
    pitch_range: int = 0
    volume: int = -1
    punctuation: str = 'some'
    duration: int = 0


def bounded_int(value: str, lower: int, upper: int) -> int:
    if not signed.fullmatch(value):
        raise ValueError(value)
    number = int(value)
    if not lower <= number <= upper:
        raise ValueError(value)
    return number


def style_options(fields: dict[str, str]) -> dict[str, Any]:
    settings: dict[str, Any] = {}
    for key in style_fields:
        if key not in fields:
            continue
        value = fields[key]
        if key in {'rate', 'pitch', 'pitch_range'}:
            settings[key] = bounded_int(value, -100, 100)
        elif key == 'volume':
            settings[key] = bounded_int(value, 0, 100)
        elif key == 'punctuation':
            if value not in {'none', 'some', 'all'}:
                raise ValueError(value)
            settings[key] = value
        elif key == 'split_caps':
            if value not in {'0', '1'}:
                raise ValueError(value)
            settings[key] = value == '1'
        elif key == 'language':
            if not identifier.fullmatch(value):
                raise ValueError(value)
            settings[key] = value
        else:
            settings[key] = value
    return settings


def symbol_name(char: str) -> str:
    return 'space' if char == ' ' else unicodedata.name(char, char).lower().replace('-', ' ')


def cocoa_words(item: SpeechItem) -> str:
    if item.kind == 'character':
        return symbol_name(item.text) if not item.text.isalnum() else item.text
    if item.punctuation == 'some':
        return item.text
    return ''.join(
        (f' {symbol_name(char)} ' if item.punctuation == 'all' else ' ')
        if unicodedata.category(char)[0] in 'PS' else char
        for char in item.text
    )


class SpeechDispatcher:
    # The library handles the configured Speech Dispatcher connection. Each
    # pane has its own client connection so cancellation stays in that pane.
    def __init__(self) -> None:
        lib = CDLL('libspeechd.so.2')

        def fn(name: str, result, *args):
            function = getattr(lib, f'spd_{name}')
            function.argtypes = args
            function.restype = result
            return function

        self.open = fn('open', c_void_p, c_char_p, c_char_p, c_char_p, c_int)
        self.disconnect = fn('close', None, c_void_p)
        self.cancel = fn('cancel', c_int, c_void_p)
        self.pause = fn('pause', c_int, c_void_p)
        self.resume = fn('resume', c_int, c_void_p)
        self.speak = fn('say', c_int, c_void_p, c_int, c_char_p)
        self.char = fn('char', c_int, c_void_p, c_int, c_char_p)
        self.sound = fn('sound_icon', c_int, c_void_p, c_int, c_char_p)
        self.set_data_mode = fn('set_data_mode', c_int, c_void_p, c_int)
        self.set_language = fn('set_language', c_int, c_void_p, c_char_p)
        self.get_language = fn('get_language', c_void_p, c_void_p)
        self.set_voice = fn('set_synthesis_voice', c_int, c_void_p, c_char_p)
        self.set_voice_type = fn('set_voice_type', c_int, c_void_p, c_int)
        self.get_voice_type = fn('get_voice_type', c_int, c_void_p)
        self.set_rate = fn('set_voice_rate', c_int, c_void_p, c_int)
        self.set_pitch = fn('set_voice_pitch', c_int, c_void_p, c_int)
        try:
            self.set_pitch_range = fn('set_voice_pitch_range', c_int, c_void_p, c_int)
        except AttributeError:
            self.set_pitch_range = None
        self.set_volume = fn('set_volume', c_int, c_void_p, c_int)
        self.get_volume = fn('get_volume', c_int, c_void_p)
        self.set_punctuation = fn('set_punctuation', c_int, c_void_p, c_int)
        self.free = CDLL(None).free
        self.free.argtypes = [c_void_p]
        self.free.restype = None
        self.commands: deque[tuple[int, SpeechItem | str]] = deque()
        self.condition = Condition()
        self.closed = False
        Thread(target=self.run, name='Silkie speech', daemon=True).start()

    def submit(self, pane: int, item: SpeechItem | str) -> None:
        with self.condition:
            if item == 'stop':
                self.commands = deque((p, i) for p, i in self.commands if p != pane)
                self.commands.appendleft((pane, item))
            else:
                self.commands.append((pane, item))
            self.condition.notify()

    def run(self) -> None:
        connections: dict[int, tuple[int, bytes, int, int]] = {}
        try:
            while True:
                with self.condition:
                    self.condition.wait_for(lambda: self.closed or self.commands)
                    if self.closed:
                        return
                    pane, item = self.commands.popleft()
                if isinstance(item, str):
                    if known := connections.get(pane):
                        connection = known[0]
                        if item == 'stop':
                            self.cancel(connection)
                            self.disconnect(connection)
                            del connections[pane]
                        elif item == 'pause':
                            self.pause(connection)
                        elif item == 'resume':
                            self.resume(connection)
                    continue
                if pane not in connections:
                    connection = self.open(b'silkie', f'pane-{pane}'.encode(), None, 0)
                    if not connection:
                        continue
                    language = b''
                    if value := self.get_language(connection):
                        language = string_at(value)
                        self.free(value)
                    connections[pane] = connection, language, self.get_voice_type(connection), self.get_volume(connection)
                connection, language, voice_type, volume = connections[pane]
                self.play(connection, language, voice_type, volume, item)
        finally:
            for connection, *_ in connections.values():
                self.cancel(connection)
                self.disconnect(connection)

    def play(self, connection: int, language: bytes, voice_type: int, volume: int, item: SpeechItem) -> None:
        priority = 3  # SPD_TEXT queues additional speech of the same priority.
        if item.kind == 'sound':
            self.sound(connection, priority, item.text.encode())
            return
        if item.kind == 'silence':
            self.set_data_mode(connection, 1)  # SPD_DATA_SSML
            self.speak(connection, priority, f'<speak><break time="{item.duration}ms"/></speak>'.encode())
            self.set_data_mode(connection, 0)
            return
        if language or item.language:
            self.set_language(connection, item.language.encode() or language)
        if voice_type > 0:
            self.set_voice_type(connection, voice_type)
        if item.voice:
            self.set_voice(connection, item.voice.encode())
        self.set_rate(connection, item.rate)
        self.set_pitch(connection, item.pitch)
        if self.set_pitch_range is not None:
            self.set_pitch_range(connection, item.pitch_range)
        self.set_volume(connection, item.volume * 2 - 100 if item.volume >= 0 else volume)
        self.set_punctuation(connection, {'all': 0, 'none': 1, 'some': 2}[item.punctuation])
        (self.char if item.kind == 'character' else self.speak)(connection, priority, item.text.encode())

    def close(self) -> None:
        with self.condition:
            self.closed = True
            self.condition.notify()


@dataclass
class SpeechChannel:
    queued: list[SpeechItem] = field(default_factory=list)
    ready: deque[SpeechItem] = field(default_factory=deque)
    styles: dict[str, dict[str, Any]] = field(default_factory=dict)
    paused: bool = False
    until: float = 0.
    remaining: float = 0.


class SpeechController:
    def __init__(self) -> None:
        self.channels: dict[int, SpeechChannel] = {}
        self.timer: int | None = None
        self.dispatcher: SpeechDispatcher | None = None
        self.engine = 'avspeech' if is_macos else 'none'
        if not is_macos:
            try:
                self.dispatcher = SpeechDispatcher()
                self.engine = 'speech_dispatcher'
            except (OSError, AttributeError):
                pass

    def available(self) -> bool:
        return self.engine != 'none'

    def control(self, pane: int, op: str) -> None:
        if self.dispatcher is not None:
            self.dispatcher.submit(pane, op)
        elif is_macos:
            fdt.cocoa_speech_control(pane, op)

    def release(self, pane: int) -> None:
        if pane in self.channels:
            del self.channels[pane]
            self.control(pane, 'stop')

    def close(self) -> None:
        for pane in tuple(self.channels):
            self.release(pane)
        if self.timer is not None:
            fdt.remove_timer(self.timer)
            self.timer = None
        if self.dispatcher is not None:
            self.dispatcher.close()

    def deliver(self, pane: int, channel: SpeechChannel, items: list[SpeechItem]) -> None:
        if self.dispatcher is not None and not channel.paused:
            for item in items:
                self.dispatcher.submit(pane, item)
        elif items:
            channel.ready.extend(items)
            if self.dispatcher is None and is_macos:
                if self.timer is None:
                    self.timer = fdt.add_timer(self.pump, 0.05)
                self.pump()

    def pump(self, _id: int | None = None) -> None:
        keep = False
        now = monotonic()
        for pane, channel in self.channels.items():
            if channel.paused:
                continue
            if not channel.ready:
                continue
            keep = True
            if channel.until > now or fdt.cocoa_speech_control(pane, 'busy'):
                continue
            item = channel.ready.popleft()
            if item.kind == 'silence':
                channel.until = now + item.duration / 1000
            elif item.kind == 'sound':
                channel.until = now + fdt.cocoa_speech_sound(pane, item.text)
            else:
                text = cocoa_words(item)
                if text.strip():
                    fdt.cocoa_speak(pane, text, item.voice, item.language, item.rate, item.pitch, item.volume)
        if not keep and self.timer is not None:
            fdt.remove_timer(self.timer)
            self.timer = None

    def request(self, pane: int, op: str, fields: dict[str, str]) -> str:
        if op == 'query':
            range_pitch = int(bool(self.dispatcher and self.dispatcher.set_pitch_range))
            return f'op=cap:available={int(self.available())}:engine={self.engine}:pitch_range={range_pitch}'
        if not self.available():
            return 'status=ENOTSUP'
        try:
            return 'status=' + self.change(pane, op, fields)
        except (ValueError, OverflowError):
            return 'status=EINVAL'

    def change(self, pane: int, op: str, fields: dict[str, str]) -> str:
        channel = self.channels.setdefault(pane, SpeechChannel())
        if op in {'set-style', 'clear-style'}:
            name = fields.get('style', '')
            if not identifier.fullmatch(name):
                return 'EINVAL'
            if op == 'clear-style':
                return 'OK' if channel.styles.pop(name, None) is not None else 'ENOENT'
            channel.styles[name] = style_options(fields)
        elif op in {'queue', 'speak', 'character', 'sound', 'silence'}:
            item = self.item(channel, op, fields)
            if fields.get('interrupt', '0') not in {'0', '1'} or fields.get('queued', '0') not in {'0', '1'}:
                return 'EINVAL'
            if fields.get('interrupt') == '1':
                self.stop(pane, channel)
            if op == 'queue' or fields.get('queued') == '1':
                channel.queued.append(item)
            else:
                self.deliver(pane, channel, [item])
        elif op == 'dispatch':
            items, channel.queued = channel.queued, []
            self.deliver(pane, channel, items)
        elif op in {'stop', 'reset'}:
            self.stop(pane, channel)
            if op == 'reset':
                channel.styles.clear()
        elif op == 'pause':
            if not channel.paused:
                channel.paused = True
                channel.remaining = max(0., channel.until - monotonic())
                self.control(pane, op)
        elif op == 'resume':
            if channel.paused:
                channel.paused = False
                channel.until = monotonic() + channel.remaining
                self.control(pane, op)
                if self.dispatcher is not None and channel.ready:
                    items = list(channel.ready)
                    channel.ready.clear()
                    self.deliver(pane, channel, items)
                elif is_macos and channel.ready and self.timer is None:
                    self.timer = fdt.add_timer(self.pump, 0.05)
        else:
            return 'EINVAL'
        return 'OK'

    def stop(self, pane: int, channel: SpeechChannel) -> None:
        channel.queued.clear()
        channel.ready.clear()
        channel.paused = False
        channel.until = channel.remaining = 0.
        self.control(pane, 'stop')

    @staticmethod
    def item(channel: SpeechChannel, op: str, fields: dict[str, str]) -> SpeechItem:
        if op == 'silence':
            return SpeechItem(op, duration=bounded_int(fields.get('duration', ''), 0, 2147483647))
        if op == 'sound':
            name = fields.get('name', '')
            if not identifier.fullmatch(name):
                raise ValueError(name)
            return SpeechItem(op, name)
        name = fields.get('style', '')
        options = dict(channel.styles[name]) if name in channel.styles else {}
        if name and name not in channel.styles:
            raise ValueError(name)
        options.update(style_options(fields))
        text = fields.get('text', '')
        if op == 'character':
            if len(text) != 1:
                raise ValueError(text)
        elif not text.strip():
            raise ValueError(text)
        elif options.get('split_caps'):
            text = re.sub(r'(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])', ' ', text)
        options.pop('split_caps', None)
        return SpeechItem('character' if op == 'character' else 'text', text, **options)
