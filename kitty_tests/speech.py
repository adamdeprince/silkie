# License: GPLv3

from types import SimpleNamespace

from kitty.speech import SpeechController, cocoa_words

from .base import BaseTest, parse_bytes


class TestSpeech(BaseTest):
    def test_terminal_speech_queue_and_styles(self):
        screen = self.create_screen()
        sent = []
        controller = SpeechController.__new__(SpeechController)
        controller.channels = {}
        controller.timer = None
        controller.engine = 'speech_dispatcher'
        controller.dispatcher = SimpleNamespace(set_pitch_range=True, submit=lambda pane, item: sent.append((pane, item)))
        screen.callbacks.speech_command = lambda op, fields: controller.request(5, op, fields)

        def command(body):
            screen.callbacks.clear()
            parse_bytes(screen, (f'\x1b]777;speech;v=1:{body}\x1b\\').encode())
            return screen.callbacks.wtcbuf

        self.ae(command('op=query:id=1'), b'\x1b]777;speech;v=1:op=cap:id=1:available=1:engine=speech_dispatcher:pitch_range=1\x1b\\')
        okay = b'\x1b]777;speech;v=1:op=ack:id=1:status=OK\x1b\\'
        self.ae(command('op=set-style:style=comment:voice=Native%20Voice:pitch=-20:split_caps=1:id=1'), okay)
        for _ in range(128):
            self.ae(command('op=queue:style=comment:text=MixedCase%21:punctuation=all:id=1'), okay)
        self.ae(sent, [])
        self.ae(command('op=dispatch:id=1'), okay)
        self.ae(len(sent), 128)
        for pane, item in sent:
            self.ae((pane, item.text, item.voice, item.pitch), (5, 'Mixed Case!', 'Native Voice', -20))
            self.assertIn('exclamation mark', cocoa_words(item))
        self.ae(command('op=character:text=%20:interrupt=1:id=1'), okay)
        self.ae((sent[-2], sent[-1][1].text), ((5, 'stop'), ' '))
        self.ae(command('op=pause:id=1'), okay)
        previous = len(sent)
        self.ae(command('op=speak:text=Resumed:id=1'), okay)
        self.ae(len(sent), previous)
        self.ae(command('op=resume:id=1'), okay)
        self.ae((sent[-2], sent[-1][1].text), ((5, 'resume'), 'Resumed'))
        self.ae(command('op=speak:text=%00:id=1'), b'')
        self.ae(command('op=speak:text=hello:pitch=101:id=1'), b'\x1b]777;speech;v=1:op=ack:id=1:status=EINVAL\x1b\\')
        self.ae(command('op=reset:id=1'), okay)
        self.ae(controller.channels[5].styles, {})
