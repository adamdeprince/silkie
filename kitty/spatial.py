#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince
"""Spatial presentation and the XREAL One-series USB-network IMU transport.

Wire facts: https://github.com/nikryden/Screen-XREAL-1S (OneReportFramer.cs).
This is an independent decoder: six-byte header, 128-byte little-endian body.
All samples are integrated; only the current orientation needs to be retained.
"""
import math
import socket
import struct
import threading
import time
from dataclasses import dataclass, field

MODES = ('off', 'follow', 'anchored')
IDENTITY = (1., 0., 0., 0.)


def requires_xreal(mode: str, output: str) -> bool:
    return mode != 'off' and (mode == 'anchored' or output == 'sbs')


def multiply(a: tuple[float, ...], b: tuple[float, ...]) -> tuple[float, ...]:
    w, x, y, z = a
    v, i, j, k = b
    return (w*v-x*i-y*j-z*k, w*i+x*v+y*k-z*j, w*j-x*k+y*v+z*i, w*k+x*j-y*i+z*v)


def inverse(q: tuple[float, ...]) -> tuple[float, ...]:
    return (q[0], -q[1], -q[2], -q[3])


def matrix(q: tuple[float, ...]) -> tuple[float, ...]:
    """Column-major world-to-view rotation for OpenGL."""
    w, x, y, z = inverse(q)
    return (1-2*(y*y+z*z), 2*(x*y+w*z), 2*(x*z-w*y),
            2*(x*y-w*z), 1-2*(x*x+z*z), 2*(y*z+w*x),
            2*(x*z+w*y), 2*(y*z-w*x), 1-2*(x*x+y*y))


class IMUDecoder:
    def __init__(self) -> None:
        self.pending = bytearray()
        self.orientation = IDENTITY
        self.timestamp: int | None = None
        self.samples = 0

    def feed(self, data: bytes) -> int:
        self.pending.extend(data)
        offset = 0
        samples = 0
        while len(self.pending) - offset >= 6:
            if self.pending[offset] not in (0x27, 0x28) or self.pending[offset+1] != 0x36:
                offset += 1
                continue
            if struct.unpack_from('>I', self.pending, offset+2)[0] != 128:
                offset += 1
                continue
            if len(self.pending) - offset < 134:
                break
            body = offset + 6
            if struct.unpack_from('<I', self.pending, body+0x18)[0] == 0x0b:
                stamp = struct.unpack_from('<Q', self.pending, body+8)[0]
                gyro = struct.unpack_from('<3f', self.pending, body+0x1c)
                accel = struct.unpack_from('<3f', self.pending, body+0x28)
                if all(map(math.isfinite, (*gyro, *accel))):
                    self.integrate(stamp, gyro, accel)
                    samples += 1
            offset += 134
        del self.pending[:offset]
        self.samples += samples
        return samples

    def integrate(self, stamp: int, gyro: tuple[float, ...], accel: tuple[float, ...]) -> None:
        previous, self.timestamp = self.timestamp, stamp
        if previous is None or stamp <= previous:
            return
        dt = (stamp - previous) / 1e9
        # A suspended/restarted device cannot describe the intervening motion.
        # Rebase its clock; never integrate an invented long movement.
        if dt > 0.1:
            return
        # Sensor: right/down/forward. Renderer: right/up/backward.
        gx, gy, gz = gyro[0], -gyro[1], -gyro[2]
        ax, ay, az = accel[0], -accel[1], -accel[2]
        norm = math.hypot(ax, ay, az)
        if norm:
            ax, ay, az = ax/norm, ay/norm, az/norm
            w, x, y, z = self.orientation
            # Gravity direction predicted in body coordinates. Complementary
            # correction stabilizes pitch/roll; yaw still needs recentering.
            vx, vy, vz = 2*(x*y+w*z), 1-2*(x*x+z*z), 2*(y*z-w*x)
            gx += ay*vz-az*vy
            gy += az*vx-ax*vz
            gz += ax*vy-ay*vx
        speed = math.hypot(gx, gy, gz)
        if speed:
            angle = speed*dt/2
            scale = math.sin(angle)/speed
            q = multiply(self.orientation, (math.cos(angle), gx*scale, gy*scale, gz*scale))
            length = math.hypot(*q)
            if math.isfinite(length) and length:
                self.orientation = tuple(v/length for v in q)


