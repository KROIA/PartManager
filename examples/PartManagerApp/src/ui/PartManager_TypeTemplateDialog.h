// @file PartManager_TypeTemplateDialog.h
// @brief The §2/§11 Type Template editor — what a part type declares: its fields and its files.
//
// The type tree on the left is the `parent_type_id` forest (§2b), so a type sits
// under the one it inherits from; the two tables on the right show that type's
// *effective* attributes and file slots, inherited rows included and greyed, so
// it is visible at a glance which rows can be edited here and which would need an
// override row on this type first.
//
// **`key` and `role` are chosen once.** A key is the name a part's value is stored
// under in the `attributes` JSON and the name of the `attr_<key>` fast-filter
// column; renaming it would leave every existing part's value under a name
// nothing reads any more, and nothing would report it. So the key is asked for
// when the row is created — validated by attributeKeyProblem(), against the
// effective set so an inherited key cannot be shadowed by accident — and is
// read-only from then on. A role is the same story against `part_file.role`, and
// is picked from the fixed PartFileRole vocabulary rather than typed.
//
// Marking a numeric attribute searchable adds its `attr_<key>` column to `part`
// (PartTypeRepository's documented side effect). Unmarking it does **not** drop
// the column — dropping a column is the one schema change that can lose data, and
// the unused column costs nothing. The Searchable header says so.
//
// Reachable from the Parts ribbon tab's *Manage* group. Every action writes
// through immediately (there is no OK/Cancel over the tables), matching §10 and
// what the mockup says in so many words.
// @see docs/design/ARCHITECTURE.md §2, §2a, §2b, §10, §11
// @see docs/design/mockups/type-template-editor.svg
// @see PartManager_PartEditorController.h, PartManager_ManageTagsDialog.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include <QDialog>

class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QSyntaxHighlighter;
class QTableWidget;
class QTableWidgetItem;
namespace Ui { class TypeTemplateDialog; }

namespace PartManager
{

	class KeywordCheckList;

	class TypeTemplateDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit TypeTemplateDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~TypeTemplateDialog() override;

	private slots:
		void onNewType();
		void onDeleteType();
		void onTypeSelectionChanged();

		// The form above the tables. Each writes the whole `part_type` row back (§10) — there is
		// one field per row here, so a per-field update would buy nothing over one statement.
		// Opens PartTypePickerDialog on the parent field, with this type's own subtree hidden so a
		// parent cycle cannot be picked in the first place.
		void chooseParentType();
		void onTypeFieldEdited();
		// §14c: writes the row like any other field, then repaints the swatch and the tree row so
		// the choice is visible where it will actually be seen rather than only in the combo.
		void onIconChanged();

		void onAddAttribute();
		void onRemoveAttribute();
		void onEditEnumOptions();
		void onAttributeSelectionChanged();
		void onAttributeItemChanged(QTableWidgetItem* item);
		void onTooltipEdited();

		// The §2b category transfer, repeated here from the ribbon because this is where categories
		// are actually edited: noticing that the tree needs a category another database already has
		// happens in front of this tree, not in front of the part table. Import rebuilds the tree
		// afterwards — the merge can rename, re-parent or re-attribute anything in it, including
		// whatever row is selected right now.
		void onExportCategories();
		void onImportCategories();
		// The optional tail of an import (§11), reopenable long after the import itself. No plan is
		// handed over: there is none any more, and the incomplete-parts half never needed one.
		void onReconcileCategories();

		void onAddFileSlot();
		void onRemoveFileSlot();
		void onFileSlotSelectionChanged();
		void onFileSlotItemChanged(QTableWidgetItem* item);

	private:
		// Re-reads everything. The selection is restored afterwards — every edit that changes the
		// forest rebuilds the tree, and losing the selection mid-edit is jarring. `selectTypeId`
		// overrides which type that is, for a type that did not exist before the rebuild.
		void refreshTypeTree(int selectTypeId = NoParentType);
		// The selected type's own fields, and the parent combo (which must offer neither the type
		// itself nor anything below it, or the §2b walk would run in a cycle).
		void refreshTypeForm();
		// `selectAttributeId`/`selectFileSlotId` name the row to leave selected once the table has
		// been rebuilt — a reorder has to restore the selection by id, because the row index is
		// exactly what it changed. 0 (the default) leaves nothing selected.
		void refreshAttributeTable(int selectAttributeId = 0);
		void refreshFileSlotTable(int selectFileSlotId = 0);
		void updateButtons();

