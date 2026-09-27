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

**A footprint file is named after the part's package, not the part.** A footprint is a pad layout, and every 0402 capacitor has the same one — naming the file after the part's *value* gave each of them a private copy called `1 µF 10 V X7S 0402`, which is neither findable in KiCad nor anything a second part could reuse. The name is `<Reference>_<Package>` in KiCad's own style (`C_0402`), the reference letter coming from the type's base symbol so the file says what it is for; KiCad's `_1005Metric` suffix is a metric size this project has no table for and is deliberately absent rather than guessed. A part with no package falls back to its own name. Footprint names are ASCII and space-free — stricter than `sanitizeSymbolName`, because a symbol name lives inside a quoted s-expression while this **is a file name**, travelling through an fp-lib-table, a `.pretty` folder and whatever filesystem the library is copied onto.

**Where one package name is claimed by different pad layouts, the later one is suffixed with a hash of its content** (`C_0402_3f2a19b8`) and the clash is recorded in the generation result. A content hash rather than a counter, so the same layout always lands on the same name and adding a part never renames a file KiCad already refers to. Measured against the developer's own database on 2026-09-27: 30 distinct footprints across 5 packages, **13 names claimed by different layouts and nothing shared at all**, because every part carried its own vendor file and no two were byte-identical. Naming alone therefore consolidates nothing — it only makes the duplicates visible.

**So parts can share one stored footprint file, and the decision is offered at the moment a part receives a footprint** — after an EasyEDA fetch, after New Part, and after a vendor-ZIP import, which is the case this was asked for. The footprints already in the database are ranked against the arriving one and the best is offered first, with an overlay as the evidence: every candidate drawn through **one shared transform** so a 0.1 mm pad difference is 0.1 mm on screen, the selected one opaque on top and the rest behind it. The overlay draws **pads only** — physical interchangeability is decided by the copper, and silkscreen is the busiest layer in most vendor files, so drawing it makes two compatible footprints look unalike.

The metric (`core/kicad/PartManager_FootprintCompatibility.h`) compares pads and nothing else:

- **Tolerances: 0.15 mm position, 0.25 mm size, 0.10 mm drill, 1° rotation.** Derived from package geometry rather than chosen: 0402 and 0603 nominal lands put their pad centres 0.36 mm apart and their pad sizes 0.3 mm apart, so both thresholds sit at under half the distance to the neighbouring package while still absorbing the spread between two vendors' versions of one package. Calibrated on the developer's database, where a genuine accept measured 0.14 mm and a genuine reject 0.27 mm — a 1206 LED land pattern against a 1206 capacitor one.
- **A whole-footprint translation is free**; the comparison aligns on the centroid of the pad centres first, because two vendors drawing the same pads around a different origin is the ordinary case.
- **Rotation is deliberately not normalised.** Pads running north-south are not interchangeable with east-west: KiCad would place the part turned, and it would not fit the board it was designed for.
- **Pads pair by number** when both files number them, so pad 1 meets pad 1 and a polarised part cannot be judged compatible with its own mirror; position pairing is the fallback for unnumbered pads.
- **Through-hole against surface-mount scores zero** and is never offered as a best fit. One needs a hole in the board and the other must not have one.

**Ranked by score alone — `compatible` is a badge, not a filter.** On real data the margin between the two verdicts is hundredths of a millimetre, so sorting on the boolean would claim a discontinuity the copper does not have, and would bury the case that matters most: IPC publishes three density levels for one package, and a user deliberately after the denser land pattern has to find it in order rather than underneath everything that scraped past a threshold. Every row carries its measurements for the same reason — a number the user can check beats a verdict they have to trust.

**Accepting re-points the part's `part_file` row in place** (`FileStore::useStoredFile`), it does not re-import the bytes. The ordinary attach route runs through `detachFile()`, which deletes a stored file the moment its last row goes: measured taking the filestore from 139 files to 138 by destroying the vendor original of a part that already had a footprint. Nothing here deletes a file — an unreferenced footprint stays on disk, becomes an ordinary orphan, and only goes when the user runs the storage sweep in Settings. Regeneration then drops it from `kicad_libs/` on its own, because the generator only ever writes footprints that a part actually references.

