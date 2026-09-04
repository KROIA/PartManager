// @file PartManager_MainWindowController.h
// @brief Owns the open database for the main window and answers what it needs to display (§12b).
//
// Everything the Home tab shows — the category tree (§7a) and the part table
// (§7b) — is assembled here, so the view stays a dumb renderer. The assembly
// itself is split in two: free functions that are pure (types in, nodes/columns
// out; no SQL, no widgets — unit-tested in TST_MainWindowController) and the
// controller methods that just feed them repository rows.
//
// Selecting a category shows its OWN parts plus every descendant type's parts:
// "Capacitor" is a real category with real parts in the mockup's tree, and a
// user clicking it expects the ceramics and electrolytics under it too. The
// columns shown in that case are the selected type's effective attributes — a
// child's extra attributes are not merged in, since they don't exist on siblings.
// @see docs/design/ARCHITECTURE.md §7a, §7b, §2b, §2d, §12b
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_Part.h"
#include "domain/PartManager_PartType.h"
#include "domain/PartManager_PartTypeAttribute.h"
#include "domain/PartManager_PartTypeListColumn.h"
#include "domain/PartManager_Tag.h"
#include <QString>
#include <QStringList>
#include <map>
#include <memory>
#include <vector>

namespace PartManager
{

	// One node of the §7a category tree — a part_type plus its resolved subtree.
	struct CategoryNode
	{
		int typeId = NoParentType;
		QString name;                       // user data (the type's name) — never tr()'d
		// **What the tree's "(n)" shows.** Every part in the category, in stock or not. It used to
		// be inStockCount, which made adding a part with no opening stock look like nothing had
		// happened — the count the user had just changed was not the count on screen.
		int partCount = 0;                  // parts of any quantity, own + all descendants
		int inStockCount = 0;               // parts with stock_qty > 0, own + all descendants
		int matchCount = 0;                 // §7a tree filter hits, own + all descendants; 0 with no filter
		std::vector<CategoryNode> children;
	};

	// One column of the §7b part table, either a built-in or a part_type_attribute.
	struct PartColumn
	{
		QString key;                        // built-in key ('name', 'stock_qty', ...) or the attribute's key
		QString label;                      // header text; attribute labels are user data, built-ins are tr()'d
		bool isAttribute = false;
		QString unit;                       // §2a dropdown unit, empty for "(no unit)"; drives ValueParser::format()
		AttributeDataType datatype = AttributeDataType::Text;
		QString labelOverride;              // §7b renamed header, empty = `label` is the real one
		bool visible = true;                // §7b saved visibility; columnsFor() drops the hidden ones
		int widthPx = 0;                    // §7b saved width, 0 = size the column to its contents
	};

	// One rendered row of the §7b part table.
	// Which of the four §3 file slots a part actually carries. A bitmask rather than four bools
	// because the table paints them as one strip of glyphs and sorts on the whole set: "show me
	// the parts that still have nothing attached" is the question this column exists to answer.
	enum PartAttachment
	{
		AttachmentDatasheet = 1 << 0,
		AttachmentKicadSymbol = 1 << 1,
		AttachmentKicadFootprint = 1 << 2,
		Attachment3DModel = 1 << 3,
	};

	struct PartRow
	{
		int partId = 0;
		QStringList cells;                  // one entry per PartColumn, already display-formatted
		std::vector<Tag> tags;              // §2d chips, rendered by TagChipDelegate
		int stockQty = 0;
		int stockMinQty = 0;
		// Absolute path of the part's `part_file(role='image')`, empty when it has none or the
		// stored file is gone. Painted as a thumbnail in the first column so a part is
		// recognisable without reading the row.
		QString imagePath;
		// The part's type, which decides the placeholder glyph when `imagePath` is empty —
		// which is the normal state, not an exception (a CSV import brings no photos at all).
		QString typeName;
		// OR of PartAttachment. Painted as the "Files" column's glyph strip, so a part missing
		// its footprint is visible from the list instead of only from inside the editor.
		int attachments = 0;
	};