		// The selected type, or a default-constructed one (id == NoParentType) when nothing is.
		PartType selectedType() const;
		// The selected attribute/file slot, by the id carried on column 0. Ids of 0 mean nothing
		// is selected — every row in these tables is already a saved row.
		int selectedAttributeId() const;
		int selectedFileSlotId() const;

		// The type's own rows out of the effective list, in the order the tables paint them.
		// Only these can be reordered or removed here; an inherited row belongs to its ancestor.
		std::vector<PartTypeAttribute> ownAttributes() const;
		std::vector<PartTypeFileSlot> ownFileSlots() const;
		// Swaps the selected own row with its neighbour `offset` places away and renumbers the
		// whole own group's sort_order. Nothing happens at either end.
		void moveSelectedAttribute(int offset);
		void moveSelectedFileSlot(int offset);

		// Fills the "Insert…" menu with one entry per effective attribute plus the three part
		// fields, each inserting its `{key}` into the naming pattern. Rebuilt on every open.
		void refreshInsertKeyMenu();
		// Re-reads which keys exist, repaints the pattern's placeholders (green = a key this type
		// has, orange = not one yet) and re-renders the example name under it. Called whenever the
		// pattern or the attribute list changes, which are the only two things it depends on.
		// **Not callable from the pattern field's textChanged** — see the note on its rehighlight().
		void updateNameTemplateFeedback();
		// Just the example name. This is the half that is safe to run on every keystroke.
		void updateNamePreview();

		// Writes one table row back to its `part_type_attribute` / `part_type_file_slot` row.
		void saveAttributeRow(int row);
		void saveFileSlotRow(int row);

		// §14c. Both combos are filled from TypeIconStyle rather than from a list spelled here,
		// so the picker, the stored column and the `set_category_icon` tool offer one vocabulary.
		// Each entry carries its canonical name as item data; the visible text is translated and
		// the canonical name is what is written, so the two never have to agree.
		void fillIconPickers();
		// The swatch beside the combos and the glyph on the selected tree row — the two places the
		// choice is actually judged. Cheap enough to run on every change: TypeIconPainter caches.
		void updateIconPreview();
		// The two vocabularies as the user reads them. Separate from the stored spelling on
		// purpose: a German user looks for "Widerstand", the column holds "Resistor".
		static QString glyphDisplayName(const QString& canonical);
		static QString colourDisplayName(const QString& canonical);

		Ui::TypeTemplateDialog* m_ui;
		// Borrowed, never owned — kept because the parent-type picker opens on the same database.
		DatabaseHandle* m_handle;
		PartEditorController m_controller;
		// §7a: the search words this type inherits from its ancestors, one tick box each. Unticking
		// one drops it for this type and for everything under it.
		KeywordCheckList* m_inheritedKeywords = nullptr;
		// §11: the naming pattern, and the menu that inserts a placeholder for one of this type's
		// attributes into it — the syntax and the click-together way of writing it are the same
		// field, so a pattern can be assembled without knowing the keys or typed if you do.
		// A QPlainTextEdit sized to one line rather than a QLineEdit: the placeholders are coloured
		// as they are typed, and a QSyntaxHighlighter needs a QTextDocument to attach to.
		QPlainTextEdit* m_nameTemplateEdit = nullptr;
		QSyntaxHighlighter* m_nameHighlighter = nullptr;
		// The keys the highlighter paints green — this type's effective attributes plus the three
		// part fields. Held here rather than in the highlighter so refreshing the attribute table
		// is all it takes to keep the colours honest; the highlighter reads this list by pointer.
		QStringList m_knownTemplateKeys;
		QLabel* m_namePreviewLabel = nullptr;
		QMenu* m_insertKeyMenu = nullptr;

		// The rows behind the widgets. Every accessor reads from here rather than off the table,
		// so a field the editor does not paint — enumOptions, the tooltip — survives a cell edit
		// instead of being written back as whatever the row happens to show.
		std::vector<PartType> m_types;
		std::vector<PartTypeAttribute> m_attributes;   // effective, inherited rows included
		std::vector<PartTypeFileSlot> m_fileSlots;     // effective, inherited rows included

		// Set while a refresh is repopulating the widgets, so the change signals the repopulation
		// itself emits are not mistaken for the user editing a field.
		bool m_reloading = false;

		// The parent shown on the parent button, NoParentType for a root type. The button carries a
		// label, not a value, so the id it stands for is held here — one place, the way New Part
		// holds the type it is creating.
		int m_parentTypeId = NoParentType;
	};

}
