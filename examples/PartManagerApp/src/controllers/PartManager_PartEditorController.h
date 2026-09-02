// @file PartManager_PartEditorController.h
// @brief Widget-free logic behind the part editor, New Part and Manage Tags screens (§12b).
//
// Same split as MainWindowController: the parts that are pure (the `attributes`
// JSON read/write, §11 required-field validation, which widget a datatype gets,
// the tag set arithmetic) are free functions unit-tested in
// TST_PartEditorController; the controller class is the thin repository wrapper
// the dialogs talk to.
//
// **`attributes` JSON shape** (§2a leaves the `unit` field ambiguous — this is
// the interpretation the whole app uses, read and write): a `dimension`
// attribute is `{"<key>": {"value": <base-SI number>, "unit": "<the field's
// dropdown unit>"}}` — so a 4k7 resistor stores `{"value": 4700, "unit": "Ω"}`,
// never the typed prefix. The prefix the user sees is re-derived on display by
// ValueParser::format(), which renders engineering notation anyway, and the
// stored number is then the same one that goes into the `attr_*` column, so the
// two can never drift. Number/Text/Bool/Enum store a bare JSON value. A field
// left empty is omitted from the object entirely rather than stored as null.
// @see docs/design/ARCHITECTURE.md §2a, §2d, §3, §6, §10, §11, §12b
// @see PartManager_MainWindowController.h, PartManager_AttributeFormWidget.h, PartManager_FileStore.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_Part.h"
#include "domain/PartManager_PartFile.h"
#include "domain/PartManager_PartType.h"
#include "domain/PartManager_PartTypeAttribute.h"
#include "domain/PartManager_PartTypeFileSlot.h"
#include "domain/PartManager_Seller.h"
#include "domain/PartManager_Tag.h"
#include "domain/PartManager_TagCategory.h"
#include <QString>
#include <map>
#include <string>
#include <vector>

namespace PartManager
{

	// One attribute's value as the generated form holds it, datatype-agnostic.
	// `present == false` means the user left the field empty — the key is then
	// left out of the JSON instead of being written as null or 0.
	struct AttributeValue
	{
		bool present = false;
		double number = 0.0;    // Number and Dimension (base-SI)
		QString text;           // Text and Enum (user data)
		bool flag = false;      // Bool
	};

	// Which editor widget an attribute gets on the generated form.
	enum class AttributeWidgetKind
	{
		Dimension,   // DimensionLineEdit, SI-prefix aware (§2a)
		Number,      // plain numeric line edit
		Text,
		Bool,
		Enum         // combo box over enumOptions
	};

	// Maps a datatype to its widget. An Enum with no options would render an
	// unusable empty combo, so it falls back to a free-text field.
	AttributeWidgetKind widgetKindFor(const PartTypeAttribute& attribute);

	// Parses a part's `attributes` JSON into one value per declared attribute (see header
	// note for the shape). Keys the JSON does not carry come back with present == false;
	// keys the type no longer declares are dropped. A dimension stored as a bare number
	// instead of an object is read as an already-base-SI value rather than discarded.
	std::map<std::string, AttributeValue> readAttributesJson(const QString& attributesJson,
		const std::vector<PartTypeAttribute>& attributes);

	// The inverse — round-trips with readAttributesJson for every datatype.
	QString writeAttributesJson(const std::vector<PartTypeAttribute>& attributes,
		const std::map<std::string, AttributeValue>& values);

	// §11: keys of `required` attributes still without a value. Empty => nothing blocks Create.
	// A required Text/Enum field counts as filled only when its text is non-empty; a required
	// Bool counts as filled either way, since a checkbox always has an answer.
	std::vector<std::string> missingRequiredKeys(const std::vector<PartTypeAttribute>& attributes,
		const std::map<std::string, AttributeValue>& values);

	// §2d: which of the managed tags a part does not carry yet — what the editor's
	// "+ Tag" menu offers. Input order is preserved, matching is by tag id.
	std::vector<Tag> availableTagsToAdd(const std::vector<Tag>& allTags, const std::vector<Tag>& partTags);