**The variant browser** (KiCad tab → Footprint Variants) is the read-only other half: every package, the distinct footprints under it grouped by content hash, which parts use each, and the same overlay. A package with one variant is finished work; a package with several is the work list. Parts with no package are never in conflict — they each write a file named after themselves — and are marked as such rather than counted as clashes.

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

## 14. LLM assistant (QtLLM integration)

The app embeds an LLM the user can talk to, and which can act on the database
through a fixed set of tools. The library is KROIA's `QtLLM` (`dependencies/QtLLM.cmake`,
fetched like every other dependency), which supplies the client, the tool-calling
loop, a ready-made chat dock, a settings dialog and headless background agents.

Two things shape everything below. First, **the default provider is a local
Ollama model**, because a parts database is a place where a feature that costs
money per click does not get used. Second, **a tool result is the only thing the
model learns from**, so every handler is written to be corrected by: it validates,
and when it refuses it says what it expected.

### 14a. Where it lives

```
core/inc/llm/  core/src/llm/
  PartManager_LlmTool.h        LlmToolContext (an open DatabaseHandle + an allowWrites flag),
                               LlmTool (schema + handler), llmOk()/llmError(),
                               registerLlmTools().
  PartManager_PartToolset.h    the database tools: categories, parts, attributes, tags.
  PartManager_MouserToolset.h  mouser_search / mouser_suggest_category / mouser_import_part (§6).
  PartManager_MigrationAgent.h the headless agent that turns a part number into a part row.

examples/PartManagerApp/src/controllers/
  PartManager_LlmController.h  owns the one Client and the chat dock; §14d's prompt injection.
```

`core/llm/` is Qt but widget-free, like every other `core/` module (§12a) — the
chat panel and the settings dialog are the app's, the tools and the agents are
not. That split is what lets a tool be unit-tested without a model.

**A toolset returns `std::vector<LlmTool>`; it does not register itself on a
client.** The handler is then a plain function of a `QJsonObject` and an open
database, so a test calls it directly with no model, no network and no event
loop — the same split that made `StepConverter` testable by having it name a
path instead of running the subprocess (§13).

### 14b. Provider and model, and what was measured

Measured against the local Ollama server on 2026-09-26, running the real
migration tool loop (search → list categories → create category → create part):

| Model | Result |
|---|---|
| `gpt-oss:20b` | Correct. 6 calls, ~103 s. Recovered from a rejected enum value on its own. **Default.** |
| `qwen3:8b` | Correct. 5 calls, ~690 s — it reasons at length before each call. Fallback only. |
| `qwen2.5-coder:14b` | **Emits no tool calls at all** through `/api/chat`; it prints the call as JSON in the message body, which arrives as ordinary assistant text. |
| `llama3.2:3b` | **Unusable.** Called `create_part` first with empty strings for every field, then printed an invented result. It answers a single trivial tool call correctly, which is what makes this failure easy to miss. |

So a model is a preference, never a promise: `AgentConfig::fallbackModels` is
walked when the server does not offer the configured one, and the effective
model is recorded in the result. `llama3.2` is deliberately *not* a fallback —
it ships with almost every Ollama install and would be picked silently.

Claude is supported by the same code (`QtLLM::Provider::Claude`, default model
`claude-sonnet-5`, key from `ANTHROPIC_FOUNDRY_API_KEY` or `ANTHROPIC_API_KEY`
in the environment). It is not the default provider. The assistant's settings
dialog does have an API-key field — pre-filled from the environment, overriding
it for the session, and never persisted; §14f has the rule and why it is not the
same thing as §9's Mouser field.

**How the provider is chosen and remembered.** The settings dialog's provider,
endpoint, model, system prompt, font size and tool-call tick are persisted in
`AppPreferences` (§9), so a user who switches to Claude once does not re-take
that decision — or re-type that endpoint — on every launch. Three rules make
that work:

- **Per provider, not per client.** `QtLLM::Client` holds one endpoint and one
  model at a time, and the dialog has one field and one combo between two
  providers. The controller therefore keeps the Ollama url/model and the Claude
  endpoint/model separately and fills the dialog from whichever provider is
  selected. Filled from `client->model()` instead, switching to Claude sent
  `gpt-oss:20b` to Anthropic.
- **An endpoint is normalized wherever it is resolved** — from the environment,
  from `AppPreferences`, or out of the dialog. Trailing slashes come off and
  `/v1/messages` goes on unless the URL already ends in `/messages`. The field
  opens showing a URL *with* the path, so a gateway base pasted over it looks
  like the same kind of thing and is not, and nothing says otherwise until the
  first prompt fails; "typed wins" must not mean "typed wrong wins silently".
  A genuinely non-standard path is still honoured — it just has to name it.
- **An empty setting means "whatever the default is now".** The Claude endpoint
  defaults to `ANTHROPIC_FOUNDRY_BASE_URL` + `/v1/messages` (that variable
  carries no path), or `https://api.anthropic.com/v1/messages` when it is unset.
  A value the user types wins and is stored; a value equal to the current
  default is stored as *empty*, so a later change to the environment is followed
  rather than frozen. The Ollama url and both model ids follow the same rule.
- **A fallback is not a preference.** The Ollama model id is only written when it
  came out of the dialog's combo. The one the §14b walk settles on by itself is
  left empty, and does not take the first-candidate slot on the next launch
  either — otherwise a machine without `gpt-oss:20b` would resolve to `qwen3:8b`,
  have it persisted by the next unrelated Apply, and go on preferring it after
  `gpt-oss:20b` was finally pulled, with nothing in the UI to say why or how to
  undo it.
- **Nor is the combo a preference across a provider switch.**
  `SettingsDialog::onProviderChanged()` clears the model combo and writes its own
  text — `claude-haiku-4-5` for Claude, `llama3.2:latest` for Ollama. Read back
  as a choice, one switch to Ollama and back would pin `llama3.2`, which is the
  model §14b measured as unusable and left out of the fallback list for exactly
  this reason: it gets picked silently. An Apply that changes the provider
  therefore keeps the remembered model for the provider being switched to, and
  changing the model is a second Apply.
- **Claude without a key does not start.** If the remembered provider is Claude
  and neither variable is set, the app starts on Ollama and the status line says
  which two variables to set — a client built with an empty key fails on the
  first prompt with an authentication error that names nothing actionable. On
  Claude the `OllamaManager` probe is skipped entirely: it starts `ollama serve`
  when it finds nothing, and that is a server the user did not ask for.

An endpoint or model edit applies whether or not the provider changed.
`Client::setProvider()` clears the conversation by design (two providers'
message formats do not interleave), so an edit made while staying on the same
provider goes in through `setEndpointUrl()`/`setApiKey()` instead — the key is
re-read from the environment on every Apply, because it may have been set since
launch.

### 14c. The toolsets

`PartToolset` is a thin *validating* wrapper over the repositories — it adds no
persistence and issues no SQL (§12a still holds). What it adds is what a model
needs and a C++ caller does not: an answer that says why.

Three rules here are load-bearing, each because the opposite was observed:

1. **`create_category` is idempotent on `(name, parentId)`.** Handed a
   non-idempotent create, a local model that had just created "Varistors"
   created it again, and again — eight calls before the cap stopped it.
2. **Ids are never invented.** `create_part` takes a `categoryId` that came from
   `list_categories`/`create_category` and refuses an unknown one *while naming
   the ones that exist*. A category name is not accepted in its place: two
   branches may legitimately carry the same leaf name (§2b).
3. **Every column with a fixed vocabulary is an enum parameter, and is
   re-validated in the handler.** Two separate observations: the model answered
   a `glyph` enum with an emoji, and — given a free-text `domain` — wrote
   `"Circuit Protection"` into it, having read the name as "which product domain
   this part comes from" rather than as `part_type.domain`'s
   `electronic`/`mechanical`/`generic`. The first is caught by validation; the
   second is only caught by *declaring* the enum, because a free-text parameter
   has nothing to validate against and `effectiveDomain()` would inherit the
   junk down the whole §2b subtree. A parameter description therefore says what
   the value means in PartManager's terms, not just what it is called.

