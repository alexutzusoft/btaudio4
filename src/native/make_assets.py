"""Generate the native app's fonts, icons, and package artwork.

Outputs (under build/native, the native build's -I include dir):
  ui_font.h  - anti-aliased glyph bitmaps at several native pixel sizes
  ui_icons.h - 8-bit alpha masks (tinted at runtime) + the logo tile (ARGB)
  icon0.png  - 512x512 PS4 home-screen icon
  pic1.png   - 1920x1080 PS4 launch splash

The same data is exposed through build_data() so the PC previewer renders
exactly what the console will.

Glyph bitmaps and splash text use the bundled Inter variable font.
"""
from io import BytesIO
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from resvg_py import svg_to_bytes

root = Path(__file__).resolve().parents[2]
out = root / 'build/native'
art = Path(__file__).resolve().parent / 'art'

FONT = art / 'InterVariable.ttf'

# Name, weight, native pixel size, and letter spacing in pixels.
FACES = [
    ('TITLE', 625, 39, -1.05),
    ('DIALOG', 630, 29, -0.65),
    ('SECTION', 625, 25, -0.45),
    ('BRAND', 650, 20, -0.45),
    ('LABEL', 600, 20, 0),
    ('KICKER', 550, 17, 0),
    ('NAME', 550, 19, 0),
    ('BODY', 400, 16, 0),
    ('SMALL', 400, 15, 0),
    ('SMALLB', 600, 16, 0),
    ('KEY', 550, 12, 0),
]
GAMMA = 0.90  # Slightly reinforce antialiased strokes on TV output.

def _font(weight, size):
    font = ImageFont.truetype(str(FONT), size)
    font.set_variation_by_axes([min(32, max(14, size)), weight])
    return font


# --- Fonts -------------------------------------------------------------------
def build_face(weight, size, tracking):
    font = _font(weight, size)
    asc, desc = font.getmetrics()
    lut = [min(255, round(255 * ((i / 255.0) ** GAMMA))) for i in range(256)]
    glyphs, bits = [], bytearray()
    for code in range(32, 127):
        ch = chr(code)
        adv16 = round((font.getlength(ch) + tracking) * 16)
        x0, y0, x1, y1 = font.getbbox(ch, anchor='ls')
        w, h = x1 - x0, y1 - y0
        if w <= 0 or h <= 0 or ch == ' ':
            glyphs.append((len(bits), adv16, 0, 0, 0, 0))
            continue
        im = Image.new('L', (w, h), 0)
        ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor='ls')
        off = len(bits)
        bits.extend(lut[v] for v in im.tobytes())
        glyphs.append((off, adv16, w, h, x0, y0))
    return dict(glyphs=glyphs, bits=bytes(bits), asc=asc, line=asc + desc)


# --- Icons (alpha masks) ------------------------------------------------------
# SVG paths from the approved screen, shared across sizes in the native atlas.
HEADPHONES = ('<path d="M3 14v-2a9 9 0 0 1 18 0v2'
              'M3 14a2 2 0 0 1 2-2h1a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1H5a2 2 0 0 1-2-2Z'
              'm18 0a2 2 0 0 0-2-2h-1a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h1a2 2 0 0 0 2-2Z"/>')
SPEAKER = '<rect x="6" y="3" width="12" height="18" rx="3"/><path d="M12 7h.01"/><circle cx="12" cy="14" r="2.5"/>'
PANEL_ICONS = [
    ('HERO', 42, HEADPHONES),
    ('HERO_SPEAKER', 42, SPEAKER),
    ('ROWHP', 23, HEADPHONES),
    ('DISPLAY', 22, '<rect x="3" y="5" width="18" height="12" rx="2"/><path d="M8 21h8m-4-4v4"/>'),
    ('LOUDSPEAKER', 23, SPEAKER),
    ('CHECK', 23, '<path d="m6 12 4 4 8-8"/>'),
    ('BANG', 20, '<path d="M12 5v9m0 5h.01"/>'),
    ('DOWNLOAD', 21, '<path d="M12 4v15m-5-5 5 5 5-5"/>'),
]
BUTTON_ICONS = [
    ('BCROSS', '<path d="m4 4 8 8m0-8-8 8"/>'),
    ('BCIRCLE', '<circle cx="8" cy="8" r="4.5"/>'),
    ('BSQUARE', '<rect x="3.5" y="3.5" width="9" height="9" rx=".6"/>'),
    ('BTRI', '<path d="M8 3.5 13 12.5H3Z"/>'),
]