	// Repository wrapper shared by PartEditorDialog, NewPartDialog and ManageTagsDialog.
	// Holds the caller's DatabaseHandle without owning it — MainWindow's controller does.
	class PartEditorController
	{
	public:
		explicit PartEditorController(DatabaseHandle* handle);

		std::vector<PartType> types() const;
		std::vector<PartTypeAttribute> attributesFor(int typeId) const;
		std::vector<PartTypeFileSlot> fileSlotsFor(int typeId) const;

		bool loadPart(int partId, Part& outPart) const;
		// §10 autosave: writes an already-existing record, no save button anywhere.
		bool savePart(const Part& part) const;
		// Returns the new part's id, 0 on failure. Default tags are seeded inside
		// PartRepository::insertPart() (§2d), not here.
		int createPart(const Part& part) const;

		// §3 datasheet slot. All four only touch `part.datasheetFileId` in memory — writing the
		// part back is the caller's §10 autosave, so there is still exactly one save path.
		// Attaching or downloading over an existing datasheet detaches the old one first, so a
		// part never leaves a `part_file` row behind that nothing points at.
		// Both return the new part_file id, 0 on failure with the reason in outError.
		int attachDatasheet(Part& part, const std::string& sourcePath, std::string* outError = nullptr) const;
		// §6: fetches the Mouser DataSheetUrl. Blocking with a timeout, see FileStore::downloadFile().
		int downloadDatasheet(Part& part, const std::string& url, std::string* outError = nullptr) const;
		// Removes the current datasheet. False when the part carried none, or the row was already gone.
		bool detachDatasheet(Part& part) const;
		// The part's datasheet `part_file` row. False when it carries none.
		bool datasheetFile(const Part& part, PartFile& outFile) const;
		// Absolute path of the stored datasheet — empty when there is none, or the file
		// is no longer on disk, which is what tells the editor to say so instead of opening nothing.
		std::string datasheetPath(const Part& part) const;

		// Single-slot `part_file` roles — the §13 3D model ('kicad_3dmodel') and the product
		// photo ('image'). A part carries at most one of each, and unlike the datasheet there is
		// no column on `part` pointing at it, so these read and write `part_file` directly and
		// need no autosave from the caller. Attaching over an existing one detaches it, so a part
		// never accumulates two rows for the same slot.
		//
		// **Any recognised 3D format is accepted, renderable or not.** A STEP file is exactly
		// what KiCad wants and exactly what the viewer cannot draw; refusing it to keep the
		// viewer happy would break the more important half.
		// Returns the new part_file id, 0 on failure with the reason in outError.
		int attachRoleFile(int partId, PartFileRole role, const std::string& sourcePath,
			std::string* outError = nullptr) const;
		// Same, from a URL — §6 downloads the Mouser product photo this way. Blocking with a
		// timeout, see FileStore::downloadFile().
		int downloadRoleFile(int partId, PartFileRole role, const std::string& url,
			std::string* outError = nullptr) const;
		// Same, for content that never existed as a file: the `.kicad_sym`/`.kicad_mod` text
		// EasyEdaConverter produces (§5c). Writing it to a temp file first only to import it back
		// would leave a copy of the part's geometry outside the store.
		int attachRoleBytes(int partId, PartFileRole role, const std::string& bytes,
			const std::string& filename, std::string* outError = nullptr) const;
		// The part's row for that slot. False when it carries none.
		bool roleFile(int partId, PartFileRole role, PartFile& outFile) const;
		// Absolute path of the stored file — empty when there is none, or it is no longer on
		// disk, which is what tells the viewer to say so instead of drawing nothing.
		std::string roleFilePath(int partId, PartFileRole role) const;
		// Removes the current file in that slot. False when the part carried none.
		bool detachRoleFile(int partId, PartFileRole role) const;

		int attachModel3D(int partId, const std::string& sourcePath, std::string* outError = nullptr) const
		{ return attachRoleFile(partId, PartFileRole::Kicad3DModel, sourcePath, outError); }
		bool model3DFile(int partId, PartFile& outFile) const
		{ return roleFile(partId, PartFileRole::Kicad3DModel, outFile); }
		std::string model3DPath(int partId) const
		{ return roleFilePath(partId, PartFileRole::Kicad3DModel); }
		bool detachModel3D(int partId) const
		{ return detachRoleFile(partId, PartFileRole::Kicad3DModel); }

