# PartManager

A desktop parts inventory for electronics: what components you own, how many are
left, what their datasheets and 3D models look like, what a project needs, and
what to order from Mouser to fill the gap. Built as a reusable C++/Qt library
(`core/`) with a Qt Widgets application on top.

![The main window: category tree, part table, and the preview panel with photo, KiCad symbol, footprint and the 3D model on a rendered PCB](docs/screenshots/main-window.png)

*The preview panel on the right is live: the part's photo, its KiCad schematic
symbol, its footprint, and its STEP model tessellated and placed on a board the
way KiCad would place it.*

---

## What it does

**Parts and categories.** Components live in a category tree of *type templates*
— Resistor, MOSFET, Logic IC — and a template defines which attributes a part of
that type has and which files it can carry. Templates inherit, so *Ceramic
Capacitor* gets everything *Capacitor* declares plus its own. Values are typed
the way an engineer writes them: `4k7`, `100n`, `4,7k` and `4.7k` all parse to
the same number, and searching compares against the parsed value rather than the
text.

**Stock.** Every restock and take-out is a logged transaction, not a number
someone overwrote, and a take-out records whether the parts went into a build or
were lost. The count you see is derived from that log.

**Tags, in two levels.** Tags cross-cut the category tree: a part is *SMD*, or
*I2C*, or *Needs datasheet*. They group into families — *Bus protocols*, *PCB
placement*, *Lifecycle*, *Voltage domain*, *Handling* — and each family's tags
are shades of one colour, so a table of chips stays readable. The tag filter ORs
within a family and ANDs across them: `tag:I2C,SPI tag:SMD` means *either bus,
and surface-mount*.

**Search.** One box, a small grammar: free text, `tag:` terms, and attribute
comparisons like `resistance>1k` or `voltage<=25V`. A malformed query is an empty
result and a red border, never a crash.

**Partlists and orders.** Build a partlist by hand or import a BOM/CSV with
column mapping, then let PartManager work out the shortfall against stock and
stage exactly that into a Mouser cart. Orders track through to arrival, and
prices observed along the way are kept as history.

**KiCad integration.** Generates per-category `.kicad_sym` symbol libraries and
`.pretty` footprint folders with the library tables to match, and tolerates you
editing them in KiCad — manual edits are tracked and synced back rather than
overwritten.

**3D models.** Any part can carry a 3D model. OBJ, PLY, STL and glTF are drawn
directly; STEP is tessellated once into a cached mesh in the background and drawn
from cache after that. Colours are read out of the STEP file itself, so a MOSFET
comes out with a black body and tinned leads rather than one lump of grey.

**Backups, settings, localization.** Scheduled database snapshots with a
retention count, and a settings screen for language, theme and storage. The
interface ships in **English and German** (the screenshot above is the German
build).

---

## Requirements

| | |
|---|---|
| **OS** | Windows (developed and tested on Windows 11; the library itself is portable, the build scripts are not) |
| **Compiler** | MSVC 2019 or newer, C++17 |
| **CMake** | 3.20 or newer |
| **Build tool** | Ninja (comes with Visual Studio) |
| **Qt** | 5.15.2, modules `Core Gui Network Widgets` — the app additionally needs `3DCore 3DRender 3DInput 3DExtras` |
| **FreeCAD** | *optional* — only to preview **STEP** models, see below |
| **Mouser API keys** | *optional* — only for search and cart features, see below |

Everything else is fetched automatically at configure time (no submodules, no
vcpkg, no conan): the KROIA `AppSettings`, `Logger`, `RibbonWidget`,
`SQLiteWrapper` and `UnitTest` libraries, plus `easy_profiler` for profiling
builds.

## Building

```bat
git clone https://github.com/KROIA/PartManager.git
cd PartManager
build.bat x64-Debug
```

`build.bat` bootstraps the Visual Studio environment itself via `vswhere`, so it
works from a plain terminal. Useful variants:

```bat
build.bat                      :: x64-Debug and x64-Release
build.bat x64-Release
build.bat list                 :: show the available presets
build.bat clean                :: wipe build output, keep the dependency cache
build.bat clean-all            :: wipe everything under build\ and installation\
build.bat help
```

