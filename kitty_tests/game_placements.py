#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from .base import BaseTest, parse_bytes
from .graphics import parse_full_response, parse_response, send_command


class TestGamePlacements(BaseTest):

    def screen(self):
        screen = self.create_screen(cols=160, lines=50, cell_width=8, cell_height=16)
        self.upload(screen)
        return screen

    def upload(self, screen):
        response = parse_full_response(send_command(screen, 'a=t,f=32,i=1,s=64,v=64', bytes((180, 140, 70, 255)) * 4096))
        self.ae(response.code, 'OK', response.msg)

    def put(self, screen, placement, column=1, z=-1900000000):
        parse_bytes(screen, f'\x1b[2;{column}H'.encode())
        response = parse_response(send_command(screen, f'a=p,i=1,p={placement},C=1,x=16,y=0,w=16,h=16,z={z}'))
        self.ae(response, 'OK')

    def layers(self, screen):
        return screen.grman.update_layers(0, -1, 1, 2 / 160, 2 / 50, 160, 50, 8, 16)

    def test_stable_sprite_placement_updates(self):
        screen = self.screen()
        # Sparse protocol IDs must not turn into an array sized by the largest ID.
        ids = [0xffffffff, 0x80000000, 17] + [1000 + i * 13 for i in range(2048)]
        for index, placement in enumerate(ids):
            self.put(screen, placement, z=-1900000000 + index)
        original = self.layers(screen)
        self.ae(len(original), len(ids))
        refs = [r['ref_id'] for r in original]
        self.ae(len(set(refs)), len(ids))
        for frame in range(12):
            for index in reversed(range(len(ids))):
                self.put(screen, ids[index], column=1 + frame, z=-1900000000 + index)
            layers = self.layers(screen)
            self.ae([r['ref_id'] for r in layers], refs)
            self.ae([r['z_index'] for r in layers], [r['z_index'] for r in original])
            self.ae(screen.grman.image_for_client_id(1)['refs.count'], len(ids))
            if frame:
                self.assertGreater(layers[0]['dest_rect']['left'], original[0]['dest_rect']['left'])

    def test_sprite_placement_lifetimes(self):
        screen = self.screen()
        self.put(screen, 7)
        original = self.layers(screen)[0]['ref_id']
        # Anonymous placements accumulate rather than replacing one another.
        for _ in range(8):
            self.put(screen, 0)
        self.ae(screen.grman.image_for_client_id(1)['refs.count'], 9)
        for _ in range(128):
            self.ae(send_command(screen, 'a=d,d=i,i=1,p=7'), b'')
            self.ae(screen.grman.image_for_client_id(1)['refs.count'], 8)
            self.put(screen, 7)
            self.ae(screen.grman.image_for_client_id(1)['refs.count'], 9)
            layers = self.layers(screen)
            self.assertNotIn(original, [r['ref_id'] for r in layers])
            original = max(r['ref_id'] for r in layers)
        # Replacing an uploaded sheet releases its placement index as well as its refs.
        self.upload(screen)
        self.ae(screen.grman.image_for_client_id(1)['refs.count'], 0)
        self.put(screen, 7)
        self.ae(screen.grman.image_for_client_id(1)['refs.count'], 1)

    def test_sprite_placements_in_synchronized_frames(self):
        screen = self.screen()
        for placement in range(1, 33):
            self.put(screen, placement, z=-1900000000 + placement)
        original = [r['ref_id'] for r in self.layers(screen)]
        # The paused graphics manager owns independent refs and lookup storage.
        for frame in range(64):
            self.assertTrue(screen.pause_rendering(True, 5000))
            for placement in range(1, 33):
                self.put(screen, placement, column=1 + frame, z=-1900000000 + placement)
            self.assertTrue(screen.pause_rendering(False, 0))
            self.ae([r['ref_id'] for r in self.layers(screen)], original)