def icon_mask(body, width, height, view_box, stroke):
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="{view_box}">'
           f'<g fill="none" stroke="#fff" stroke-width="{stroke}" stroke-linecap="round" '
           f'stroke-linejoin="round">{body}</g></svg>')
    with Image.open(BytesIO(svg_to_bytes(svg_string=svg))) as image:
        return image.getchannel('A')


def build_icons():
    icons = {
        name: icon_mask(body, size, size, '0 0 24 24', 1.65)
        for name, size, body in PANEL_ICONS
    }
    for name, body in BUTTON_ICONS:
        icons[name] = icon_mask(body, 15, 15, '0 0 16 16', 1.55)
    icons['BUPDOWN'] = icon_mask(
        '<path d="M5 12V4m-2.5 2.5L5 4l2.5 2.5M14 4v8m-2.5-2.5L14 12l2.5-2.5"/>',
        18, 16, '0 0 18 16', 1.2)
    dismiss = Image.new('L', (34, 15))
    dismiss.paste(icons['BCROSS'], (0, 0))
    dismiss.paste(icons['BCIRCLE'], (19, 0))
    icons['DISMISS'] = dismiss
    return icons


# --- Brand artwork -----------------------------------------------------------
def render_svg(name, mode, **size):
    png = svg_to_bytes(svg_path=str(art / name), skip_system_fonts=True,
                       font_files=[str(FONT)], **size)
    with Image.open(BytesIO(png)) as image:
        return image.convert(mode)


def build_icon512():
    mark = render_svg('btaudio4-logo.svg', 'RGBA', width=336, height=336)
    icon = Image.new('RGB', (512, 512), '#09090b')
    icon.paste(mark, (88, 88), mark.getchannel('A'))
    return icon


def build_data():
    faces = {name: build_face(*spec) for name, *spec in FACES}
    icons = build_icons()
    logo = render_svg('btaudio4-logo.svg', 'RGBA', width=25, height=25)
    splash = render_svg('btaudio4-launch-splash.svg', 'RGB')
    return dict(faces=faces, icons=icons, icon512=build_icon512(), logo=logo, splash=splash)


# --- C header emission ---------------------------------------------------------
def write_headers(data):
    out.mkdir(parents=True, exist_ok=True)
    lines = ['/* Generated by native/make_assets.py - do not edit. */',
             'struct glyph{unsigned off;unsigned short adv16;unsigned char w,h;signed char bx,by;};',
             'struct face{const struct glyph*g;const unsigned char*bits;int asc,line;};']
    for name in data['faces']:
        f = data['faces'][name]
        lines.append(f'static const unsigned char fb_{name}[]={{' + ','.join(map(str, f['bits'])) + '};')
        rows = ','.join('{%d,%d,%d,%d,%d,%d}' % g for g in f['glyphs'])
        lines.append(f'static const struct glyph fg_{name}[95]={{{rows}}};')
    lines.append('enum{' + ','.join(f'F_{n}' for n in data['faces']) + ',F_COUNT};')
    lines.append('static const struct face FACES[F_COUNT]={' + ','.join(
        '{fg_%s,fb_%s,%d,%d}' % (n, n, f['asc'], f['line']) for n, f in data['faces'].items()) + '};')
    (out / 'ui_font.h').write_text('\n'.join(lines) + '\n')

    lines = ['/* Generated by native/make_assets.py - do not edit. */',
             'struct icon{const unsigned char*a;unsigned short w,h;};']
    for name, im in data['icons'].items():
        lines.append(f'static const unsigned char ia_{name}[]={{' + ','.join(map(str, im.tobytes())) + '};')
    lines.append('enum{' + ','.join(f'I_{n}' for n in data['icons']) + ',I_COUNT};')
    lines.append('static const struct icon ICONS[I_COUNT]={' + ','.join(
        '{ia_%s,%d,%d}' % (n, im.width, im.height) for n, im in data['icons'].items()) + '};')
    logo = data['logo']
    pixels = iter(logo.tobytes())
    vals = [(a << 24) | (r << 16) | (g << 8) | b for r, g, b, a in zip(pixels, pixels, pixels, pixels)]
    lines.append(f'#define LOGO_W {logo.width}\n#define LOGO_H {logo.height}')
    lines.append('static const unsigned int logo_argb[LOGO_W*LOGO_H]={' + ','.join(map(str, vals)) + '};')
    (out / 'ui_icons.h').write_text('\n'.join(lines) + '\n')
    data['icon512'].save(out / 'icon0.png')
    data['splash'].save(out / 'pic1.png')


if __name__ == '__main__':
    d = build_data()
    write_headers(d)
    print('Native assets generated:', out)
    for name, face in d['faces'].items():
        print('  face', name, len(face['bits']), 'bytes')
    print('  icons', len(d['icons']), ' logo', d['logo'].size)
