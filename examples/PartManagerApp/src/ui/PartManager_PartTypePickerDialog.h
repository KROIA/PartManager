// @file PartManager_PartTypePickerDialog.h
// @brief "Pick a category" as the tree it actually is (§2b), for every dialog that used a flat combo.
//
// `part_type.parent_type_id` makes the categories a forest, and §2b inheritance
// only makes sense read that way: "Ceramic Capacitor" means what it means
// *because* it hangs under "Capacitor". A QComboBox flattens that away — every
// type on one alphabetical list, a subtype sitting next to a root it has nothing
// to do with, and no way to tell which is which. On a catalogue with a handful of
// types that was merely lossy; on a deep one it is unreadable.
//
// So the picker is the browser the user already knows: the same forest builder the
// Home tab's category tree uses (buildCategoryTree), the same glyphs at the same
// CategoryGlyphSize. Recognising it should cost nothing.
//
// Deliberately *not* the part counts the main window's tree shows. That number
// costs one listParts() per type over there (see MainWindowController::
// categoryTree's own ponytail note) and a picker is opened to answer "where does
// this part go", not "how full is each shelf" — a per-row query to decorate a
// dialog that is closed two seconds later is the wrong trade.
//
// "No category" is a button in the button row rather than a row at the top of the
// tree. A tree row would have to be exempted from the filter by hand — otherwise
// typing anything hides the one answer that is always available — and it would
// need selecting *and* confirming, where the button is one click. The button also
// lets each caller word it for itself: New Part asks for a category, the type
// template editor asks for a parent, and "make it a root type" is not "no
// category".
// @see docs/design/ARCHITECTURE.md §2, §2b, §7a
// @see PartManager_MainWindow.h (addCategoryItem — the look this reproduces)
// @see PartManager_TypeIconPainter.h, PartManager_MainWindowController.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_PartType.h"
#include <QDialog>
#include <QString>
#include <vector>

class QPushButton;
class QTreeWidgetItem;

namespace Ui { class PartTypePickerDialog; }

namespace PartManager
{

	struct CategoryNode;

	// The `Capacitor › Ceramic Capacitor` label for one type, so the control that opens the picker
	// can show the inherited context instead of a leaf name that could sit anywhere. Empty for
	// NoParentType and for an id the list does not contain; a parent chain that loops stops rather
	// than growing forever, the same way PartTypeRepository::ancestorChainRootFirst() does.
	QString partTypePath(const std::vector<PartType>& types, int typeId);

	class PartTypePickerDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `preselectedTypeId` may be NoParentType (0), meaning nothing is selected yet.
		//
		// `excludeSubtreeOfTypeId` hides one type *and everything under it*. Its only caller is the
		// parent-type field of the type template editor, where offering a type itself or one of its
		// descendants would build a parent cycle and the §2b walk would never reach a root.
		PartTypePickerDialog(DatabaseHandle* handle, int preselectedTypeId,
			QWidget* parent = nullptr);
		PartTypePickerDialog(DatabaseHandle* handle, int preselectedTypeId,
			int excludeSubtreeOfTypeId, QWidget* parent);
		~PartTypePickerDialog() override;

		// The type the user settled on. NoParentType (0) both when the dialog was cancelled and
		// when they pressed the "no category" button — the caller tells the two apart by exec()'s
		// result, exactly as it already does for every other field.
		int selectedTypeId() const;

		// What the "no category" button says. New Part's wording is the default; the type template
		// editor's parent field says "make it a root type" instead.
		void setNoTypeButtonText(const QString& text);

		// The types this picker was built from, so a caller that needs the full path of the answer
		// does not have to read the database a second time to spell it.
		const std::vector<PartType>& types() const;

	private:
		// Shared by the two constructors — Qt5 has no delegating-constructor problem here, but the
		// .ui setup and every connection would otherwise be written twice.
		void build(int preselectedTypeId, int excludeSubtreeOfTypeId);
		// Fills the tree under `parent` from the forest builder's nodes. `excluded` is checked at
		// every level rather than only at the roots — the type a parent field must not offer is
		// almost always somebody's child, so a root-only check would exclude nothing at all.
		// `outPreselected` comes back pointing at the item carrying `preselectedTypeId`, if one
		// was built.
		void addTypeItem(const CategoryNode& node, QTreeWidgetItem* parent,
			const std::vector<int>& excluded, int preselectedTypeId,
			QTreeWidgetItem*& outPreselected);
		// Hides what the filter does not match. An item survives when its own name matches or when
		// any descendant's does — without that second half a filtered child is unreachable, since
		// hiding its parent hides it too.
		void applyFilter(const QString& filter);
		// True when `item` or anything under it matches, hiding the rest as it goes.
		bool filterItem(QTreeWidgetItem* item, const QString& filter);
		// OK follows the selection: nothing selected, nothing to confirm.
		void updateOkState();

		Ui::PartTypePickerDialog* m_ui;
		std::vector<PartType> m_types;
		QPushButton* m_okButton = nullptr;
		QPushButton* m_noTypeButton = nullptr;
		int m_selectedTypeId = NoParentType;
	};

}