A fourth rule joined them from the same kind of observation, one level down —
about argument *shapes* rather than about vocabulary. **An optional parameter is
absent when it is `null` and when it is `""`.** Measured on `gpt-oss:20b`
(2026-09-27): asked for a filtered listing it sent `"modifiedAfter": null` on
one call and `"modifiedAfter": ""` on the next, and a handler that accepted the
first while refusing the second as an unparseable date cost a whole turn to
teach the model nothing — the two mean the same thing and only one of them was
understood. So every handler is forgiving about *shape* and strict about
*meaning*: integers are read out of `"5"`, booleans out of `"true"` and `1`,
and an empty optional is an omission rather than a value to validate. What is
still refused, loudly, is a value that means something wrong.

A fifth thing was expected and turned out false: **a longer tool list did not
make the model worse.** The same migration ran in 6 calls / 103 s with four
tools advertised and 5 calls / 71 s with fourteen — the extra tools
(`mouser_suggest_category`, `mouser_import_part`) *shortened* the loop by making
whole steps unnecessary. Trim a tool list for correctness, not for length.

**Category pictograms are chosen out of two closed lists (v13).**
`create_category` takes an optional `glyph`, and `set_category_icon` takes
`{categoryId, glyph, colour?}`. `glyph` is a `TypeGlyph` name; `colour` is one
of the twelve palette slots **by name**, and there is deliberately no RGB
parameter — the palette exists because an arbitrary hash-to-RGB produced
unreadable mud about a third of the time (§7c), and a model picking hex would
do no better. Both are stored on `part_type` (`icon_glyph`, `icon_colour`),
both are re-validated in the handler per rule 3, and an omitted `colour` leaves
the category's current one alone the way `update_part` does.

Unset is the normal state and means *derive it*: `TypeIconStyle::resolve()`
prefers the stored values and falls back to `forType()`, which is what every
pre-v13 category still draws. The v13 migration therefore writes nothing into
the new columns — seeding the derived value would freeze today's name matching
into the rows, so renaming a category would stop changing its icon and a later
improvement to `forType()` would reach none of them. The picture is **not**
inherited down the §2b chain: a subtype is a different picture, not the same
one again. The same two lists drive the Type Template editor's glyph and
colour pickers, so the assistant and the user choose from one vocabulary.

`MouserToolset` keeps `mouser_import_part` as **one** tool rather than
primitives the model assembles, because that path already knows what a model
does not: that `Price` carries its own currency, that the image URL lies about
its extension, that an HTTP 200 can still be a block page, and that a Mouser
article number belongs in `part_seller_link` and never in `part.mpn` (§6).

### 14d. Prompt injection — the buttons in the app

A button like "Generate description" next to the part editor's description field
does **not** call the model behind the user's back. It injects a prepared prompt
into the chat through `ChatDockWidget::submitPrompt()`, which renders it as a
user bubble and sends it — so it costs a visible turn and leaves the answer, the
tool calls and the cost where every other answer lives. A feature that quietly
spends tokens is a feature nobody can audit.

`submitPrompt()` refuses while a turn is in flight, which is why
`LlmController::injectPrompt()` returns a bool and the buttons report it rather
than queueing: two impatient clicks must not stack two paid turns.

### 14e. Background agents

`QtLLM::Agent` is a conversation the user never sees, with its own provider,
model, tool list and spend cap, listed live in the settings dialog's Agents tab.
`MigrationAgent` is the first one: given a Mouser article number, a manufacturer
part number or a pasted product-page URL, it looks the part up, decides which
category it belongs in — creating one when nothing fits — and creates the part.

It is capped three ways, because the failure mode of an unattended agent is a
loop and not a wrong answer: a per-turn tool-call cap, a wall-clock timeout, and
a tool list trimmed to what the job needs (a small model does measurably worse
the longer the tool list gets).

### 14f. Safety

- **The database is the blast radius.** `LlmToolContext::allowWrites = false`
  turns the whole toolset read-only, enforced *in the handlers* rather than by
  leaving tools unregistered — so the model is told why instead of guessing at a
  missing capability.
