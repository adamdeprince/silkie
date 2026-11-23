"""Play the Chinese writing-direction demo in a private Silkie window.

Run python3 demo.py, or python3 demo.py --record movie.mp4 (requires FFmpeg).
The recording is made from Silkie's own window screenshots.
"""

import argparse
import os
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import termios
import time
from contextlib import ExitStack
from pathlib import Path

FPS = 8
SECONDS = 30
ESC = '\033'
ST = ESC + '\\'
RESET = ESC + '[0m'
RED = ESC + '[38;2;155;54;37m'
GREEN = ESC + '[38;2;66;93;74m'
CLEAR = ESC + '[2J' + ESC + '[H'
# Zhang Ruoxu, "Spring River, Flower, Moon, Night" (Tang dynasty).
# https://zh.wikisource.org/zh-hant/春江花月夜_(張若虛)
OPENING = '春江潮水连海平，海上明月共潮生。'
POEM = (
    '春江潮水連海平海上明月共潮生'
    '灩灩隨波千萬里何處春江無月明'
    '江流宛轉遶芳甸月照花林皆似霰'
    '空裏流霜不覺飛汀上白沙看不見'
    '江天一色無纖塵皎皎空中孤月輪'
    '江畔何人初見月江月何年初照人'
    '人生代代無窮已江月年年望相似'
    '不知江月待何人但見長江送流水'
    '白雲一片去悠悠青楓浦上不勝愁'
    '誰家今夜扁舟子何處相思明月樓'
    '可憐樓上月徘徊應照離人妝鏡臺'
    '玉戶簾中卷不去擣衣砧上拂還來'
    '此時相望不相聞願逐月華流照君'
    '鴻雁長飛光不度魚龍潛躍水成文'
    '昨夜閒潭夢落花可憐春半不還家'
    '江水流春去欲盡江潭落月復西斜'
    '斜月沉沉藏海霧碣石瀟湘無限路'
    '不知乘月幾人歸落月搖情滿江樹'
)
VERTICAL_START = 3.5


def caption(t: float) -> str:
    def small(line: str) -> str:
        return ESC + ']66;n=4:d=5:v=2:h=2;' + line + ST

    if t < VERTICAL_START:
        first, second = '唐　张若虚', '横着读，还记得下一句吗？'
    elif t < 14:
        first, second = '换成古书里的竖排，', '自上而下，自右向左。'
    else:
        first, second = '新诗句从左边一列列写入，', '先前的诗句便从右边移出。'
    return (CLEAR + RED + 'Silkie' + RESET + '  ' + GREEN + '春江花月夜' + RESET
            + '\r\n' + small(first) + '\r\n' + small(second))


def scene(t: float) -> tuple[str, str]:
    if t < VERTICAL_START:
        return 'right down', OPENING[:1 + int(7 * t)]
    elapsed = t - VERTICAL_START
    # Give the opening couplet time to settle before the river fills the page.
    count = min(14, 1 + int(5 * elapsed)) + max(0, int((elapsed - 3.5) * 12))
    return 'down left', POEM[:count]


def render_pane(role: str, address: str) -> None:
    tty_settings = termios.tcgetattr(sys.stdin)
    tty_settings[3] &= ~(termios.ECHO | termios.ECHONL)
    termios.tcsetattr(sys.stdin, termios.TCSANOW, tty_settings)
    with socket.socket(socket.AF_UNIX) as conn:
        conn.connect(address)
        with conn.makefile('rwb', buffering=0) as stream:
            stream.write((role + '\n').encode())
            previous = None
            sys.stdout.write(ESC + '[?25l')
            for request in stream:
                t = int(request) / FPS
                if role == 'caption':
                    state = caption(t)
                    output = state if previous != state else ''
                else:
                    state = scene(t)
                    axes, text = state
                    if previous is None or axes != previous[0]:
                        inline, line = axes.split()
                        output = CLEAR + f'{ESC}]777;flow;v=1:op=set:inline={inline}:line={line}{ST}' + CLEAR + text
                    else:
                        output = text[len(previous[1]):]
                previous = state
                sys.stdout.write(output)
                sys.stdout.flush()
                stream.write(b'1\n' if output else b'0\n')


