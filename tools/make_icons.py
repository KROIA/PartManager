"""Generates PartManager's ribbon icons as 512x512 PNGs.

Style is copied from the icons the user supplied (new-part.png, tabelle.png,
plus.png): flat colour fills, heavy black outline, rounded joins, transparent
background, composite icons built as base-object + accent glyph.

Run:  python tools/make_icons.py [name ...]
With no arguments it writes every icon. Output goes to core/resources/icons/.
"""

import os
import sys

import cairosvg

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "core", "resources", "icons")

# Palette lifted from the supplied icons.
BLACK = "#000000"
GREY = "#CFDCE8"   # blue-grey panel fill
PAPER = "#F2F6FA"  # near-white page fill
CYAN = "#4EC8DC"
GREEN = "#4CAF50"
YELLOW = "#FBE18F"
SAGE = "#8CC98C"
TAUPE = "#A89986"
AMBER = "#E9A13B"
DEEP = "#A9C3D6"   # shaded face of the 3D cube

STROKE = 20  # main outline
THIN = 12    # inner detail lines


def svg(body):
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" '
        'viewBox="0 0 512 512">'
        '<g stroke="%s" stroke-width="%d" stroke-linejoin="round" '
        'stroke-linecap="round">%s</g></svg>' % (BLACK, STROKE, body)
    )


def outlined_stroke(d, colour, width):
    """A stroked path drawn twice: black underneath, colour on top.

    cairosvg has no stroke-outside, so the outline is a wider black copy of the
    same path. Cheaper than converting every stroke to a filled outline path.
    """
    return (
        '<path d="%s" fill="none" stroke="%s" stroke-width="%d"/>'
        '<path d="%s" fill="none" stroke="%s" stroke-width="%d"/>'
        % (d, BLACK, width + STROKE, d, colour, width)
    )


ICONS = {}

# 1. Refresh -- circular arrow, View group. Wired to reloadCategories().
#    The arrowhead sits at the arc's start and points along the direction of
#    travel, so the gap in the ring reads as "the arrow came from here".
#    Drawn as ONE filled silhouette -- ring sector with the arrowhead built into
#    the same path. A stroked arc plus a separate triangle leaves a seam where
#    the two outlines cross, and reads as a play button glued to a circle.
ICONS["refresh"] = (
    outlined_stroke("M 256 118 A 138 138 0 1 1 118 256", CYAN, 54)
    + '<polygon points="46,268 190,268 118,132" fill="%s"/>' % CYAN
)

# 2. Manage Tags -- luggage tag with its hole, Parts/Manage group. Wired.
#    The hole needs a thin stroke and a wide radius, or the 40px outline
#    closes it up into a black dot.
ICONS["manage-tags"] = (
    '<path d="M 250 62 H 452 V 264 L 262 454 L 60 252 Z" fill="%s"/>'
    '<circle cx="378" cy="136" r="46" fill="%s" stroke-width="%d"/>'
    % (YELLOW, PAPER, THIN)
)

# 3. Restock -- arrow down into a box, Home/Stock group.
#    The arrowhead ends deep inside the box (box is y=268..448, tip at 385) so
#    the overlap reads as "goes in". Stopping at the box's top edge, as the
#    first version did, just looked like the two shapes were touching.
ICONS["restock"] = (
    '<rect x="86" y="268" width="340" height="180" rx="18" fill="%s"/>'
    '<polygon points="216,110 296,110 296,270 356,270 256,385 156,270 216,270" '
    'fill="%s"/>' % (GREY, GREEN)
)

# 4. Take Out -- arrow up out of a box, Home/Stock group. Amber, not green, so
#    direction is not the only thing telling it apart from Restock.
#    Mirror of restock: same y range (110..385), tail buried in the box, head
#    clear of it -- "comes out".
ICONS["take-out"] = (
    '<rect x="86" y="268" width="340" height="180" rx="18" fill="%s"/>'
    '<polygon points="216,385 216,225 156,225 256,110 356,225 296,225 296,385" '
    'fill="%s"/>' % (GREY, AMBER)
)

# 5. List view -- stacked rows with bullets, Home/View toggle state A.
_rows = ""
for y in (92, 212, 332):
    _rows += ('<rect x="52" y="%d" width="96" height="96" rx="16" fill="%s" '
              'stroke-width="%d"/>' % (y, CYAN, THIN))
    _rows += ('<rect x="184" y="%d" width="276" height="96" rx="16" fill="%s" '
              'stroke-width="%d"/>' % (y, GREY, THIN))