	// One label/value line of the §7c preview panel.
	struct PreviewField
	{
		QString label;                      // attribute labels are user data, built-ins are tr()'d
		QString value;                      // already display-formatted, same string the table cell holds
	};

	// Everything the §7c preview panel shows for one part. A partId of 0 is the empty
	// state — nothing selected, or a part id that no longer resolves.
	struct PartPreview
	{
		int partId = 0;
		QString name;                       // user data, the panel's title
		QString description;                // user data
		std::vector<PreviewField> fields;   // manufacturer, mpn, package, attributes, stock, datasheet
		std::vector<Tag> tags;              // §2d chips
		QString imagePath;                  // same file the table's thumbnail comes from, shown full size
		// §5a KiCad previews. The paths are empty when the part has nothing attached, which is
		// the normal case — `typeName` is then what decides which generated base gets drawn, so
		// the panel can still show what the part will look like once its library is generated.
		QString kicadSymbolPath;
		QString kicadFootprintPath;
		// §13's 3D model, empty when the part has none. A STEP file here is a path to convert,
		// not to draw — the viewer decides that, because the answer depends on a cache the
		// controller has no business knowing about.
		QString model3DPath;
		// The stored datasheet, empty when the part has none or the file is gone — which is what
		// the preview panel's Datasheet button greys itself out on.
		QString datasheetPath;
		QString typeName;
	};

	// Assembles the part_type forest from flat rows, honoring parent_type_id (§2b).
	// inStockByType maps a type id to the count of ITS OWN in-stock parts; each node's
	// inStockCount comes back as that count plus every descendant's. Children are sorted
	// by name; a row whose parent is missing (or which sits in a parent cycle) becomes a root.
	// matchByType is the same thing for the §7a tree filter's per-type hit counts.
	// partCountByType is the same shape again, for the total the tree label shows.
	std::vector<CategoryNode> buildCategoryTree(const std::vector<PartType>& types,
		const std::map<int, int>& inStockByType,
		const std::map<int, int>& matchByType = std::map<int, int>(),
		const std::map<int, int>& partCountByType = std::map<int, int>());

	// Parse error for a §7a filter box, empty when the text parses (or is blank). The box is
	// a filter, so this exists only to explain a typo — a valid query that matches nothing
	// still reports no error.
	QString searchError(const QString& filterText);

	// typeId plus every type below it, so selecting a parent lists its children's parts (see header note).
	std::vector<int> typeIdWithDescendants(const std::vector<PartType>& types, int typeId);

	// §7b default columns for a type: the built-ins the mockup shows, with the type's effective
	// attributes (declaration order) slotted in before the stock column. This is the full set of
	// columns a category *can* show — `part_type_list_column` only ever reorders/hides/resizes
	// what comes out of here, it can never invent a column.
	std::vector<PartColumn> deriveColumns(const std::vector<PartTypeAttribute>& effectiveAttributes);

	// §7b columns for the all-categories scope (§7a): the built-ins plus a "type" column naming
	// the category each row came from, and deliberately no attributes — the rows span every type,
	// so a `resistance` column would be blank on almost all of them. Not a special case in the
	// table: it is an ordinary PartColumn list, so sorting, widths and the glyph columns work as
	// they do for a category.
	std::vector<PartColumn> allCategoryColumns();

	// Lays a saved §7b layout over the derived columns. An empty `config` (the untouched state,
	// and every database that predates the table) returns `derived` unchanged.
	//
	// Otherwise the saved order/visibility/width/label wins, but the derived set still decides
	// what exists: a saved key that no longer resolves is dropped, and a column the type gained
	// after the layout was saved (a new attribute) is appended, visible, rather than vanishing.
	// `name` is always forced first and visible — the table hangs the part id and the §2d tag
	// chips off column 0, so a layout that moved or hid it would break selection.
	std::vector<PartColumn> applyColumnConfig(const std::vector<PartColumn>& derived,
		const std::vector<PartTypeListColumn>& config);

