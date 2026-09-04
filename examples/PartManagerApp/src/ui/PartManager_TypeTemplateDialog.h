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
		void onTypeFieldEdited();

		void onAddAttribute();
		void onRemoveAttribute();
		void onEditEnumOptions();
		void onAttributeSelectionChanged();
		void onAttributeItemChanged(QTableWidgetItem* item);
		void onTooltipEdited();

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

		Ui::TypeTemplateDialog* m_ui;
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
	};

}
