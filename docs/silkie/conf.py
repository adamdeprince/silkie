"""Build only Silkie's extensions; link inherited behavior to upstream kitty."""

import ast
from pathlib import Path
from textwrap import dedent, indent

from docutils import nodes
from sphinx.application import Sphinx
from sphinx.environment import BuildEnvironment
from sphinx.errors import ConfigError

project = 'Silkie'
author = 'Adam DePrince'
copyright = '2026, Adam DePrince'
language = 'en'
root_doc = 'index'
html_theme = 'furo'
html_title = 'Silkie'
templates_path = ['_templates']
html_static_path = ['_static']
html_css_files = ['silkie.css']
html_show_sourcelink = False
html_show_sphinx = False
html_copy_source = False
exclude_patterns = ['_build', '_includes']
nitpicky = True

# Goblin Core's paper and ink, with deeper accents for text on light surfaces.
html_theme_options = {
    'light_css_variables': {
        'color-foreground-primary': '#12140f',
        'color-foreground-secondary': '#62645c',
        'color-foreground-muted': '#62645c',
        'color-foreground-border': '#79817a',
        'color-background-primary': '#f7f6f1',
        'color-background-secondary': '#eeede6',
        'color-background-hover': '#e9e8e1',
        'color-background-hover--transparent': '#e9e8e100',
        'color-background-border': '#d8d8cf',
        'color-background-item': '#d8d8cf',
        'color-brand-primary': '#416b06',
        'color-brand-content': '#416b06',
        'color-brand-visited': '#096c78',
        'color-code-background': '#eeede6',
        'color-code-foreground': '#12140f',
        'color-highlighted-background': '#e4efcf',
        'color-highlight-on-target': '#e4efcf',
        'color-admonition-background': '#fffefa',
        'color-accent-warning': '#aa4615',
    },
    'dark_css_variables': {
        'color-foreground-primary': '#f7f6f1',
        'color-foreground-secondary': '#c5c8be',
        'color-foreground-muted': '#aeb1a7',
        'color-foreground-border': '#79817a',
        'color-background-primary': '#11130f',
        'color-background-secondary': '#1b1e17',
        'color-background-hover': '#2b3024',
        'color-background-hover--transparent': '#2b302400',
        'color-background-border': '#45483f',
        'color-background-item': '#45483f',
        'color-brand-primary': '#a6e22e',
        'color-brand-content': '#a6e22e',
        'color-brand-visited': '#41c4d1',
        'color-code-background': '#1b1e17',
        'color-code-foreground': '#f7f6f1',
        'color-highlighted-background': '#34431e',
        'color-highlight-on-target': '#34431e',
        'color-admonition-background': '#1b1e17',
        'color-accent-warning': '#ff8a55',
    },
}

UPSTREAM = 'https://sw.kovidgoyal.net/kitty/'
UPSTREAM_DOCS = {
    'graphics-protocol': 'Terminal graphics protocol',
    'kittens/query_terminal': 'Query terminal',
}
UPSTREAM_OPTIONS = {'input_delay', 'repaint_delay', 'macos_colorspace'}
SILKIE_OPTIONS = (
    'hdr', 'hdr_max_headroom', 'cursor_brightness', 'wayland_colorspace',
    'eink_mode', 'color_scheme', 'display_info', 'tablet_hardware_ids', 'haptic_feedback',
)


def page_template(app: Sphinx, pagename: str, templatename: str, context: dict[str, object], doctree: nodes.document | None) -> str | None:
    if pagename == 'index':
        return 'home.html'
    return None


def upstream_reference(app: Sphinx, env: BuildEnvironment, node: nodes.Element, content: nodes.Element) -> nodes.reference | None:
    target = node.get('reftarget', '')
    kind = node.get('reftype')
    if kind == 'doc' and target in UPSTREAM_DOCS:
        if not node.get('refexplicit'):
            content = nodes.inline('', UPSTREAM_DOCS[target])
        url = f'{UPSTREAM}{target}/'
    elif kind == 'opt' and target in UPSTREAM_OPTIONS:
        url = f'{UPSTREAM}conf/#opt-kitty.{target}'
    else:
        return None
    return nodes.reference('', '', content, refuri=url, internal=False)


def configuration_reference(app: Sphinx, docname: str, source: list[str]) -> None:
    if docname != 'configuration':
        return
    path = Path(__file__).resolve().parents[2] / 'kitty/options/definition.py'
    app.env.note_dependency(str(path))
    options: dict[str, str] = {}
    # Read the declarations without importing the terminal or its native modules.
    for node in ast.walk(ast.parse(path.read_text())):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name) or node.func.id != 'opt' or len(node.args) < 2:
            continue
        name_node = node.args[0]
        if not isinstance(name_node, ast.Constant) or not isinstance(name_node.value, str):
            continue
        name = name_node.value
        if name not in SILKIE_OPTIONS:
            continue
        default = ast.literal_eval(node.args[1])
        description = next(ast.literal_eval(kw.value) for kw in node.keywords if kw.arg == 'long_text')
        body = f'Default: ``{default}``.' if default else 'Default: empty (no overrides).'
        body += '\n\n' + dedent(description).strip()
        options[name] = f'.. opt:: {name}\n\n' + indent(body, '   ') + '\n'
    missing = set(SILKIE_OPTIONS) - options.keys()
    if missing:
        raise ConfigError(f'Silkie configuration definitions missing: {sorted(missing)}')
    source[0] += '\n\n' + '\n'.join(options[name] for name in SILKIE_OPTIONS)


def setup(app: Sphinx) -> None:
    app.add_object_type('opt', 'opt', objname='configuration option')
    app.connect('missing-reference', upstream_reference)
    app.connect('source-read', configuration_reference)
    app.connect('html-page-context', page_template)