ICONS["view-list"] = _rows

# 6. Grid view -- 2x2 tiles, Home/View toggle state B.
_tiles = ""
for x, y in ((76, 76), (272, 76), (76, 272), (272, 272)):
    _tiles += '<rect x="%d" y="%d" width="164" height="164" rx="20" fill="%s"/>' % (x, y, GREY)
ICONS["view-grid"] = _tiles

# 7. 3D viewer -- isometric cube, Home/View group.
ICONS["viewer-3d"] = (
    '<polygon points="256,64 434,166 256,268 78,166" fill="%s"/>'
    '<polygon points="78,166 256,268 256,466 78,364" fill="%s"/>'
    '<polygon points="434,166 434,364 256,466 256,268" fill="%s"/>'
    % (PAPER, GREY, DEEP)
)

# 8. Edit type templates -- a form being edited, Parts/Manage group.
ICONS["edit-type-template"] = (
    '<rect x="60" y="54" width="268" height="404" rx="22" fill="%s"/>'
    '<rect x="112" y="140" width="164" height="52" rx="18" fill="%s" stroke-width="%d"/>'
    '<rect x="112" y="238" width="164" height="52" rx="18" fill="%s" stroke-width="%d"/>'
    '<rect x="112" y="336" width="164" height="52" rx="18" fill="%s" stroke-width="%d"/>'
    '<g transform="rotate(34 372 296)">'
    '<rect x="332" y="128" width="82" height="62" rx="12" fill="%s"/>'
    '<rect x="332" y="184" width="82" height="196" fill="%s"/>'
    '<polygon points="332,376 414,376 373,452" fill="%s"/>'
    '</g>'
    % (PAPER, CYAN, THIN, CYAN, THIN, CYAN, THIN, AMBER, YELLOW, TAUPE)
)

# 9. Attach file -- paperclip, Parts/Files group.
#    Only two nested bends, not the usual three: at this outline weight a third
#    arm's black border merges with its neighbour and the clip turns into a blob.
#    Arms sit at x=146/274/366 so ~18px of black gap survives between them.
ICONS["attach-file"] = outlined_stroke(
    "M 366 172 V 366 a 110 110 0 0 1 -220 0 V 158 a 64 64 0 0 1 128 0 V 352",
    CYAN, 34)

# 10. Open datasheet -- page with a folded corner, Parts/Files group.
ICONS["open-datasheet"] = (
    '<path d="M 116 48 H 320 L 420 148 V 464 H 116 Z" fill="%s"/>'
    '<path d="M 320 48 L 420 148 H 320 Z" fill="%s" stroke-width="%d"/>'
    '<rect x="164" y="232" width="196" height="46" rx="18" fill="%s" stroke-width="%d"/>'
    '<rect x="164" y="314" width="196" height="46" rx="18" fill="%s" stroke-width="%d"/>'
    '<rect x="164" y="396" width="128" height="46" rx="18" fill="%s" stroke-width="%d"/>'
    % (PAPER, GREY, THIN, CYAN, THIN, CYAN, THIN, CYAN, THIN)
)


# 11. Home tab -- the supplied tab-home.png was a flat black silhouette, the one
#     icon that did not match anything else in the ribbon.
ICONS["tab-home"] = (
    '<rect x="116" y="240" width="280" height="216" rx="20" fill="%s"/>'
    '<polygon points="256,64 464,254 48,254" fill="%s"/>'
    '<rect x="216" y="332" width="80" height="124" rx="14" fill="%s" stroke-width="%d"/>'
    % (PAPER, AMBER, CYAN, THIN)
)

# 12. Mouser search -- a parts list under a magnifier. Item 7's embedded search
#     browser. The lens is translucent so the rows stay visible through it,
#     otherwise it is just a magnifier sitting on a blank card.
ICONS["mouser-search"] = (
    '<rect x="52" y="60" width="304" height="392" rx="22" fill="%s"/>'
    '<rect x="100" y="120" width="208" height="42" rx="14" fill="%s" stroke-width="%d"/>'
    '<rect x="100" y="196" width="208" height="42" rx="14" fill="%s" stroke-width="%d"/>'
    '<rect x="100" y="272" width="144" height="42" rx="14" fill="%s" stroke-width="%d"/>'
    % (PAPER, CYAN, THIN, CYAN, THIN, CYAN, THIN)
    + outlined_stroke("M 398 386 L 466 454", CYAN, 40)
    + '<circle cx="322" cy="310" r="114" fill="%s" fill-opacity="0.45"/>' % CYAN
)