class Tracker:
    def __init__(self, address: str) -> None:
        host, sep, port = address.rpartition(':')
        if not sep or not host or not 0 < int(port) < 65536:
            raise ValueError('Tracking address must be host:port')
        self.address = (host, int(port))
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.orientation = IDENTITY
        self.updated = 0.
        self.status = 'Connecting to XREAL tracking…'
        self.connection: socket.socket | None = None
        self.thread = threading.Thread(target=self.run, name='Silkie XREAL tracking', daemon=True)
        self.thread.start()

    def run(self) -> None:
        while not self.stop.is_set():
            decoder = IMUDecoder()
            try:
                with socket.create_connection(self.address, timeout=1) as connection:
                    self.connection = connection
                    connection.settimeout(0.25)
                    while not self.stop.is_set():
                        try:
                            data = connection.recv(134 * 64) # read batch, not a stream capacity
                        except socket.timeout:
                            continue
                        if not data:
                            raise ConnectionError('XREAL tracking disconnected')
                        if decoder.feed(data):
                            with self.lock:
                                self.orientation = decoder.orientation
                                self.updated = time.monotonic()
                                self.status = 'XREAL orientation tracking (3DoF)'
            except (OSError, ValueError) as error:
                with self.lock:
                    self.updated = 0
                    self.status = str(error)
            finally:
                self.connection = None
            self.stop.wait(1)

    def close(self) -> None:
        self.stop.set()
        connection = self.connection
        if connection is not None:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def pose(self) -> tuple[tuple[float, ...], bool]:
        with self.lock:
            return self.orientation, bool(self.updated and time.monotonic()-self.updated < 0.25)


@dataclass
class Presentation:
    mode: str = 'off'
    output: str = 'sbs'
    reference: tuple[float, ...] | None = None
    tracking: bool = False


@dataclass
class Controller:
    windows: dict[int, Presentation] = field(default_factory=dict)
    tracker: Tracker | None = None
    timer: int = 0

    def close(self, window: int | None = None) -> None:
        if window is None:
            self.windows.clear()
        else:
            self.windows.pop(window, None)
        self.stop_unused_tracking()

    def stop_unused_tracking(self) -> None:
        if self.tracker and not any(p.mode == 'anchored' for p in self.windows.values()):
            self.tracker.close()
            self.tracker = None
        if self.timer and not any(p.mode == 'anchored' for p in self.windows.values()):
            from .fast_data_types import remove_timer
            remove_timer(self.timer)
            self.timer = 0

    def configure(self, window: int, mode: str, output: str | None = None) -> None:
        from .constants import is_macos
        from .fast_data_types import add_timer, get_options, spatial_configure
        if mode not in MODES:
            raise ValueError('Use off, follow or anchored')
        if mode != 'off' and not is_macos:
            raise ValueError('Spatial presentation is currently implemented on macOS')
        opts = get_options()
        if output is None:
            previous = self.windows.get(window)
            output = previous.output if previous else opts.spatial_output
        if output not in ('sbs', 'red-blue'):
            raise ValueError('Use sbs or red-blue')
        if requires_xreal(mode, output) and not opts.experimental_xreal_1s:
            raise ValueError('XREAL 1S support is experimental and untested. Enable experimental_xreal_1s in Settings → Experimental and untested before using it.')
        host, sep, port = opts.spatial_tracking_address.rpartition(':')
        if mode == 'anchored' and (not sep or not host or not 0 < int(port) < 65536):
            raise ValueError('Tracking address must be host:port')
        if not spatial_configure(window, MODES.index(mode), output == 'red-blue', opts.spatial_wall_scale, opts.spatial_field_of_view,
                                 opts.spatial_positive_divergence, opts.spatial_negative_divergence, opts.spatial_vertical_phase_limit,
                                 math.exp2(opts.spatial_divergence_exponent)):
            return
        self.windows[window] = Presentation(mode, output)
        if mode == 'anchored':
            if self.tracker and self.tracker.address != (host, int(port)):
                self.tracker.close()
                self.tracker = None
            if self.tracker is None:
                self.tracker = Tracker(opts.spatial_tracking_address)
        if not self.timer and any(p.mode == 'anchored' for p in self.windows.values()):
            self.timer = add_timer(self.tick, 1/120, True)
        self.stop_unused_tracking()

    def recenter(self, window: int) -> None:
        if window in self.windows:
            self.windows[window].reference = None

    def tick(self, timer: int) -> None:
        from .fast_data_types import get_boss, remove_timer, spatial_pose
        boss = get_boss()
        self.windows = {wid: p for wid, p in self.windows.items() if wid in boss.os_window_map}
        if not any(p.mode == 'anchored' for p in self.windows.values()) and self.tracker:
            self.tracker.close()
            self.tracker = None
        changed = False
        for wid, p in self.windows.items():
            if p.mode != 'anchored':
                continue
            q, valid = self.tracker.pose() if self.tracker else (IDENTITY, False)
            if valid and p.reference is None:
                p.reference = q
            if not valid:
                p.reference = None
            spatial_pose(wid, matrix(multiply(inverse(p.reference or q), q)), valid)
            changed |= valid != p.tracking
            p.tracking = valid
        if changed:
            from .native_ui import update_native_menus
            update_native_menus()
        if not any(p.mode == 'anchored' for p in self.windows.values()):
            remove_timer(timer)
            self.timer = 0


controller = Controller()