	// The inverse: the rows that persist `columns` as-is, ready for ListColumnRepository::saveColumns().
	std::vector<PartTypeListColumn> toColumnConfig(const std::vector<PartColumn>& columns);

	// Pulls one column's value out of a part's `attributes` JSON and renders it for display —
	// dimensioned values go through ValueParser::format(), so 4700 Ω shows as "4.7 kΩ" (§2a).
	QString formatAttributeValue(const QString& attributesJson, const PartColumn& column);

	// One column's display text for one part — built-ins read the record's fields, attributes go
	// through formatAttributeValue(). The table cell and the preview line are literally the same
	// string, so the panel can never disagree with the row above it.
	QString formatCell(const Part& part, const PartColumn& column);

	// Display text for the §3 datasheet slot: "None", the stored file's name, or that name
	// marked missing when the `part_file` row points at a file that is no longer on disk.
	QString datasheetState(const QString& fileName, bool onDisk);

	// §7c preview fields for a part: every column except `name` (which is the panel's title),
	// with empty values dropped and the datasheet state appended last. `part.stockQty` is
	// expected to already carry the authoritative quantity (see previewFor()).
	PartPreview buildPreview(const Part& part, const std::vector<PartColumn>& columns,
		const std::vector<Tag>& tags, const QString& datasheet);

	class MainWindowController
	{
	public:
		explicit MainWindowController(std::unique_ptr<DatabaseHandle> handle);

		// The open database, still owned here — the editor/New Part/Manage Tags dialogs
		// take a non-owning pointer so they read and write the same connection.
		DatabaseHandle* handle() const;

		// Display name of the open database — its folder name (§1, no stored name field).
		QString databaseName() const;
		// Path of the open database's .pmdb entry file.
		QString pmdbPath() const;

		// The whole §7a category forest with live in-stock counts. A non-empty filterText also
		// fills each node's matchCount — the tree filter counts hits across all categories, it
		// never hides a category (§7a).
		std::vector<CategoryNode> categoryTree(const QString& filterText = QString()) const;

		// §7b columns for one category, saved layout applied, hidden ones already dropped —
		// what the table and the preview panel actually render.
		std::vector<PartColumn> columnsFor(int typeId) const;

		// The same list with the hidden columns still in it, for the "Customize columns..." dialog.
		std::vector<PartColumn> allColumnsFor(int typeId) const;

		// Persists the dialog's result as this category's own §7b layout.
		bool saveColumns(int typeId, const std::vector<PartColumn>& columns) const;

		// "Reset to default": drops this category's saved layout, so it derives its columns again.
		bool resetColumns(int typeId) const;

		// Persists one column's width after the user drags a header divider. The first drag on a
		// never-customized category materializes its current layout, so the width has somewhere
		// to live without silently reordering everything else.
		bool saveColumnWidth(int typeId, const QString& columnKey, int widthPx) const;

		// Rows for one category, including every descendant type's parts (see header note).
		// A non-empty filterText keeps only the rows the §7a query matches; text that fails to
		// parse matches nothing, same as SearchEngine.
		//
		// `allCategories` drops the type scope entirely (§7a's Ctrl+F search): `typeId` is then
		// ignored and the same query runs over every part in the database. Only the scope changes
		// — it is the same grammar through the same SearchEngine.
		std::vector<PartRow> partsFor(int typeId, const std::vector<PartColumn>& columns,
			const QString& filterText = QString(), bool allCategories = false) const;

		// §7c preview panel content for one part. A partId of 0, or one that no longer resolves,
		// comes back as the empty PartPreview the panel renders as its empty state.
		PartPreview previewFor(int partId) const;

	private:
		std::unique_ptr<DatabaseHandle> m_handle;
	};

}
