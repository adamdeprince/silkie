# License: GPLv3

from urllib.parse import quote

from kitty.accessibility import image_review_text
from kitty.fast_data_types import SCROLL_LINE

from .base import BaseTest, parse_bytes
from .graphics import parse_response, send_command


def annotate(screen, fields):
    screen.callbacks.clear()
    parse_bytes(screen, f'\x1b]777;a11y;{fields}\x1b\\'.encode('ascii'))
    return screen.callbacks.wtcbuf


def ack(id=1, status='OK'):
    return f'\x1b]777;a11y;v=1:op=ack:id={id}:status={status}\x1b\\'.encode('ascii')


class TestImageAccessibility(BaseTest):

    def test_magnification_reports_whole_raster_scale_up_to_three_times_base(self):
        s = self.create_screen(cell_width=9, cell_height=17)
        base, doubled, capped = (9, 17), (18, 34), (27, 51)

        def zoom(scale):
            s.pane_magnification(scale, 0.5, 0.5)
            s.update_graphics_layers()
            return s.reported_cell_size()

        put = 'a=T,f=24,i=9,p=1,s=1,v=1,c=1,r=1,C=1'
        self.ae(zoom(1.5), base)
        self.ae(parse_response(send_command(s, put, b'abc')), 'OK')
        densities = (
            (1, base), (1.001, doubled), (1.5, doubled), (2, doubled),
            (2.001, capped), (2.75, capped), (3, capped), (9, capped), (2, doubled),
        )
        for _ in range(64):
            for scale, expected in densities:
                self.ae(zoom(scale), expected)
        self.ae(zoom(9), capped)
        s.callbacks.clear()
        parse_bytes(s, b'\x1b[16t')
        self.ae(s.callbacks.wtcbuf, b'\x1b[6;51;27t')
        send_command(s, 'a=d,d=i,i=9,p=1')
        self.ae(zoom(1.5), doubled)
        self.ae(zoom(1), base)
        self.ae(zoom(1.5), base)

    def test_magnification_can_use_base_raster_density(self):
        s = self.create_screen(options={'magnify_raster_resolution': False})
        put = 'a=T,f=24,i=9,p=1,s=1,v=1,c=1,r=1,C=1'
        self.ae(parse_response(send_command(s, put, b'abc')), 'OK')
        s.pane_magnification(1.5, 0.5, 0.5)
        self.ae(len(s.update_graphics_layers()), 1)
        self.ae(s.reported_cell_size(), (10, 20))

        self.set_options({'magnify_raster_resolution': True})
        s.update_graphics_layers()
        self.ae(s.reported_cell_size(), (20, 40))
        self.set_options({'magnify_raster_resolution': False})
        s.update_graphics_layers()
        self.ae(s.reported_cell_size(), (10, 20))

    def test_image_annotations_are_per_placement(self):
        s = self.create_screen()
        svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><circle cx="5" cy="5" r="4"/></svg>'
        self.ae(parse_response(send_command(s, 'a=T,f=201,i=7,p=3,C=1', svg)), 'OK')
        s.cursor_position(2, 1)
        self.ae(parse_response(send_command(s, 'a=p,i=7,p=4,C=1')), 'OK')
        s.cursor_position(3, 1)
        self.ae(parse_response(send_command(s, 'a=p,i=7,p=5,C=1')), 'OK')

        title = 'Latency: café + throughput'
        description = 'One million requests\nThe median stayed below 20 ms.'
        details = 'https://example.com/results?mode=chart'
        body = ':'.join(f'{key}={quote(value, safe="")}' for key, value in dict(name=title, description=description, details=details).items())
        self.ae(annotate(s, f'v=1:op=set-image:i=7:p=3:purpose=essential:{body}:id=1'), ack())
        self.ae(annotate(s, 'v=1:op=set-image:i=7:p=4:purpose=decorative:name=Flourish'), b'')
        self.ae({entry[5]: entry[6] for entry in s.image_descriptions()}, {
            3: ('essential', title, description, details),
            4: ('decorative', 'Flourish', '', ''),
            5: None,
        })
        review = image_review_text(s.image_descriptions())
        self.assertIn(title + '\nEssential image, row 1, column 1.\n' + description, review)
        self.assertIn(details, review)
        self.assertIn('Unclassified image, row 3, column 1.', review)
        self.assertNotIn('Flourish', review)
        self.assertIn('Flourish', image_review_text(s.image_descriptions(), True))

    def test_image_annotation_validation_and_growth(self):
        s = self.create_screen()
        self.ae(annotate(s, 'v=1:op=clear-image:i=9:p=1:id=1'), ack(status='ENOENT'))
        self.ae(parse_response(send_command(s, 'a=T,f=24,i=9,p=1,s=1,v=1,C=1', b'abc')), 'OK')
        prefix = 'v=1:op=set-image:i=9:p=1:'
        for i in range(64):
            description = 'Chart row ' * 4096 + str(i)
            self.ae(annotate(s, f'{prefix}id={i}:purpose=informative:description={quote(description)}'), ack(i))
        original = s.image_descriptions()
        self.ae(original[0][6], ('informative', '', description, ''))
        for fields in (
            'v=2:i=9:p=1', 'v=1:p=1', 'v=1:i=9', 'v=1:i=0:p=1', 'v=1:i=9:p=0', 'v=1:i=9:p=4294967296',
            'v=1:i=9:p=1:purpose=important', 'v=1:i=9:p=1:name=x%4', 'v=1:i=9:p=1:name=x%zz', 'v=1:i=9:p=1:name=%FF',
            'v=1:i=9:p=1:name=%00', 'v=1:i=9:p=1:name=%0A', 'v=1:i=9:p=1:details=%1B', 'v=1:i=9:p=1:details=%C2%9B',
            'v=1:i=9:p=1:p=2', 'v=1:i=9:p=1:description=a:description=b',
        ):
            with self.subTest(fields=fields):
                self.ae(annotate(s, f'op=set-image:{fields}:id=1'), b'')
                self.ae(s.image_descriptions(), original)
        self.ae(annotate(s, prefix + 'name=Updated:id=1'), ack())
        self.ae(s.image_descriptions()[0][6], ('', 'Updated', '', ''))
        self.ae(annotate(s, 'v=1:op=clear-image:i=9:p=1:id=1'), ack())
        self.assertIsNone(s.image_descriptions()[0][6])

    def test_image_annotation_lifetime(self):
        s = self.create_screen(lines=3, scrollback=10)
        put = 'a=T,f=24,i=9,p=1,s=1,v=1,C=1'
        self.ae(parse_response(send_command(s, put, b'abc')), 'OK')
        annotate(s, 'v=1:op=set-image:i=9:p=1:purpose=essential:name=Chart')
        self.assertTrue(s.pause_rendering(True, 5000))
        send_command(s, 'a=d,d=i,i=9,p=1')
        self.assertIn('Chart', image_review_text(s.image_descriptions()))
        self.assertTrue(s.pause_rendering(False))
        self.ae(s.image_descriptions(), [])

        send_command(s, 'a=p,i=9,p=1,C=1')
        self.assertIsNone(s.image_descriptions()[0][6])
        annotate(s, 'v=1:op=set-image:i=9:p=1:purpose=essential:name=Chart')
        s.cursor_position(1, 2)
        send_command(s, 'a=p,i=9,p=1,C=1')
        self.assertIn('Chart\nEssential image, row 1, column 2.', image_review_text(s.image_descriptions()))
        parse_bytes(s, b'\x1b[?1049h')
        self.ae(s.image_descriptions(), [])
        self.ae(annotate(s, 'v=1:op=clear-image:i=9:p=1:id=1'), ack(status='ENOENT'))
        parse_bytes(s, b'\x1b[?1049l')
        self.assertIn('Chart', image_review_text(s.image_descriptions()))
        s.cursor_position(3, 1)
        s.index()
        self.ae(s.image_descriptions(), [])
        s.scroll(SCROLL_LINE, True)
        self.assertIn('Chart', image_review_text(s.image_descriptions()))
        s.scroll(SCROLL_LINE, False)
        send_command(s, put, b'def')
        self.assertIsNone(s.image_descriptions()[0][6])

    def test_unicode_placeholder_images_share_annotations(self):
        s = self.create_screen()
        self.ae(parse_response(send_command(s, 'a=T,f=24,i=42,p=1,s=2,v=2,c=2,r=2,U=1', b'abc' * 4)), 'OK')
        annotate(s, 'v=1:op=set-image:i=42:p=1:purpose=essential:name=Chart')
        self.ae(s.image_descriptions(), [])
        s.apply_sgr('38;5;42')
        s.apply_sgr('58;5;1')
        s.draw('\U0010eeee\u0305\U0010eeee\u0305')
        s.cursor_position(2, 1)
        s.draw('\U0010eeee\u030d\U0010eeee\u030d')
        s.update_only_line_graphics_data()
        self.assertIn('Chart\nEssential image', image_review_text(s.image_descriptions()))
        self.ae(image_review_text(s.image_descriptions()).count('Chart'), 1)