- **No tool takes an API key as a parameter**, so a model can neither read one
  nor be talked into echoing one into the chat. Keys stay in the environment
  (§6, §9).
- **The Claude key is never persisted — that is the whole of the rule.** The
  environment supplies it (`ANTHROPIC_FOUNDRY_API_KEY`, falling back to
  `ANTHROPIC_API_KEY`), the settings dialog's field opens pre-filled with
  whatever is in force, and a key typed there overrides the environment **for
  that session**, held in the controller and nowhere else. `AppPreferences` has
  no field it could be written to and is not getting one: a stored key lands in
  a plain-text file in the user's data folder, which is exactly what the
  environment avoids. The next launch therefore starts from the environment
  again. The field is `QLineEdit::Password` echo, and the key is never logged —
  not even its length. The *endpoint* beside it is not a secret: it is editable,
  persisted and defaulted from the environment like any other preference (§14b).
- `Client::setValidateToolInput(true)` is on, and the per-turn tool-call cap is
  set. Both are off by default in the library.
- The filesystem built-in tools (`read_text_file`, `write_text_file`,
  `list_directory`) are **not** registered. The parts database is reachable
  through typed tools; a general file-write tool adds nothing to that and removes
  the guarantee that the assistant can only touch parts.

**The one exception, and the boundary that replaces the old rule (2026-09-27).**
This section used to say that *no* tool touches the filesystem and that no tool
takes a path. The first half is no longer true and the second half now is, more
strictly than before.

What changed it is the user's own sentence: *"migrate this part — I already
downloaded the ECAD zip into my downloads folder"*. Every step of that was
already built except the one the user had done themselves, and the alternatives
were worse than a narrow opening: re-downloading the archive the assistant
cannot see, or having the user paste a path into a chat that must not accept one.
`EcadDownloadToolset` (`examples/PartManagerApp/src/llm/`, §5c) is the answer,
and the shape of it *is* the safety argument:

- **The model never supplies a path — it supplies a name it was given.**
  `list_downloaded_libraries` hands back bare file names; `inspect_downloaded_library`
  and `attach_downloaded_library` take one back. Everything that becomes a path
  is built by `resolveArchive()` out of a name plus an allowed root.
- **A name that is not a name is an error that says so.** A path separator, a
  `:`, a `..`, or anything not ending in `.zip` is refused by a message naming
  the rule — not by an empty result, which cannot tell "I may not ask for that"
  from "there is no such file".