def binary(name: str, local: str) -> str:
    path = Path(__file__).resolve().parents[1] / 'kitty/launcher' / local
    return str(path) if path.is_file() else shutil.which(name) or name


def play(destination: Path | None) -> None:
    if destination and destination.exists():
        raise FileExistsError(destination)
    with tempfile.TemporaryDirectory(prefix='silkie-film-') as tmp, ExitStack() as stack:
        folder = Path(tmp)
        address = str(folder / 'panes')
        listener = stack.enter_context(socket.socket(socket.AF_UNIX))
        listener.bind(address)
        listener.listen()
        listener.settimeout(20)
        remote = 'unix:' + str(folder / 'control')
        command = shlex.join([sys.executable, str(Path(__file__).resolve()), '--pane'])
        session = folder / 'session'
        session.write_text('layout splits\n' + f'launch {command} caption {shlex.quote(address)}\n'
                           + f'launch --location=hsplit --bias=66 {command} essay {shlex.quote(address)}\n')
        config = folder / 'silkie.conf'
        config.write_text('''remember_window_size no
initial_window_width 476
initial_window_height 476
font_size 31
font_family Menlo
symbol_map U+3000-U+303F,U+3400-U+9FFF,U+FF00-U+FFEF Songti TC
background #f5efdf
foreground #262c28
cursor_blink_interval 0
window_padding_width 10
window_border_width 1px
active_border_color #aab29e
inactive_border_color #aab29e
inactive_text_alpha 1
hide_window_decorations yes
tab_bar_style hidden
scrollbar never
pixel_scroll no
allow_remote_control socket-only
''')
        app = subprocess.Popen([binary('silkie', 'kitty'), '--config', str(config), '--title', 'Silkie · 春江花月夜',
                                '--listen-on', remote, '--session', str(session)],
                               env=dict(os.environ, KITTY_CACHE_DIRECTORY=str(folder / 'cache')))

        def close_app() -> None:
            if app.poll() is None:
                app.terminate()
                app.wait()

        stack.callback(close_app)
        panes = []
        for _ in range(2):
            conn, _ = listener.accept()
            stack.enter_context(conn)
            pipe = stack.enter_context(conn.makefile('rwb', buffering=0))
            pipe.readline()
            panes.append(pipe)
        if destination:
            encoder = subprocess.Popen(['ffmpeg', '-v', 'error', '-n', '-f', 'image2pipe', '-framerate', str(FPS),
                                        '-c:v', 'png', '-i', '-', '-vf', 'scale=540:540:flags=lanczos,setsar=1,fps=24',
                                        '-an', '-c:v', 'libx264', '-crf', '17', '-pix_fmt', 'yuv420p',
                                        '-movflags', '+faststart', str(destination)], stdin=subprocess.PIPE)
            stack.enter_context(encoder)
        start = time.monotonic()
        for tick in range(SECONDS * FPS):
            for pipe in panes:
                pipe.write(f'{tick}\n'.encode())
            replies = [pipe.readline() for pipe in panes]
            if not all(replies):
                raise RuntimeError('A demo pane closed before the last frame')
            if destination:
                if b'1\n' in replies:
                    time.sleep(0.10 if tick else 0.3)
                    frame = folder / 'frame.png'
                    subprocess.run([binary('kitten', 'kitten'), '@', '--to', remote, 'screenshot', str(frame)], check=True)
                    pixels = frame.read_bytes()
                encoder.stdin.write(pixels)
            else:
                time.sleep(max(0, start + (tick + 1) / FPS - time.monotonic()))
        if destination:
            encoder.stdin.close()
            if encoder.wait():
                raise RuntimeError('Video encoding failed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', type=Path)
    parser.add_argument('--pane', nargs=2, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.pane:
        render_pane(*args.pane)
    else:
        play(args.record)
