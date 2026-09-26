// @file PartManager_PartTypeTransfer.h
// @brief Export/import of component categories (§2, §2b) between two databases.
//
// The problem: a user builds up their category tree in one database — attributes,
// file slots, table layouts, default tags, the lot — and then wants that work in a
// second database without redoing it by hand. §1b already lets several databases
// exist side by side and deliberately keeps them fully independent, so there is no
// shared table to point at: the categories have to travel as data.
//
// Three stages, deliberately separate, because they have different risks:
//   1. **Bundle** — read a (sub)tree of `part_type` out of a database into plain
//      structs, and turn that into JSON. Reads only.
//   2. **Plan** (`planMerge`) — decide, for every incoming category, whether it is
//      new here, is the *same* category as a local one, or should be left alone,
//      and list every field the two disagree on. **Writes nothing at all.** That is
//      the whole point: a merge that touches a user's category tree has to be
//      reviewable before it happens, and the caller (a dialog, later) shows this
//      plan and lets the user flip individual decisions.
//   3. **Apply** (`applyMerge`) — execute a plan inside one Transaction. All of it
//      or none of it (see PartManager_Transaction.h for why).
//
// **Identity across databases is the path, never an id.** `part_type.id`,
// `parent_type_id`, `tag.id` and every attribute/slot/column row id are local
// primary keys and mean nothing in another file; carrying them across would file
// categories under whatever category happened to hold that number here. The stable
// identity is the full category path — the ordered list of names from the root down,
// {"Capacitor", "Ceramic Capacitor"} — and tags travel by name.
//
// **The path is a list of segments, never one joined string.** It used to be
// "Capacitor/Ceramic Capacitor", which was convenient right up until someone named a
// category `Crystal / Oscilator`: the separator is an ordinary character in a name the
// user typed, so a joined path cannot be split back without inventing a parent that
// never existed. A list of segments has no separator to trip over and lets a category
// be called anything at all. Joining is for *showing* a path to a person
// (displayPartTypePath()); it is never how one is compared.
//
// **Only OWN rows are exported, never effective/inherited ones.** A "Ceramic
// Capacitor" bundled with its inherited `capacitance` flattened into it would
// arrive as a type that redeclares everything its parent already says, and §2b's
// resolution — the reason the tree exists — would be permanently undone by one
// round trip. Inheritance is re-derived on the other side from the parent chain
// the bundle also carries, so a subtype that owns nothing arrives owning nothing.
//
// Widget-free and network-free, like the rest of core (§12a). QJsonDocument does
// the JSON, as everywhere else in this project.
// @see docs/design/ARCHITECTURE.md §1a, §1c, §2, §2b, §2d, §7b, §11
// @see PartManager_Transaction.h, PartManager_PartTypeRepository.h, PartManager_PartListMigration.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_PartType.h"
#include "domain/PartManager_PartTypeAttribute.h"
#include "domain/PartManager_PartTypeFileSlot.h"
#include "domain/PartManager_PartTypeListColumn.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// The bundle format this build writes and is the newest it can read (§1c's rule applied to a
	// file rather than to a schema: a bundle from a newer PartManager is refused, never guessed at).
	// v2 writes `"path"` as an array of name segments; v1 wrote it as one '/'-joined string and is
	// still read (see fromJson()).
	constexpr int CurrentBundleFormatVersion = 2;

	// The `"format"` marker every bundle carries. A JSON file without it is not ours, whatever
	// else it happens to contain, and is rejected before a single field is read.
	PART_MANAGER_API const char* bundleFormatMarker();

	// The separator a **format v1** bundle joined its path with. Kept for reading those files and
	// for nothing else — no path this build writes or compares goes through it (see file header).
	constexpr char PartTypePathSeparator = '/';

	// One category and everything it owns, ready to cross into another database.
	struct PART_MANAGER_API PartTypeNodeBundle
	{
		// The `part_type` row's data. `id` and `parentTypeId` are cleared when bundling and are
		// meaningless here — `path` is what says where this category sits (see file header).
		PartType type;
		// {"Capacitor", "Ceramic Capacitor"} — the cross-database identity, root first, one entry
		// per category, each the real (untrimmed, uncased) name. Never joined for comparison.
		std::vector<std::string> path;
		// OWN rows only, never effective/inherited ones (see file header). Their `id`/`partTypeId`
		// are cleared for the same reason the type's are.
		std::vector<PartTypeAttribute> ownAttributes;
		std::vector<PartTypeFileSlot> ownFileSlots;
		std::vector<PartTypeListColumn> ownListColumns;   // §7b, the saved Home-table layout
		// §2d default tags by NAME. A tag id is local to one database; a name is the vocabulary
		// the user actually manages, so import looks the name up and creates it if it is missing.
		std::vector<std::string> defaultTagNames;
	};

	// A whole export: a forest of categories plus who wrote it and when.
	struct PART_MANAGER_API PartTypeBundle
	{
		int formatVersion = CurrentBundleFormatVersion;
		std::string sourceToolVersion;    // LibraryInfo::version of the writing build
		std::string sourceDatabaseName;   // the source database folder's name (§1a), for the UI to name
		std::string exportedAt;           // ISO-8601 UTC, same shape as db_meta's timestamps (§1a)
		// Parents always precede their children, so an importer can resolve a parent path by
		// walking the list once. bundleFromDatabase() guarantees it.
		std::vector<PartTypeNodeBundle> nodes;
	};

	// What a merge intends to do with one incoming category.
	enum class TypeMergeAction
	{
		AddNew,     // nothing local is this category; insert it
		MergeInto,  // this *is* a local category (`localTypeId`); reconcile the two
		Skip        // leave both sides alone — the caller's choice, never planMerge()'s
	};

	// Who wins one disagreement. The default leans TakeIncoming: someone importing a category
	// is asking for the other database's version of it, and a merge that defaulted to KeepLocal
	// would import nothing at all on every field that already had a value.
	enum class FieldResolution
	{
		KeepLocal,
		TakeIncoming
	};

	// One field the two databases disagree on, and what to do about it.
	struct PART_MANAGER_API FieldConflict
	{
		// Stable ascii key, never a display string: "description", "kicad_category",
		// "attribute:resistance:label", "fileslot:datasheet:required",
		// "listcolumn:resistance:visible". The app maps these to tr()'d text; core does not
		// translate (§8).
		std::string fieldKey;
		// Untranslated English label for the same field — "Description", "KiCad category". A
		// hint for the UI to translate, not something to show raw in a German build.
		std::string displayLabel;
		std::string localValue;
		std::string incomingValue;
		FieldResolution resolution = FieldResolution::TakeIncoming;
	};

	// The plan for one incoming category.
	struct PART_MANAGER_API TypeMergeEntry
	{
		std::vector<std::string> path;    // the incoming path, as in PartTypeNodeBundle::path
		TypeMergeAction action = TypeMergeAction::AddNew;
		int localTypeId = NoParentType;   // the matched local `part_type.id`; NoParentType for AddNew
		std::vector<FieldConflict> conflicts;
		// Attribute keys this merge would introduce on the local type. Not conflicts — there is
		// no local value to disagree with — but worth showing, because they change every part
		// editor form of that category.
		std::vector<std::string> addedAttributeKeys;
		// §11: the subset of addedAttributeKeys arriving `required`. These make every part already
		// filed under this category incomplete the moment the merge lands, which is exactly the
		// kind of thing that must be on screen before the user presses OK rather than after.
		std::vector<std::string> newRequiredAttributeKeys;
		int affectedPartCount = 0;        // parts currently filed directly under localTypeId
	};

	// The reviewable result of planMerge().
	struct PART_MANAGER_API MergePlan
	{
		std::vector<TypeMergeEntry> entries;     // one per bundle node, in bundle order (parents first)
		std::vector<int> unmatchedLocalTypeIds;  // local categories nothing incoming matched; untouched
		// Human-readable, untranslated English. Ambiguous names, datatype changes, re-parents —
		// everything the user should read before applying but that does not stop the merge.
		std::vector<std::string> warnings;
	};

	// A path as a person reads it: " › "-joined, the same glyph the type picker's partTypePath()
	// puts between category names, so one path looks the same wherever the app shows it. For
	// messages and UI only — a joined path is a sentence, not an identity (see file header).
	PART_MANAGER_API std::string displayPartTypePath(const std::vector<std::string>& segments);

	// The comparison form of a path: every segment trimmed and lowercased, still a list. This is
	// what makes {"capacitor", "ceramic capacitor"} and {"Capacitor", " Ceramic Capacitor "} the
	// same category — two people naming the same thing should not produce two categories.
	PART_MANAGER_API std::vector<std::string> normalizedPartTypePath(
		const std::vector<std::string>& segments);

	// Splits a **format v1** path string on '/'. Only fromJson() calls it, and only for a v1 file:
	// the split is lossy by construction, because a v1 writer had no way to escape a '/' that was
	// part of a category's own name (see file header). Nothing else in this codebase may re-split
	// a path.
	PART_MANAGER_API std::vector<std::string> splitLegacyPartTypePath(const std::string& path);

	// Serializes a bundle to JSON text. Always writes CurrentBundleFormatVersion, whatever the
	// bundle's own `formatVersion` says — a bundle read from a v1 file and written back out is a
	// v2 file, with its path as an array. Carries the `"format"` marker so a reader can tell one
	// of these from any other JSON file before parsing it.
	PART_MANAGER_API std::string toJson(const PartTypeBundle& bundle);

	// Parses bundle JSON. Reads both v2 (`"path"` an array of segments) and v1 (`"path"` one
	// '/'-joined string, split back on the way in). Returns false with a human-readable English
	// reason in `outError` when the text is not JSON, is not an object, carries no/an unknown
	// `"format"` marker, or was written by a newer build (`formatVersion` above
	// CurrentBundleFormatVersion) — the same "refuse when newer, never downgrade" stance §1c takes
	// for the database schema, for the same reason: this build cannot know what a field it has
	// never heard of means.
	PART_MANAGER_API bool fromJson(const std::string& text, PartTypeBundle& outBundle,
		std::string& outError);

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	// Reads categories out of an open database into a bundle. An empty `typeIds` means every
	// category; otherwise only those — plus **every ancestor of each**, added automatically,
	// because §2b makes a child meaningless without the parent it inherits from and a bundle
	// whose parent is missing is not importable at all.
	//
	// Nodes come out parents-first. Reads only; nothing is written.
	PART_MANAGER_API PartTypeBundle bundleFromDatabase(SQLiteWrapper::SQLite& db,
		const std::vector<int>& typeIds = std::vector<int>());

	// The same, but from *another* database file, which nothing in this codebase did before:
	// DatabaseHandle owns exactly one connection and there is no ATTACH anywhere. A second,
	// query-only connection is opened on the other file purely to read the type tables and is
	// closed again before this returns — the source database is never written to.
	//
	// `pmdbOrSqlitePath` may be either a `.pmdb` entry file, resolved to its sibling
	// `partmanager.db` exactly as DatabaseHandle does (§1a — every path is a fixed sibling name
	// relative to wherever the entry file sits), or that `.db` directly.
	//
	// Refuses, with a §1c-shaped message, a source whose `schema_version` is newer than this
	// build understands: its `part_type` tables may hold columns this reader would silently drop.
	PART_MANAGER_API bool bundleFromDatabaseFile(const std::string& pmdbOrSqlitePath,
		PartTypeBundle& outBundle, std::string& outError);

	// Works out what importing `bundle` into `db` would do. **Pure: it reads and it plans, it
	// writes nothing.** Run it, show it, let the user edit the actions and resolutions, then hand
	// the edited plan to applyMerge().
	//
	// Matching an incoming category to a local one, in order:
	//   1. **Full path**, compared segment by segment, each segment trimmed and case-insensitive.
	//      The strict case. Segment by segment and not as one string, so a category whose own name
	//      contains the separator is still exactly one segment (see file header).
	//   2. **Unique leaf name.** If no path matched, and the incoming category's own name matches
	//      exactly ONE local category anywhere in the tree (case-insensitively), those two are the
	//      same category. This is the common real case: one database has a flat `Resistor`, the
	//      other `Passive/Resistor`. They are one category and must merge — and the local one gets
	//      re-parented — rather than becoming two categories that mean the same thing.
	//   3. **Ambiguous leaf name — no match.** If the name fits more than one local category, the
	//      entry stays AddNew and a warning names the candidates. This codebase's standing rule
	//      (DECISIONS.md, 2026-09-26, `PartTypeMatcher`) is that the fix for a bad guess is not a
	//      better guess but a guess that knows it is unsure: a wrongly merged category rewrites
	//      attributes on parts that never belonged to it.
	//
	// A local category is claimed by at most one incoming one; the second claimant falls through
	// to AddNew.
	PART_MANAGER_API MergePlan planMerge(SQLiteWrapper::SQLite& db, const PartTypeBundle& bundle);

	// Executes `plan` against `db`, honouring every entry's `action` and every conflict's
	// `resolution` — a KeepLocal field is simply not written. The whole thing runs inside one
	// Transaction: any failure rolls back every table it had touched and returns false with the
	// reason in `outError`.
	//
	// **Invariant, and the load-bearing one: this never writes `part.attributes` and never
	// changes any `part.part_type_id`.** Merging categories reshapes the *template*; it does not
	// touch the parts filed under it. An attribute that disappears from a category leaves its
	// value sitting in the part's `attributes` JSON, unreferenced and intact — the user's
	// explicit decision to keep orphan values, because a value someone typed is data and a
	// template is only a description of it. Re-filing parts onto different categories is a
	// separate, later, user-driven step and is not something an import may do behind their back.
	//
	// A searchable numeric attribute is always inserted through
	// PartTypeRepository::insertAttribute(), so its `attr_<key>` fast-filter column is created
	// (§2) instead of appearing the first time some unrelated save happens to notice.
	//
	// A re-parent that would make a category its own ancestor is refused outright: a cycle in
	// `parent_type_id` breaks every §2b walk in the app, and half a tree is worse than no import.
	PART_MANAGER_API bool applyMerge(SQLiteWrapper::SQLite& db, const PartTypeBundle& bundle,
		const MergePlan& plan, std::string& outError);

	// The full path of one local category, root first — the same segment list bundling produces.
	// Empty when `typeId` names no category.
	PART_MANAGER_API std::vector<std::string> partTypePathOf(SQLiteWrapper::SQLite& db, int typeId);

#endif

}
