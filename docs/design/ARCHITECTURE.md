# PartManager — Architecture & Feature Design

Status: design phase, no domain code written yet. Confirmed against user 2026-08-31 (see `.claude/ProjectManager/DECISIONS.md`).

Stack already vendored (FetchContent): Qt5 Widgets, `RibbonWidget` (KROIA), `SQLiteWrapper` (KROIA, "multi user environment" wrapper), `AppSettings` (KROIA).

## 1. Storage model

**Hybrid, and multiple independent databases** (confirmed — e.g. two separate companies' stock, kept fully apart). A "database" is a self-contained folder; the app can know about many, one is open at a time, switchable without restarting:

```
MyCompanyA/                         (folder name = the database's display name)
  MyCompanyA.pmdb                   (entry file, JSON — §1a. What you actually open/double-click.)
  README.md                         (free-text description, plain file, editable outside the app too)
  partmanager.db                    (SQLite, WAL mode)
  filestore/<hash[0:2]>/<hash>.<ext>   (content-addressed, auto-dedup)
  kicad_libs/
    PartManager_Resistors.kicad_sym
    PartManager_Resistors.pretty/
    ...one pair per category...
    3dmodels/
    partmanager-sym-lib-table       (generated, user adds once to KiCad's table)
    partmanager-fp-lib-table
  backups/                         (§9a — scheduled partmanager.db snapshots)
```

Whole folder is the backup unit (zip it, done) — one per database. DB never stores BLOBs — keeps it small/fast, and lets KiCad/OS tools open library files directly instead of round-tripping through an export step.

`part_file.content_hash` catches silent corruption/swaps; same hash = same file = stored once regardless of how many parts reference it (e.g. a shared 3D model for a package used by 50 parts).

There is deliberately no separate "name" field anywhere — the display name is always the current folder name, re-read live, so renaming the folder in Explorer just works instead of needing to stay in sync with a duplicated stored name.

### 1a. Entry file (`<name>.pmdb`) & the internal `db_meta` table

Opening a database means picking its `.pmdb` file, not its folder — deliberately: a file (not a folder) can be **double-clicked** (once `.pmdb` is registered as a file association) to launch the app straight into that database, and a file picker filtered to `*.pmdb` is unambiguous in a way "pick the right folder" isn't. The entry file sits right next to `partmanager.db` inside the database's own folder; every other path (`filestore/`, `kicad_libs/`, `backups/`, `partmanager.db` itself) is a fixed sibling name resolved relative to wherever the `.pmdb` file actually is — so the folder is fully relocatable/renamable as a unit.

```json
{
  "schema_version": 7,
  "tool_version_last_saved": "0.3.0",
  "created_at": "2026-01-10T12:00:00",
  "last_opened_at": "2026-08-31T09:00:00"
}
```

`schema_version` is a plain increasing integer describing the SQL **structure** (tables/columns) only — never the data inside it, exactly as specified. It's duplicated inside the DB itself as the authoritative copy:

```sql
CREATE TABLE db_meta (key TEXT PRIMARY KEY, value TEXT);
-- seeded with: schema_version, created_at, tool_version_last_saved
```

The entry file is a **fast cache** of the same numbers so the database picker (§1b) can show a schema-version/compat badge for every known database without opening each one's SQLite file — `db_meta` inside the DB is what's trusted if the two ever disagree (e.g. someone copied the folder by hand and the `.pmdb` went stale), and the entry file gets silently rewritten to match on next open.

### 1b. Database registry, startup, and switching

The list of *known* databases (their `.pmdb` paths + last-opened time) is itself app-wide state, not inside any single database — stored via `core/settings` (the `AppSettings` facade), since it has to exist before any database is even open.

- **App startup**: always shows a **database selector** (confirmed — no auto-resume-last-used skip), listing every known database (name from its folder, description from `README.md`, schema-version badge, last opened) — Open, Browse for existing..., New Database..., or Manage.
- **Switch Database** (right-click context menu, confirmed) — same list, reachable anytime without restarting; switching closes the current `DatabaseHandle` (autosave already means nothing is lost, §10) and opens the new one.
- **Manage Databases...** (same context menu) — the same list plus: **Browse...** (file picker filtered to `*.pmdb` — this *is* "browse for a database to import": you navigate into that database's folder and pick the entry file sitting inside it, which registers it in the list without moving/copying anything), **New Database...** (pick a parent folder + name → creates the folder, `partmanager.db`, `README.md`, seeds default type templates, writes the entry file), edit description (`README.md`), **Remove from list** (forgets it, touches no files), **Delete** (actually removes the folder — destructive, confirmed separately, well clear of "Remove from list").

Mocked up in `docs/design/mockups/database-selector.svg`.

### 1c. Version compatibility — forward migration only, no downgrades

On opening a database, compare its `schema_version` (from `db_meta`, §1a) against the running app's compiled-in current schema version:

- **Equal** → open normally.
- **Lower** → back up first (§9a — always snapshot before a schema migration, this is the single riskiest mutation an install ever does), then run forward migrations in order up to current, update `db_meta` and the `.pmdb` cache.
- **Higher** → **refuse to open.** Clear error: *"This database was last saved by a newer PartManager (schema vN, tool vX.Y.Z) — this copy only supports up to schema vM. Update PartManager to open it."* No downgrade migration, ever, exactly as specified — a database always requires a tool version at least as new as whatever last wrote to it.

## 2. Component type model

Rejected: one SQL table per type (Resistor table, Capacitor table, Screw table, ...) — N wildly different schemas means N migrations forever, and cross-type queries (e.g. "everything under 2€") need N-way UNIONs.

Rejected: pure EAV (one giant key/value attribute table) — flexible but slow and unvalidated; a "resistance > 1k" query becomes a self-join nightmare.

**Chosen: type templates + JSON, with generated columns for the fields that need to be fast.**

```sql
CREATE TABLE part_type (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,                 -- 'Resistor', 'Screw', 'Wood Sheet'
    domain TEXT NOT NULL,               -- 'electronic' | 'mechanical' | 'generic'
    kicad_relevant INTEGER NOT NULL DEFAULT 0,
    kicad_category TEXT,                -- groups types into one KiCad library file, e.g. 'Resistors'
    parent_type_id INTEGER REFERENCES part_type(id),
    description TEXT
);

CREATE TABLE part_type_attribute (
    id INTEGER PRIMARY KEY,
    part_type_id INTEGER NOT NULL REFERENCES part_type(id),
    key TEXT NOT NULL,                  -- 'resistance_ohm', 'thread_size_mm'
    label TEXT NOT NULL,                -- 'Resistance'
    unit TEXT,                          -- picked from a fixed dropdown (§2a) — 'Ω', 'F', 'mm', ... or NULL for "no unit"
    datatype TEXT NOT NULL,             -- 'number' | 'dimension' | 'text' | 'bool' | 'enum'
    enum_options TEXT,                  -- JSON array, only for 'enum'
    searchable INTEGER NOT NULL DEFAULT 0,  -- 1 => gets an app-maintained indexed column
    required INTEGER NOT NULL DEFAULT 0,    -- 1 => New Part screen blocks creation until filled
    tooltip TEXT,                       -- shown as an (i) hint next to the field on the New Part screen
    sort_order INTEGER NOT NULL DEFAULT 0,
    UNIQUE(part_type_id, key)
);

-- Same idea as part_type_attribute, but for expected *file* attachments rather than data fields
-- (e.g. Resistor: Datasheet optional; Screw: CAD Model required).
CREATE TABLE part_type_file_slot (
    id INTEGER PRIMARY KEY,
    part_type_id INTEGER NOT NULL REFERENCES part_type(id),
    role TEXT NOT NULL,                 -- matches part_file.role, e.g. 'datasheet'|'kicad_3dmodel'|'cad_model'|'photo'
    label TEXT NOT NULL,
    required INTEGER NOT NULL DEFAULT 0,
    tooltip TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0,
    UNIQUE(part_type_id, role)
);

CREATE TABLE part (
    id INTEGER PRIMARY KEY,
    part_type_id INTEGER NOT NULL REFERENCES part_type(id),
    name TEXT NOT NULL,
    manufacturer TEXT,
    mpn TEXT,
    description TEXT,
    package TEXT,                       -- 'SOIC-8', 'M3x10', ...
    attributes TEXT NOT NULL DEFAULT '{}',  -- JSON, validated against part_type_attribute at save time
    datasheet_file_id INTEGER REFERENCES part_file(id),
    stock_qty INTEGER NOT NULL DEFAULT 0,   -- cached, derived from stock_transaction sum
    stock_min_qty INTEGER NOT NULL DEFAULT 0,  -- reorder threshold
    storage_location TEXT,              -- placeholder, unused for now — see §2b for what's deferred
    is_active INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
```

Per-type "fast filter" columns are added on demand when an attribute is marked `searchable` — e.g. for Resistor's `resistance_ohm`:

```sql
ALTER TABLE part ADD COLUMN attr_resistance_ohm REAL;   -- plain column, app-maintained (see 2a)
CREATE INDEX idx_part_resistance ON part(attr_resistance_ohm) WHERE part_type_id = <resistor_type_id>;
```

Result: schema stays one table for every domain (electronics today, screws/woodsheets later, no code change to add a type), full-attribute UI is generated at runtime from `part_type_attribute`, and the handful of fields you actually filter/sort by get real indexes. Migration only needed for the rare *structural* change (new column class), never for "user added a new component category."

### 2a. Numeric/dimensioned attributes: unit dropdown, entry parsing & search

**Defining a type's attribute** (`type-template-editor.svg`): `unit` is picked from a fixed dropdown, not typed — this is what lets the app know which SI prefixes are valid and what the base-SI unit is. "(no unit)" is always the first option, for plain counts/ratios that never take a prefix.

| Unit (dropdown) | Base-SI unit | Physical quantity |
|---|---|---|
| (no unit) | — | plain number, e.g. pin count |
| Ω | Ω | resistance |
| F | F | capacitance |
| H | H | inductance |
| V | V | voltage |
| A | A | current |
| W | W | power |
| Hz | Hz | frequency |
| s | s | time |
| % | % | ratio (never SI-prefixed in practice, but parser allows it) |
| mm | mm | length (mechanical parts) |

**SI prefixes** recognized on entry, same table used everywhere a number is typed (New Part screen, search bar):

| Prefix | Multiplier | Example |
|---|---|---|
| p (pico) | ×10⁻¹² | `4.7p` = 0.0000000000047 |
| n (nano) | ×10⁻⁹ | `10n` = 0.00000001 |
| u / µ (micro) | ×10⁻⁶ | `4.7u` = 0.0000047 |
| m (milli) | ×10⁻³ | `1m` = 0.001 |
| *(none)* | ×1 | `100` = 100 |
| k (kilo) | ×10³ | `1k` = 1000 |
| M (mega) | ×10⁶ | `1M` = 1000000 |
| G (giga) | ×10⁹ | `1G` = 1000000000 |

**Decimal separator**: `.` is canonical everywhere (storage, display). `,` is accepted as an alternate decimal separator wherever a number is typed — data entry *and* search — and is normalized to `.` on commit (field loses focus / value autosaves), not on every keystroke, so the cursor doesn't jump mid-type. Because `,` always means "decimal point" under this rule, it can never also mean a thousands-grouping separator — grouping only ever uses `'` (e.g. `100'000`), never `,`.

**Entry parser**, used on every dimension-typed field (Resistance, Capacitance, ...): accepts a bare number (`100` → 100, in the field's declared unit — a Resistor's Resistance field with no suffix at all means Ω, so `100` = 100Ω exactly as asked), a number with a prefix letter directly attached (`1k` → 1000, `4.7u` → 0.0000047), or the electronics engineering shorthand where the prefix letter *replaces* the decimal point (`4k7` → 4700, `2m2` → 0.0022) — all three forms resolve to the same stored value, and an explicit unit suffix (`1kΩ`, `100uF`) is accepted but never required since the field already knows its own unit. Whatever the user typed is kept as entered (`{value: 4.7, unit: "u"}`-style) for display/round-tripping; the base-SI number is what gets written into the field's `attr_*` column (§2).

Each `dimension`-typed attribute is stored in `attributes` JSON as an object, not a bare number, because the *natural* prefix varies part-to-part even within one type (a 100nF ceramic cap and a 4700µF electrolytic are both "Capacitance" on the Capacitor template, entered in whichever prefix the datasheet uses):

```json
{ "resistance": { "value": 4700, "unit": "Ω" },
  "tolerance":  { "value": 1,    "unit": "%"  } }
```

At save time the app (not SQLite) resolves the prefix through the table above and writes the **base-SI value** into the matching plain `attr_*` column declared in §2 (e.g. `attr_resistance_ohm = 4700`, always in Ω; capacitance columns always in Farad, etc.). That column is what gets indexed and queried — the JSON stays the human-entered `{value, unit}` for display/round-tripping in the editor.

**Search bar parsing** (Home tab, §7) reuses the same entry parser, turning free text into a query in two passes, tried in order:

1. **Exact SI-scaled match** — triggered when the query has a recognized SI prefix and/or unit suffix (`100k`, `4k7`, `100uF`, `100uf`), *or* is a plain number with thousands separators (`100000`, `100'000`). Parsed to one canonical base-SI number (`100k` → 100000, `100uF` → 0.0001) and compared for equality (with a small relative tolerance, e.g. ±0.5%, to absorb float rounding) against every `attr_*` column — restricted to attributes whose dropdown unit matches when an explicit unit letter was given (`100uF` only ever checks Capacitance-unit (F) columns), unrestricted when only a bare SI-prefixed magnitude was given (`100k` checks every dimensioned column for base-SI value 100000, whatever physical quantity it happens to belong to).
2. **Bare mantissa match** — triggered only when the query is a plain unscaled number with no prefix/unit/separators (`100`, `47`, `10`). Compares the query's significant-digit pattern (leading zeros/trailing zeros stripped, i.e. normalized scientific-notation mantissa) against each `attr_*` value's own mantissa, ignoring magnitude entirely. This is the deliberately loose "ballpark value" search — `100` matches a 100Ω resistor *and* a 100µF capacitor (mantissa `1` in both), because at a glance you're often thinking "the part labeled 100-something," not which unit it's in. Phase 2 of implementation (phase 1 = the exact-scaled pass above, which covers the precise `100k`/`100000`/`100'000` equivalence and is unambiguous on its own).

Both passes run against attribute values regardless of which `part_type` they belong to — a query never needs to know it's "searching resistors," it just matches whatever numeric attributes exist. Text/enum/bool attributes and the fixed columns (`name`, `manufacturer`, `mpn`, `package`) are matched separately with a plain case-insensitive substring search, OR'd into the same result set.

### 2b. Type inheritance (`parent_type_id`)

Confirmed: real inheritance, not just flat types — e.g. "Ceramic Capacitor" and "Electrolytic Capacitor" both extend "Capacitor," adding their own attributes on top of the shared ones (Capacitance, Voltage) rather than redeclaring them.

**Resolution rule** for "what does type X actually look like" (attributes, file slots, *and* list columns — all three tables follow the same rule): walk from the root ancestor down to X, collecting rows keyed by `key` (or `role` for file slots); a row on a more specific type **overrides** an ancestor's row of the same key (label/unit/required/tooltip/searchable can all be redefined by a subtype), a new key is simply added. Effective ordering is ancestor attributes first (by their own `sort_order`), then the type's own new/overridden ones — a subtype can't currently reorder an inherited attribute relative to its ancestor's others, only override its properties or append new ones after (revisit if that turns out to matter in practice).

`kicad_category` and `domain` inherit the same way (a subtype with no `kicad_category` of its own falls back to its parent's — Ceramic Capacitor parts land in the same "Capacitors" library as any other capacitor, unless explicitly pointed elsewhere).

**Type template editor** (`type-template-editor.svg`, to be updated) needs to show inherited rows distinctly from the type's own (e.g. greyed-out with a small "from Capacitor" tag) so it's obvious at a glance which rows are editable-in-place versus which need an override row created first.

**Storage:** no schema change beyond the existing `part_type.parent_type_id` — resolution happens app-side at read time (or is cached/flattened into a derived view if it turns out to be queried hot enough to matter; not needed for the data volumes this app deals with).

### 2c. Storage location (deferred, placeholder only)

`part.storage_location` exists in the schema but is intentionally left unused for now — no bin/shelf modeling, no barcode scanning, nothing decided about *how* physical location will eventually be tracked (a printed barcode on the storage chest plus a free-text location description is the leading idea, not committed). The column stays hidden by default in every type's `part_type_list_column` seed (`visible = 0`) until this is actually designed.

### 2d. Tags

Problem: `part_type` is a single tree slot, but a part often genuinely belongs to more than one "findable-by" grouping — an LED is a Diode by type, but someone hunting for it thinks "Light source," "Red," "THT." Forcing one category per part loses the others. Tags are the cross-cutting, many-to-many answer: independent of `part_type_id`, searchable, clickable.

```sql
CREATE TABLE tag (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,          -- 'LED', 'THT', 'SMD', 'Red', 'Light source', ...
    color TEXT NOT NULL,                -- hex, e.g. '#E53935' — banner background
    sort_order INTEGER NOT NULL DEFAULT 0
);

-- Default tags a type template seeds onto every NEW part created under it.
CREATE TABLE part_type_tag (
    part_type_id INTEGER NOT NULL REFERENCES part_type(id),
    tag_id INTEGER NOT NULL REFERENCES tag(id),
    PRIMARY KEY(part_type_id, tag_id)
);

-- Actual tags on one part instance — independently editable after creation.
CREATE TABLE part_tag (
    part_id INTEGER NOT NULL REFERENCES part(id),
    tag_id INTEGER NOT NULL REFERENCES tag(id),
    PRIMARY KEY(part_id, tag_id)
);
```

**Tag management:** predefined, user-managed list (create/rename/recolor/delete a tag), same idea as the type template editor — a small dialog, not per-part free text, so tags stay a closed, consistent vocabulary instead of drifting into near-duplicates ("SMD" / "smd" / "surface mount").

**Inheritance rule (one-time seed, not a live link):** when a new `part` is created under a `part_type`, its `part_tag` rows are seeded by copying that type's resolved `part_type_tag` set — resolved the same way as §2b (ancestor tags ∪ the type's own; unlike attributes there's no override-by-key, tags just union since there's nothing to conflict). After that copy, the part's tags are fully independent: the user can remove an inherited tag or add new ones on that one part, and later edits to `part_type_tag` do **not** retroactively touch existing parts — same one-time-seed semantics already used for `part_type_list_column` (§7b).

**Rendering:** small colored banners/chips next to a part's name, wherever a part is listed (Home table row, part editor header) — background from `tag.color`, text from `tag.name`.

**Search integration:** reuses the existing search engine (§2a/§7a) rather than adding a parallel one — a tag name typed in either search box is OR'd into the same substring/attribute match already run, and clicking a tag chip anywhere is shorthand for "search: that tag name," filtering the Home-tab table to every part carrying it (cross-category, since tags don't belong to one type).

**UI touch points** (existing screens gain a control, no new top-level screen needed):
- `type-template-editor.svg` — a "Default tags" section alongside attributes/file slots, picking from the managed tag list.
- `part-editor.svg` — a tag-chip row under the header, editable (add via the managed list, remove with an ×).
- `main-window.svg` — chips rendered in the part table rows; clicking one filters the table to that tag.
- New small dialog: **Manage Tags** (name/color/sort_order CRUD), reachable from Settings or the Parts ribbon tab's *Manage* group.

**Module placement:** `Tag`/`PartTypeTag`/`part_tag` follow the same split already used for types — domain class under `core/domain`, `TagRepository` under `core/persistence` (CRUD on `tag`, `part_type_tag`, `part_tag`; seed-on-create helper mirroring `PartTypeRepository::seedDefaultTypes()`-style methods already in the codebase). No new module needed.

## 3. Files, sellers, pricing, stock history

```sql
CREATE TABLE part_file (
    id INTEGER PRIMARY KEY,
    part_id INTEGER NOT NULL REFERENCES part(id),
    role TEXT NOT NULL,                 -- 'datasheet'|'kicad_symbol'|'kicad_footprint'|'kicad_3dmodel'|'image'|'other'
    relative_path TEXT NOT NULL,        -- under filestore/
    content_hash TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    mime_type TEXT,
    original_filename TEXT,
    added_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE seller (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,                 -- 'Mouser', 'Digikey', 'Local'
    api_type TEXT NOT NULL DEFAULT 'manual'  -- 'mouser' | 'manual' | 'other'
);

CREATE TABLE part_seller_link (
    id INTEGER PRIMARY KEY,
    part_id INTEGER NOT NULL REFERENCES part(id),
    seller_id INTEGER NOT NULL REFERENCES seller(id),
    seller_part_number TEXT,
    url TEXT,
    is_primary INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE price_history (
    id INTEGER PRIMARY KEY,
    part_seller_link_id INTEGER NOT NULL REFERENCES part_seller_link(id),
    observed_at TEXT NOT NULL DEFAULT (datetime('now')),
    quantity_break INTEGER NOT NULL,    -- price-break qty, e.g. 1/10/100
    unit_price REAL NOT NULL,
    currency TEXT NOT NULL DEFAULT 'EUR',
    source TEXT NOT NULL                -- 'mouser_api_quote' | 'paid'
);

CREATE TABLE stock_transaction (
    id INTEGER PRIMARY KEY,
    part_id INTEGER NOT NULL REFERENCES part(id),
    delta_qty INTEGER NOT NULL,         -- +restock, -checkout/-loss
    reason TEXT NOT NULL,               -- 'restock'|'checkout_partlist'|'manual_adjust'|'loss'|'initial'
    ref_partlist_id INTEGER REFERENCES partlist(id),
    ref_order_id INTEGER REFERENCES mouser_order(id),
    unit_cost REAL,                     -- what you actually paid, restock only
    currency TEXT,
    note TEXT,
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
);
```

`part.stock_qty` is a cache recomputed from `SUM(stock_transaction.delta_qty)` — the transaction log is the source of truth, so "value of wealth" (`Σ current_qty × last paid unit_cost`) and "stock over time" charts both fall out of one table for free, with `source='paid'` rows in `price_history` distinct from `source='mouser_api_quote'` reference quotes.

## 4. Partlists & orders

```sql
CREATE TABLE partlist (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT,
    project_link_url TEXT,
    multiplier INTEGER NOT NULL DEFAULT 1,   -- PCB production count
    source TEXT NOT NULL DEFAULT 'manual',   -- 'manual'|'kicad_import'|'csv_import'
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE partlist_item (
    id INTEGER PRIMARY KEY,
    partlist_id INTEGER NOT NULL REFERENCES partlist(id),
    part_id INTEGER REFERENCES part(id),     -- NULL until an unresolved import row is matched
    designators TEXT,                        -- 'R1,R2,R5'
    quantity_per_unit INTEGER NOT NULL,
    raw_import_data TEXT                     -- original row JSON, kept for unresolved matches
);

CREATE TABLE mouser_order (
    id INTEGER PRIMARY KEY,
    partlist_id INTEGER REFERENCES partlist(id),
    status TEXT NOT NULL DEFAULT 'draft',    -- draft|staged_in_cart|submitted|partially_arrived|closed
    mouser_cart_id TEXT,
    mouser_order_number TEXT,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    submitted_at TEXT,
    closed_at TEXT
);

CREATE TABLE mouser_order_item (
    id INTEGER PRIMARY KEY,
    mouser_order_id INTEGER NOT NULL REFERENCES mouser_order(id),
    part_id INTEGER NOT NULL REFERENCES part(id),
    quantity_ordered INTEGER NOT NULL,
    unit_price REAL,
    currency TEXT,
    quantity_received INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'pending'   -- 'pending'(orange) | 'arrived'(green) | 'backordered'
);
```

Partlist checkout flow: open partlist → resolve any unmatched rows to real `part` records → "Check stock" diffs `quantity_per_unit × multiplier` against `part.stock_qty` → shortfall lines get pushed into a `mouser_order` draft → per-line qty editable → "Stage to Mouser Cart" (§6) → order sits `staged_in_cart`/`submitted` until arrivals are confirmed line-by-line (orange→green) → `closed` when all lines arrived (or force-closed with remainders marked backordered/cancelled) → on close, each arrived line writes a `stock_transaction(reason='restock', unit_cost=<user-entered paid price>)`.

Individual takeout (broken/lost part, not tied to a partlist) is just a manual `stock_transaction(reason='loss'|'manual_adjust', ref_partlist_id=NULL)`.

## 5. KiCad integration

### 5a. Library generation (works today, no KiCad API needed)

One `.kicad_sym` + one `.pretty` footprint dir **per `part_type.kicad_category`** (Resistors, Capacitors, ICs, ...), regenerated whenever a part with `kicad_relevant=1` changes. Each generated symbol carries:

- Footprint field pre-assigned (→ `.pretty` in the same category).
- 3D model field pointing into `kicad_libs/3dmodels/` (`${KIPRJMOD}`-relative or an env-var-based path so it's portable across machines).
- Custom fields: `PM_PartID`, `Datasheet` (from `part_file`), `Mouser P/N`.

PartManager writes its own `partmanager-sym-lib-table` / `partmanager-fp-lib-table`; **one-time setup** the user adds those as a single entry in KiCad's global library tables (env-var path). Every future regeneration is then invisible to KiCad's config — the files just update in place.

Placing a part in a schematic is then the completely normal KiCad workflow (press `A`, pick from the "PartManager" library) — already wired to footprint + 3D model + datasheet. No dependency on KiCad's API maturity for the one feature (schematic placement) it can't yet do officially.

**Confirmed: regeneration must tolerate manual edits made directly in KiCad** (fixing a pin, nudging a courtyard, etc.) — it's not safe to treat the generated files as pure disposable build output. Tracked per generated artifact, not per file:

```sql
CREATE TABLE kicad_generated_item (
    id INTEGER PRIMARY KEY,
    part_id INTEGER NOT NULL REFERENCES part(id),
    item_type TEXT NOT NULL,            -- 'symbol' | 'footprint'
    target_path TEXT NOT NULL,          -- symbol's entry inside the .kicad_sym, or the .kicad_mod file path
    last_generated_hash TEXT NOT NULL,  -- hash of the content PartManager itself last wrote
    last_generated_at TEXT NOT NULL
);
```

Regeneration logic per artifact: hash what's **currently on disk** and compare to `last_generated_hash` (not to a freshly-generated candidate) —
- **Match** → on-disk content is still exactly what PartManager last wrote, safe to overwrite with the newly generated version.
- **Mismatch** → someone hand-edited it in KiCad since the last generation. Skip overwriting that artifact, surface it (KiCad tab, and on the part itself) as "modified externally — not auto-updated," and let the user either **force-regenerate** (discard the manual edit) or explicitly **re-baseline** (accept the on-disk version as the new `last_generated_hash`, so it stops being flagged, without changing it).

Footprints are naturally per-file (`.pretty/<name>.kicad_mod`) so this is a straight per-file hash check. Symbols all live in one `.kicad_sym` per category, so the generator has to parse the file's per-symbol s-expressions, apply this match/mismatch check per symbol, and reassemble the file from [freshly generated symbols] + [preserved on-disk symbols verbatim] — the file as a whole still gets rewritten every regeneration, individual symbol bodies don't, unless they matched.

### 5b. PCB-side plugin (KiCad 10 IPC API, `kicad-python`)

KiCad 9/10's official IPC API only covers the **PCB editor** — schematic scripting is still unofficial/experimental, so nothing here assumes live schematic control. `plugin.json` + Python entry point, connects via `KICAD_API_SOCKET`/`KICAD_API_TOKEN` env vars KiCad injects. Reads the same SQLite DB directly (WAL mode → safe concurrent reader while the main app has it open). Scope:

- **Verify footprints/3D models** on the open board against PartManager's canonical assignment, flag drift.
- **Pull BOM straight from the board** (IPC API exposes footprints/nets on the PCB side) as a convenience path into PartManager's partlist import — *not* the primary path.

Primary path for getting a partlist out of KiCad, on every version, no API dependency: KiCad's built-in BOM/netlist export (Tools → Generate BOM, or plugin-based BOM generators) → CSV/XML → PartManager's generic importer (column-mapping UI, not hardcoded to one exporter's layout, so Excel/other-tool exports work through the same path). Full schematic-side "click to place at cursor" plugin becomes possible once KiCad ships official schematic IPC support — revisit then.

## 6. Mouser integration

Grounded against the real spec, `.claude/MouserAPI/MouserAPI_V1.json` (Swagger/OpenAPI 2.0, host `api.mouser.com`) — not guessed. Two separate API keys, matching the two the user already has (pending Mouser approval as of 2026-08-31 — `MOUSER_SEARCH_API` likely already active, `MOUSER_CART_API` may not be yet; live-tested once both are confirmed working, not blocking the design):

- **Search API** (`MOUSER_SEARCH_API`, sent as the `apiKey` query parameter) — `POST /api/v{version}/search/partnumber` (`SearchByPartRequest{ mouserPartNumber }`) or `.../search/keyword` (`SearchByKeywordRequest`). Response (`SearchResponse.Parts[]`, `MouserPart`) gives exactly what's needed: `ManufacturerPartNumber`, `Manufacturer`, `Description`, `Category`, `DataSheetUrl`, `ProductDetailUrl` (the part's actual mouser.com page — this is the URL every "🌐 Open on Mouser" button in the UI opens), `PriceBreaks[{Quantity,Price,Currency}]`, `Availability`/`AvailabilityInStock`, and `ProductAttributes[{AttributeName,AttributeValue}]` (Mouser's own spec table — mapped into `part_type_attribute` values by matching `AttributeName` against a type's attribute labels, unmapped ones left for manual entry, exactly "auto-fill what's possible, correct the rest").
- **Datasheet auto-download**: `DataSheetUrl` from the search result → fetched into `filestore/`, linked as `part_file(role='datasheet')`.
- **KiCad symbol/footprint/3D model**: **not** available from Mouser's API — no auto-download for these; they're either hand-made or pulled from a future SnapEDA/Ultra Librarian/manufacturer integration (explicitly out of MVP scope, flagged here so it isn't oversold).
- **Embedded Mouser search browser** (`mouser-search.svg`, confirmed feature): a keyword/part-number search box built into the New Part flow, results table straight from the Search API response, each row has **"Use this part"** (feeds the row into the same prefill/review form `new-part-mouser.svg` already renders) and **"🌐 Open on Mouser"** (opens that row's `ProductDetailUrl` in the system default browser for full detail — images, alternates, everything the app doesn't try to replicate). The same "🌐 Open" button appears next to any Mouser seller link already on a part (`part-editor.svg`), using the seller link's stored `ProductDetailUrl`.
- **Cart staging** (confirmed choice): `POST /api/v{version}/cart` (`MouserCart_UpdateCart`, `apiKey` = `MOUSER_CART_API`) builds the real Mouser cart from the order's line items (`CartItemRequest{ MouserPartNumber, Quantity }`); the response's `CartKey` (a UUID) is stored as `mouser_order.mouser_cart_id`. User opens mouser.com (or the app), reviews, and places the order there themselves — app never touches payment/checkout, and never calls the separate Order API's submit endpoint. "Mark as submitted" in PartManager sets `status='submitted'`; per-line arrival confirmation (checkbox per `mouser_order_item`) flips pending→arrived and posts the matching `stock_transaction`.
- **Price history**: every Search API quote appends a `price_history(source='mouser_api_quote')` row; the price you actually paid is a separate manual entry at restock time (`source='paid'`), since Mouser list price and your actual paid price can differ (promos, currency, etc.).

## 7. UI shell

Ribbon (`RibbonWidget`). **Corrected 2026-09-01 against the real library** — the string-keyed `addTab(icon, label)` / `addButton(tab, group, QToolButton*)` API this section originally described does not exist (those overloads are commented out in `Ribbon.h`), and there is no implicit group creation. The real API is object-tree based:

- `Ribbon(QToolBar* parent)` — a `QObject`, not a widget; it injects a `QTabWidget` into the toolbar you hand it, so the hosting `.ui` must contain a `QToolBar`.
- `RibbonTab(title, iconPath, Ribbon* parent)`, `RibbonButtonGroup(title, RibbonTab* parent)`, `RibbonButton(text, tooltip, iconPath, enabled, RibbonButtonGroup* parent)`.

Two traps: every constructor **already registers itself with its parent**, so calling `addTab`/`addGroup`/`addButton` afterwards adds the same object twice (the library's own `RibbonWidgetSandbox` example double-adds both its tabs), and `RibbonTab`'s constructor dereferences `parent` unconditionally despite defaulting it to `nullptr`. Construct with parents and never call the add methods. `examples/PartManagerApp/src/ui/PartManager_MainWindow.cpp` is the working reference.

Proposed tabs/groups:

- **Home** — groups: *New* (New Part, New Partlist), *Stock* (Restock, Take Out), *View* (List/Grid toggle, 3D Viewer).
- **Parts** — groups: *Manage* (Edit Type Templates, Import from Mouser), *Files* (Attach File, Open Datasheet).
- **Partlists** — groups: *Build* (New/Edit Partlist, Import CSV/BOM), *Checkout* (Check Stock, Take Out Parts).
- **Orders** — groups: *Mouser* (Build Order, Stage to Cart, Mark Submitted), *Tracking* (Confirm Arrival, Close Order).
- **KiCad** — groups: *Library* (Rebuild Libraries, Open Library Folder), *Plugin* (Install/Update PCB Plugin).

Static structure (main window, ribbon host, editor/dialog layouts) is built in **`.ui` files edited in Qt Designer**, not hand-written widget code — see `CODING_STYLE.md`. Code only fills in what Designer can't express: the category tree's live search annotation, per-category dynamic table columns, and forms generated at runtime from `part_type_attribute`.

One screen per tab, mocked up in `docs/design/mockups/*.svg`:

| Tab | Screen(s) | Mockup |
|---|---|---|
| — | Database selector (startup, Switch/Manage Databases) | `database-selector.svg` |
| Home | Stock overview (tree + table + preview) | `main-window.svg` |
| — | Part editor (opened from Home or any "part" link) | `part-editor.svg` |
| Parts | New Part from Mouser link/MPN | `new-part-mouser.svg` |
| Parts | Embedded Mouser search browser | `mouser-search.svg` |
| Home | New Part, manual/blank path (same generated form as above) | `create-part-manual.svg` |
| Parts | Type template editor (attributes + file slots, required/tooltip) | `type-template-editor.svg` |
| — | Settings (context menu → language/theme/storage) | `settings-dialog.svg` |
| Partlists | All partlists | `partlist-manager.svg` |
| Partlists | Partlist editor (rows, multiplier, check stock) | `partlist-editor.svg` |
| Orders | All orders | `orders-overview.svg` |
| Orders | Order detail (staging/arrival tracking) | `order-view.svg` |
| KiCad | Library status + PCB plugin status | `kicad-tab.svg` |

**Clickable prototype:** `docs/design/prototype/index.html` (open directly in a browser) stitches every screen above together with click-through hotspots — Figma-prototype-style, not a build step — plus three scripted use-case walkthroughs (new part from Mouser, build-partlist→order, quick take-out) and free navigation via the sidebar. Purely a design artifact; nothing here is app code or wired to the real DB/Mouser/KiCad.

### 7a. Category tree: search bar & live counts

Two independent filter boxes, both running the same §2a query engine, different scope:

- **Tree filter** (above the category tree, `main-window.svg`) — searches across *all* categories at once. Typing re-labels every tree leaf `Category (inStock : matches)` — e.g. `Resistors (412 : 1)` for `100` — without listing individual parts in the tree itself; it's a cross-category overview, not a row filter. Empty box: `Category (inStock)` as today.
- **Table filter** (above the part table, `main-window.svg`) — scoped to whichever category is currently selected; typing here actually filters the visible rows in that table, same query engine, no cross-category counting. Independent of the tree filter — you can have a tree-wide search narrowing which categories look promising, then a separate in-table filter to drill into the selected one.

Both are pure UI-state layered on existing widgets, not a schema change.

### 7b. Per-category table columns

Which columns the Home-tab table shows, their order and visibility, is configurable **per `part_type`** — Resistors and Screws can show completely different columns. Backed by one small config table:

```sql
CREATE TABLE part_type_list_column (
    id INTEGER PRIMARY KEY,
    part_type_id INTEGER NOT NULL REFERENCES part_type(id),
    column_key TEXT NOT NULL,        -- built-in: 'name'|'manufacturer'|'mpn'|'package'|'stock_qty'|'price'|'location'...
                                      -- or a part_type_attribute.key for that type
    label_override TEXT,             -- NULL = use the built-in/attribute label
    visible INTEGER NOT NULL DEFAULT 1,
    sort_order INTEGER NOT NULL DEFAULT 0,
    width_px INTEGER,
    UNIQUE(part_type_id, column_key)
);
```

Seeded automatically (visible, declaration order) whenever a type/attribute is created — except `location` (§2c), seeded with `visible = 0` since storage-location tracking isn't designed yet; a "Customize columns..." dialog just lets the user drag-reorder/toggle/resize, writing straight back to this table. This is genuinely dynamic per-user, per-category state — it lives in the DB and drives a `QTableView`'s header at runtime, it is *not* something a static `.ui` file can express, so it's the one part of the Home tab that's necessarily code-driven rather than Designer-driven.

## 8. Localization (English + German)

Qt's standard toolchain, not a custom string table: every user-facing string wrapped in `tr()`, `.ts` files per language (`PartManager_en.ts`, `PartManager_de.ts`) maintained with `lupdate`/`lrelease`, shipped as compiled `.qm` files loaded via `QTranslator` at startup based on the saved preference (§9). `.ui` files participate in the same `tr()` mechanism automatically (Designer marks widget text as translatable) — one more reason to keep layouts in `.ui` files rather than hand-built widgets. Switching language in Settings re-installs the translator and re-polishes open widgets (`QEvent::LanguageChange`) rather than requiring a restart, where practical; a "restart to apply" fallback is acceptable for the rare widget that doesn't retranslate cleanly. Data itself (part names, descriptions, attribute labels the user typed) is never translated — only the app's own chrome (menus, buttons, built-in field labels) is.

## 9. Settings & preferences

Reachable via a right-click context menu on the main window (and/or a Settings entry, whichever `RibbonWidget` supports more naturally) opening a **Settings dialog** — itself a `.ui` file, tabs or sections:

- **General** — Language (English / Deutsch), currency default (§0 assumption from earlier design, still just a display default).
- **Appearance** — Theme: Light / Dark / Follow system. Implemented as a Qt style sheet swap (or `QPalette` swap) applied at startup and live on change; RibbonWidget, being a normal Qt widget, follows the same stylesheet/palette.
- **Storage** — `PartManagerData/` folder location (§1), Mouser API key, KiCad library output path.
- **Backups** (confirmed, §9a below) — enable/disable, schedule, retention count, backup folder, "Backup Now" / "Restore from backup...".
- Persisted through the already-vendored `AppSettings` library — no new settings-storage mechanism needed.

Mocked up in `docs/design/mockups/settings-dialog.svg`.

### 9a. Automatic backups

No undo/edit-history log for v1 (confirmed) — instead, since autosave (§10) makes every edit instantly permanent, the app periodically snapshots the `partmanager.db` file itself into a rotating backup folder:

```
PartManagerData/backups/2026-08-31_0600.db
PartManagerData/backups/2026-08-31_1200.db
...
```

Defaults: every 6 hours while the app is running, plus always on clean shutdown; keep the last N (default 20, configurable). Only the DB file is snapshotted on this schedule — it's small and holds everything that autosave can silently overwrite (parts, attributes, stock transactions, partlists, orders). The `filestore/` and `kicad_libs/` trees are large, mostly additive, and content-hashed, so they're lower-risk and left to a manual "Backup Now" (full folder) in Settings rather than the automatic schedule. "Restore from backup..." picks a snapshot and swaps it in (with the current DB itself first moved aside, never deleted).

## 10. Autosave & close guard

Editors (Part editor, Type template editor, Partlist editor, Settings) autosave field-by-field as you type/change a value (debounced write to SQLite) — there is no explicit "Save" step for *already-created* records, and the app can be closed at any time without losing edits to something that already exists.

The one thing that blocks a close is an **in-progress multi-step action that hasn't reached a valid, committable state yet** — concretely:
- The New Part wizard (`new-part-mouser.svg` / `create-part-manual.svg`) before "Create Part" — nothing exists in the DB yet to autosave into.
- A partlist import mid-way through resolving unmatched rows.
- An order mid-stage that hasn't been written past `draft`.

Closing the app (or navigating away from) one of these shows a confirmation popup ("You have unsaved work in progress — discard and close?"); everywhere else, close is silent and instant. This means the "Save"/"Cancel" buttons drawn on some mockups (`type-template-editor.svg`, `partlist-editor.svg`) are really "confirm and leave this in-progress step" rather than a traditional save — worth relabeling to "Done"/"Discard" once implemented, to avoid implying unsaved changes exist elsewhere in the app.

## 11. Defining a new component type (attributes + files, required/optional)

`type-template-editor.svg` (updated) is where a component category itself gets defined: for each attribute, a **Required** checkbox (`part_type_attribute.required`) and a multiline **Tooltip** textbox (`part_type_attribute.tooltip`, shown as an ⓘ hint on the New Part screen) alongside the existing key/label/unit/datatype/searchable/list-visibility fields. A second list, same shape, defines the type's **expected file slots** (`part_type_file_slot`) — e.g. Resistor: Datasheet (optional), 3D Model (optional); Screw: CAD Model (required), Datasheet (optional).

The **New Part** screen generated from a type template (`create-part-manual.svg` for the manual/blank path, `new-part-mouser.svg` for the Mouser-prefilled path — both render the *same* generated form) marks required fields/files distinctly and blocks "Create Part" until every `required` attribute has a value and every `required` file slot has an attachment; optional ones are just as visible but never block creation.

## 12. Code structure — `core/` (backend) vs `examples/PartManagerApp/` (desktop app)

`core/`'s `GLOB_FILES` is recursive (see `core/CMakeLists.txt` comment on `.ui` handling) — subfolders need zero CMake changes, they're picked up automatically. The project's current convention (`CODING_STYLE.md`) is a flat `inc/`/`src/` with `PartManager_<Topic>.h/.cpp` file names; that fits the tiny template scaffold that exists today, but this app's domain (§1–§11) is large enough that flat-with-prefix alone would turn into a 60+-file pile in one directory. **Extending** the convention: subfolders by module, file names keep the `PartManager_` prefix and lose the now-redundant module word (the folder already says it), the namespace stays flat `PartManager` (not nested) as `CODING_STYLE.md` already mandates.

### 12a. `core/` — one module per concern, strict one-way dependency

```
core/inc/  core/src/
  domain/       PartManager_Part.h, PartManager_PartType.h, PartManager_PartTypeAttribute.h,
                PartManager_PartTypeFileSlot.h, PartManager_PartFile.h, PartManager_Seller.h,
                PartManager_PriceHistory.h, PartManager_StockTransaction.h, PartManager_Partlist.h,
                PartManager_PartlistItem.h, PartManager_MouserOrder.h, PartManager_MouserOrderItem.h
                — plain data classes only (§1–§4), zero Qt Widgets, zero SQL.
  database/     PartManager_DatabaseHandle.h (open/connect/WAL a single database folder, wraps
                SQLiteWrapper), PartManager_DatabaseMetadata.h (reads/writes the `.pmdb` entry file +
                `db_meta` table, §1a), PartManager_SchemaMigrator.h (forward-only migrations, §1c),
                PartManager_DatabaseRegistry.h (the known-databases list, backed by core/settings, §1b)
                — the only module that knows there can be more than one database.
  persistence/  one repository per aggregate, all operating on an already-open PartManager_DatabaseHandle:
                PartManager_PartRepository.h, PartManager_PartTypeRepository.h (incl. §2b inheritance
                resolution), PartManager_StockRepository.h, PartManager_PartlistRepository.h,
                PartManager_OrderRepository.h, PartManager_SellerRepository.h
                — the only place domain SQL/SQLiteWrapper is touched.
  units/        PartManager_UnitTable.h (dropdown list + SI-prefix table, §2a), PartManager_ValueParser.h
                (entry/search parsing incl. decimal-separator + "4k7" shorthand) — pure logic, no Qt at all,
                the easiest module to unit-test.
  search/       PartManager_SearchQuery.h (parses free text via units/), PartManager_SearchEngine.h
                (runs the two-pass match against persistence/ repositories, §2a).
  filestore/    PartManager_FileStore.h (content-addressed storage, hashing, dedup, §1).
  mouser/       PartManager_MouserClient.h (HTTP via Qt Network against MouserAPI_V1.json endpoints),
                PartManager_MouserSearchService.h (DTO → domain prefill mapping, §6),
                PartManager_MouserOrderService.h (cart staging, CartKey handling).
  kicad/        PartManager_KicadLibraryGenerator.h (per-category .kicad_sym/.pretty writer, §5a),
                PartManager_KicadLibTableWriter.h, PartManager_KicadEditTracker.h
                (kicad_generated_item hash tracking / tolerant regeneration).
  backup/       PartManager_BackupManager.h (§9a — scheduled snapshot, rotation, restore).
  settings/     PartManager_Settings.h (typed facade over the vendored AppSettings lib — language,
                theme, storage paths, API keys, backup schedule).
```

Dependency direction is one-way and enforced by review, not by CMake: `domain/` depends on nothing else in `core/`; `database/` depends only on `settings/`; `persistence/` depends on `domain/` and an already-open `database/` handle; `units/`/`filestore/`/`backup/`/`settings/` depend on nothing or only `domain/`; `search/`, `mouser/`, `kicad/` depend on `persistence/`, `domain/`, `units/` as needed. Nothing in `core/` includes anything from `examples/PartManagerApp/`, and nothing in `core/` instantiates a `QWidget` — nothing there truly *needs* it, and keeping it out is what lets `persistence/`, `units/`, `search/`, `mouser/`, `kicad/` be exercised by `unittests/` without spinning up a GUI.

### 12b. `examples/PartManagerApp/` — views stay dumb, `.ui`-driven

```
examples/PartManagerApp/src/
  main.cpp              — QApplication setup, QTranslator install (§8), theme/palette apply (§9),
                           opens MainWindow.
  ui/                    — one widget + matching .ui per screen from the tab/screen table (§7):
                           MainWindow(.ui), DatabaseSelectorDialog(.ui) (§1b — shown at startup and from
                           "Switch/Manage Databases"), PartEditorWidget(.ui), TypeTemplateEditorWidget(.ui),
                           NewPartWizard(.ui), MouserSearchWidget(.ui), PartlistManagerWidget(.ui),
                           PartlistEditorWidget(.ui), OrdersOverviewWidget(.ui), OrderDetailWidget(.ui),
                           KicadTabWidget(.ui), SettingsDialog(.ui).
                           Each widget only lays out controls and forwards user actions — no business
                           logic, no direct SQL, no direct Mouser/KiCad calls.
  widgets/               — small reusable custom controls that don't fit a single screen: a
                           DimensionLineEdit (the §2a SI-prefix-aware numeric field, used on New Part,
                           Type Template Editor, and the search boxes alike), the category tree with
                           live counts (§7a), the column-customize dialog (§7b).
  controllers/           — one per screen, the only thing a `ui/` widget talks to: translates button
                           clicks/field edits into calls on `core/`'s repositories/services and pushes
                           results back into the view. This is what keeps `ui/` dumb and swappable —
                           a controller has no Qt Widgets dependency itself beyond signals/slots, so
                           the app's actual behavior is testable without instantiating real widgets.
  resources/             — icons, .qrc, ribbon button assets.
```

**Why the controller layer**: without it, "modular and easy to expand" breaks down fast — a screen's `.ui`-generated widget class would otherwise reach directly into `PartRepository`/`MouserClient`/etc., and every UI tweak risks touching business logic. With it: `ui/` can be redesigned in Qt Designer without touching a single line of persistence/Mouser/KiCad code, and `core/` can grow a whole new screen's worth of backend logic and be unit-tested before any widget exists for it.

## 13. Open / deferred items

- 3D viewer for `.stp`/step files (mechanical parts, future) — needs a 3D lib (e.g. Qt3D or a STEP-capable viewer component); deferred until mechanical part types are actually added, so it doesn't block the electronics MVP.
- Remote/multi-device access — current design is single-machine embedded SQLite; if that's ever needed, add a thin sync/server layer later rather than building it speculatively now.
- SnapEDA/Ultra Librarian/manufacturer auto-fetch for symbols/footprints/3D models — candidate fast-follow, not MVP.