		// What one vendor-ZIP import did (§5c).
		struct EcadImportSummary
		{
			bool ok = false;
			std::string errorMessage;
			bool symbolAttached = false;
			bool footprintAttached = false;
			bool modelAttached = false;
			bool legacyKicadOnly = false;   // the archive had KiCad 5 files only
			int ignoredEntries = 0;         // the other CAD tools' files
		};

		// §5c: pulls the KiCad symbol, footprint and 3D model out of a vendor download
		// (`LIB_<MPN>.zip` from Component Search Engine, Ultra Librarian, SnapEDA) and attaches
		// them to the part, each replacing whatever held its slot. Nothing is linked — the bytes
		// are copied into the filestore like every other attachment, so the ZIP can be deleted.
		//
		// The symbol then becomes what §5a splices into the generated library instead of the
		// generic `(extends ...)` one.
		EcadImportSummary importEcadArchive(int partId, const std::string& zipPath) const;

		// Deletes the part outright, together with its part_file, part_tag and stock_transaction
		// rows (PartRepository::deletePart()). Its seller links go too — otherwise the next part
		// to reuse the id would inherit somebody else's Mouser article number.
		bool deletePart(int partId) const;

		// §3/§6: remembers that this part is a Mouser article, and what it was quoted at. Without
		// this a part created from a Mouser hit keeps no trace of where it came from, and item 9's
		// cart staging has no article number to send — `part.mpn` is the *manufacturer's* number,
		// which the Cart API rejects. Returns the part_seller_link id, 0 on failure.
		int linkToMouser(int partId, const std::string& mouserPartNumber, const std::string& url,
			const std::vector<PriceObservation>& quote) const;
		// Every seller link on a part, primary first — the part editor's "Open on Mouser" row.
		std::vector<PartSellerLink> sellerLinks(int partId) const;

		// The part's Mouser article number, empty when it has none. This is the number the Cart
		// API orders by; `part.mpn` is the manufacturer's and Mouser rejects it.
		std::string mouserPartNumber(int partId) const;
		// The stored ProductDetailUrl, empty for a hand-typed number that never came from a search.
		std::string mouserUrl(int partId) const;
		// Sets (or, with an empty `number`, removes) the part's Mouser link. Editable because a
		// part imported from CSV — or one that duplicates a row created earlier — has no link and
		// cannot be ordered until it does.
		bool setMouserPartNumber(int partId, const std::string& number,
			const std::string& url = std::string()) const;

		// Parts that already carry `mpn`, excluding `exceptPartId`. Used to warn before creating
		// a second row for a part that already exists — the duplicate is not blocked, because two
		// genuinely different parts can share an MPN across manufacturers, but it is never
		// created silently. Empty `mpn` matches nothing: a part with no MPN duplicates nothing.
		std::vector<Part> partsWithMpn(const std::string& mpn, int exceptPartId = 0) const;

		// The page "Open on Mouser" opens: the stored product URL when there is one, otherwise a
		// Mouser search for the article number. Empty when the part has neither.
		static std::string mouserPageUrl(const std::string& mouserPartNumber, const std::string& storedUrl);

		std::vector<Tag> allTags() const;
		std::vector<Tag> partTags(int partId) const;
		bool addPartTag(int partId, int tagId) const;
		bool removePartTag(int partId, int tagId) const;

		// Manage Tags dialog (§2d) — the managed vocabulary itself.
		int createTag(const Tag& tag) const;
		bool updateTag(const Tag& tag) const;
		bool deleteTag(int tagId) const;

		// §2d's second level: the families tags are grouped under. Deleting one leaves its tags
		// behind as uncategorised — see TagRepository::deleteCategory.
		std::vector<TagCategory> tagCategories() const;
		int createTagCategory(const TagCategory& category) const;
		bool updateTagCategory(const TagCategory& category) const;
		bool deleteTagCategory(int categoryId) const;
		bool setTagCategory(int tagId, int categoryId, bool recolour = true) const;

	private:
		DatabaseHandle* m_handle;
	};

}
