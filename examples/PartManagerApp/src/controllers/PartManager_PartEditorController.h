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
// @see docs/design/ARCHITECTURE.md §2a, §2d, §10, §11, §12b
// @see PartManager_MainWindowController.h, PartManager_AttributeFormWidget.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_Part.h"
#include "domain/PartManager_PartType.h"
#include "domain/PartManager_PartTypeAttribute.h"
#include "domain/PartManager_PartTypeFileSlot.h"
#include "domain/PartManager_Tag.h"
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

		std::vector<Tag> allTags() const;
		std::vector<Tag> partTags(int partId) const;
		bool addPartTag(int partId, int tagId) const;
		bool removePartTag(int partId, int tagId) const;

		// Manage Tags dialog (§2d) — the managed vocabulary itself.
		int createTag(const Tag& tag) const;
		bool updateTag(const Tag& tag) const;
		bool deleteTag(int tagId) const;

	private:
		DatabaseHandle* m_handle;
	};

}