# 13. Database -- the cylinder used by the database selector / Manage Databases.
ICONS["database"] = (
    '<path d="M 96 140 V 372 a 160 46 0 0 0 320 0 V 140 Z" fill="%s"/>'
    '<path d="M 96 218 a 160 46 0 0 0 320 0" fill="none" stroke-width="%d"/>'
    '<path d="M 96 296 a 160 46 0 0 0 320 0" fill="none" stroke-width="%d"/>'
    '<ellipse cx="256" cy="140" rx="160" ry="46" fill="%s"/>'
    % (CYAN, THIN, THIN, PAPER)
)

# 14. Settings -- gear for the App Settings dialog (item 12). Teeth are eight
#     rotated rounded rects with the hub circle painted over their inner ends,
#     which is a lot less arithmetic than a real 16-vertex gear outline.
_teeth = ""
for _i in range(8):
    _teeth += ('<rect x="228" y="48" width="56" height="128" rx="14" fill="%s" '
               'transform="rotate(%d 256 256)"/>' % (GREY, _i * 45))
ICONS["settings"] = (
    _teeth
    + '<circle cx="256" cy="256" r="150" fill="%s"/>' % GREY
    + '<circle cx="256" cy="256" r="66" fill="%s" stroke-width="%d"/>' % (PAPER, THIN)
)

# 15. Delete -- trash can, for part / tag / database row removal.
ICONS["delete"] = (
    '<rect x="204" y="54" width="104" height="52" rx="16" fill="%s"/>'
    '<path d="M 150 166 H 362 L 342 452 H 170 Z" fill="%s"/>'
    '<rect x="112" y="104" width="288" height="58" rx="18" fill="%s"/>'
    '<rect x="212" y="222" width="30" height="168" rx="14" fill="%s" stroke-width="%d"/>'
    '<rect x="270" y="222" width="30" height="168" rx="14" fill="%s" stroke-width="%d"/>'
    % (GREY, PAPER, GREY, CYAN, THIN, CYAN, THIN)
)

# 16. Import -- document with an arrow going into it, for item 8's CSV/BOM
#     import. Same green down-arrow as restock so "incoming" is one visual idea
#     across the app.
ICONS["import-csv"] = (
    '<path d="M 84 48 H 276 L 372 144 V 348 H 84 Z" fill="%s"/>'
    '<path d="M 276 48 L 372 144 H 276 Z" fill="%s" stroke-width="%d"/>'
    '<rect x="132" y="196" width="180" height="42" rx="14" fill="%s" stroke-width="%d"/>'
    '<rect x="132" y="264" width="120" height="42" rx="14" fill="%s" stroke-width="%d"/>'
    '<polygon points="246,300 326,300 326,384 386,384 286,494 186,384 246,384" fill="%s"/>'
    % (PAPER, GREY, THIN, CYAN, THIN, CYAN, THIN, GREEN)
)

# 17. Orders -- shopping cart for the Mouser cart / order view (item 9).
ICONS["orders"] = (
    '<path d="M 152 148 H 456 L 408 300 H 200 Z" fill="%s"/>' % CYAN
    + outlined_stroke("M 56 76 H 112 L 208 356 H 404", CYAN, 30)
    + '<circle cx="238" cy="432" r="46" fill="%s"/>' % GREY
    + '<circle cx="380" cy="432" r="46" fill="%s"/>' % GREY
)


def main():
    wanted = sys.argv[1:] or sorted(ICONS)
    os.makedirs(OUT_DIR, exist_ok=True)
    for name in wanted:
        if name not in ICONS:
            raise SystemExit("unknown icon: %s" % name)
        markup = svg(ICONS[name])
        svg_path = os.path.normpath(os.path.join(OUT_DIR, name + ".svg"))
        png_path = os.path.normpath(os.path.join(OUT_DIR, name + ".png"))
        with open(svg_path, "w", encoding="utf-8") as handle:
            handle.write(markup)
        cairosvg.svg2png(bytestring=markup.encode("utf-8"),
                         write_to=png_path, output_width=512, output_height=512)
        print("wrote", svg_path, "and", png_path)


if __name__ == "__main__":
    main()