- **The resolved path is checked after canonicalisation**, and its parent folder
  must *be* an allowed root rather than start with one: that defeats both a
  symlink pointing out of the folder and the `Downloads2\` prefix trick.
- **The allow-list is two folders at most.** The system Downloads folder
  (`QStandardPaths::DownloadLocation`, falling back to `HomeLocation` the way
  `EcadFetchDialog` already does), plus `AppPreferences::llmDownloadFolder`,
  which defaults empty and has deliberately no Settings-dialog field yet.
- **One extension per tool, and only four operations**: list the folder's
  archives, classify one, attach one to a part the model named, or attach a
  datasheet. No tool reads arbitrary file *content* back to the model, lists an
  arbitrary directory, or writes anything outside the filestore — both writes go
  through `PartEditorController` (`importEcadArchive()`,
  `downloadDatasheet()`/`attachDatasheet()`), the same calls the dialogs make, so
  each write path is one path and not two.

**The second relaxation: `set_part_datasheet` also takes an absolute path.**
A bare name still resolves inside the allowed roots like everything else, and a
path is accepted *in addition* — because the user asked for it in those words,
and a datasheet is saved wherever they happened to be looking rather than in
Downloads. The narrowing that makes it acceptable is the extension and the
verb: `.pdf` only, the file must exist, the refusal names which of those failed,
and the only thing done with the file is "copy it into the filestore as this
part's datasheet". The model never gets the bytes back — it gets
`readable`/`looksScanned`/`pageCount` from `PdfText` (§14g), which is what tells
it whether `search_datasheet` can answer anything from this file or whether it
must say it cannot read it. This is the one tool in the app that takes a path,
and only one the user typed.

**A modeless editor makes any part write a two-writer problem.** Part editors
outlive the call that opens them (§10) and hold their own copy of the row, so a
tool that writes a part behind an open editor has its change undone by that
editor's next autosave. `set_part_datasheet` therefore calls
`LlmUiBridge::reloadOpenPartEditor()` after writing, which reaches
`PartEditorDialog::reloadIfOpen()` — the editor re-reads and stops being stale.
A pending debounced write is dropped rather than flushed there: flushing would
write the stale copy back, which is the bug being prevented, and losing a
keystroke typed inside the 400 ms window is both smaller and visible. Note that
this is a *general* exposure and only the one tool is covered: `update_part` and
`set_part_attribute` live in `core/llm/`, which is widget-free by §12a and
cannot call the bridge at all, so the general fix is not this one (§14h).

So the rule is now: **the assistant reaches parts through typed tools, and
reaches the filesystem only as "a ZIP the user downloaded", by a name that came
out of a listing.** The KiCad and datasheet toolsets still take no path at all
(§14c, §14g) — a file there only ever arrives from another part in the same
database, and that has not been relaxed.

The filename-matching rule the listing filters by is
`EcadArchive::matchesPartNumber()` in `core/import/`, shared with the download
dialog's folder watch rather than copied into the toolset: an assistant offering
an archive the dialog would not have taken is a disagreement neither of them can
explain. It is pure, so it is unit-tested (`TST_EcadArchive`).

### 14g. Reading datasheets

`core/pdf/PdfText` pulls text out of a PDF, and `DatasheetToolset` is the three
tools over it. **No new dependency**: Qt already carries zlib, and `FlateDecode`
is the only filter the *text* streams in real datasheets use.

`search_datasheet` is the primary tool, not `read_datasheet`. A datasheet runs to
tens of pages and a local model's context does not, so the useful primitive is
"find the passage and say which page", not "here is the document".

**Measured over the 27 datasheets in the user's own `KicadFresh` filestore: 21
extracted to text, 6 correctly identified as scans, 0 failures.** Filters across
that corpus are `FlateDecode` (4333 streams), `DCTDecode` (26) and
`CCITTFaxDecode` (6) — the last two are images, which is what a scanned page is.

Two honest limits, both load-bearing:

- **It extracts text, it does not reconstruct layout.** A two-column page
  interleaves, and a pinout printed as a grid comes back as loose words. That is
  why every answer carries its page number: the contract is "here is where it
  says that", not "here is the table".
- **A datasheet it cannot read says so.** A scan comes back `scanned: true` with
  no text; an encrypted file is named as encrypted. This matters more than the
  hit rate — a model that is handed an empty string answers from its own memory
  of what an LM358 does, and inventing a specification is the worst thing this
  feature could do.

### 14h. Still not built

- **Editing KiCad symbol and footprint *geometry* through tools.** The geometry
  editors do not exist in the app either (`TASKS.md`, Feature wishes), so there
  is nothing for a tool to drive. Note that the *other* reading of the request —
  "change this part's footprint" — is built: `set_part_kicad_file` gives a part
  the symbol, footprint or 3D model another part already carries.
- **Deciding that two footprints may be shared** (`FeatureRequests.md` request 2).
  `find_parts_sharing_a_footprint` answers which parts have the same pad layout,
  and reports "identical" separately from merely compatible — but merging them
  into one shared library artifact is an open design question, not something a
  tool should settle on its own.
- **The general "a tool wrote a part an open editor is holding" problem.**
  `set_part_datasheet` handles its own case (§14f), but `update_part` and
  `set_part_attribute` have the same exposure now that editors are modeless, and
  they cannot use the same fix: they live in `core/llm/`, which is widget-free by
  §12a and cannot see `LlmUiBridge`. The shape of a real answer is therefore a
  notification rather than a call — something the write side raises and the app
  subscribes to (a "part N changed" signal on `DatabaseHandle` is the obvious
  candidate, and would also cover the part table and the tree) — or an editor
  that re-reads on focus, which is cheaper and covers less. Deliberately not
  decided here.