Binaries land in `installation\bin\`. Run `PartManagerApp.exe` to start, or pass
a `.pmdb` file to open a database directly:

```bat
installation\bin\PartManagerApp.exe "C:\path\to\MyStock\MyStock.pmdb"
```

Without an argument the database selector opens first — PartManager deliberately
does not auto-resume the last database.

### Tests

```bat
installation\bin\ExampleTest.exe
```

One executable holding every suite (a custom KROIA test framework, not gtest or
Qt Test). It includes GUI tests that drive real dialogs, so it opens windows
while it runs.

### A database on disk

A "database" is a folder, not a single file:

```
MyStock/
├── MyStock.pmdb        JSON descriptor: schema version, timestamps
├── partmanager.db      the SQLite database itself
├── filestore/          datasheets, photos, KiCad files, 3D models, mesh cache
├── kicad_libs/         generated .kicad_sym / .pretty libraries
├── backups/            scheduled snapshots
└── README.md           free-text description, editable outside the app
```

Opening a folder written by an older build migrates it forward automatically.
Opening one written by a *newer* build is refused rather than half-read.

---

## Optional: FreeCAD, for STEP previews

Qt3D can load OBJ, PLY, STL and glTF on its own. **STEP** (`.step` / `.stp`) is
not a mesh — it is trimmed-NURBS boundary representation, and turning it into
something drawable needs a CAD kernel. Rather than link one (OpenCASCADE is a
large dependency for one panel), PartManager shells out to FreeCAD's
command-line binary once per model and caches the result.

**Not having FreeCAD is a normal state.** STEP files are still stored, still
attached to parts, and still exported to KiCad libraries — only the 3D preview is
unavailable, and the panel tells you so and lists where it looked.

1. Install [FreeCAD](https://www.freecad.org/) (1.x; tested with 1.1.3).
2. That's it — PartManager finds it by itself.

It searches, in order:

1. the `PARTMANAGER_STEP_CONVERTER` environment variable, if it points at an
   executable;
2. `C:\Program Files\FreeCAD *\bin\FreeCADCmd.exe` and the same under
   `Program Files (x86)` — globbed, so the version number does not matter;
3. `FreeCADCmd` on `PATH`.

If FreeCAD lives somewhere unusual, point at it explicitly:

```bat
setx PARTMANAGER_STEP_CONVERTER "D:\Tools\FreeCAD\bin\FreeCADCmd.exe"
```

Conversion runs in the background for every unconverted model while the app is
open, one at a time, so models are usually ready before you click them. The cache
is keyed by a hash of the STEP file's contents: replace the file and it converts
again; two parts sharing one model share one mesh.

---

## Optional: Mouser API setup

Mouser features (search, create-a-part-from-a-link, cart staging, order tracking)
need API keys. They are **read from environment variables only** — never stored
in the settings file, never written to disk by PartManager, and never logged.
There is deliberately no API-key field in the settings dialog.

There are **two separate keys**, and they are not interchangeable: the Search key
returns 401 on the Cart API.

| Variable | Key to request | Used for |
|---|---|---|
| `MOUSER_SEARCH_API` | **Search API** | keyword and part-number search, create-part-from-MPN or from a product link, price and stock lookup |
| `MOUSER_CART_API` | **Cart API** | staging a shortfall into a Mouser cart, order tracking |

### Getting the keys

1. Go to [mouser.com/api-hub](https://www.mouser.com/api-hub/) and sign in with
   your Mouser account.
2. Request a key for **Search API**. You are asked for a site/application name;
   approval is usually immediate and the key is shown in the API hub.
3. Repeat for **Cart API** if you want ordering. This is a second, different key.

### Setting them

Machine-wide (survives reboots; open a *new* terminal afterwards):

```bat
setx MOUSER_SEARCH_API "your-search-key-here" /M
setx MOUSER_CART_API   "your-cart-key-here"   /M
```

Or for the current user only, drop the `/M`. In PowerShell for one session:

```powershell
$env:MOUSER_SEARCH_API = "your-search-key-here"
```

> A running process does not see a variable set after it started, and `setx`
> does not touch already-open terminals. If PartManager reports the key as
> missing right after you set it, restart the app from a fresh terminal.

Without the keys everything else works normally; the Mouser screens explain
which variable is missing without printing any part of a key.

---

## Project layout

```
core/          the library — no widgets, no app logic
  database/      DatabaseHandle, SchemaMigrator, DatabaseRegistry
  domain/        plain data classes (Part, PartType, Tag, ...)
  persistence/   repositories (SQL lives here and nowhere else)
  units/         SI-prefix value parsing (§2a)
  search/        the filter-box grammar and its SQL
  filestore/     attachment storage
  mouser/        Search and Cart API clients
  kicad/         library generation, footprint/symbol geometry
  easyeda/       EasyEDA symbol/footprint fetch and conversion
  import/        BOM / CSV parsing and column mapping
  model3d/       format detection, STEP conversion, STEP colours
  backup/        scheduled snapshots
  settings/      preferences facade
examples/
  PartManagerApp/  the desktop app (ui/, widgets/, controllers/, services/)
  PartImport/      console CSV importer
unittests/       one executable, all suites
docs/design/     ARCHITECTURE.md (the spec), mockups/, prototype/
```

`docs/design/ARCHITECTURE.md` is the actual specification — numbered sections
that the code comments reference (`§2d`, `§7a`, ...). `docs/design/mockups/`
holds the original design mockups for all 14 screens, and
`docs/design/prototype/index.html` is a click-through of them.

## Status

Early but functional — version `0.0.0`, schema v8. The parts database, stock log,
tags, search, partlists, BOM import, Mouser search and ordering, KiCad library
generation, 3D viewing, backups and settings are implemented. See
`docs/design/ARCHITECTURE.md` §13 for what is deliberately deferred.

## License

No `LICENSE` file has been added to this repository yet, so default copyright
applies and nobody else may reuse the code. Add one before sharing it.

Built on the [KROIA CMake project template](https://github.com/KROIA).
