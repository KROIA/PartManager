// @file PartManager_FootprintVariantsDialog.h
// @brief Browse the footprints that share a package but not their pads (§5a).
//
// Reached from the ribbon's KiCad tab, beside Generate Libraries — the same tab
// the generation screen is on, because this is the screen that answers what
// generation reported. Naming footprints after the package found **13 package
// names claimed by different pad layouts** in the user's real data and shared
// nothing at all, so the consolidation the naming promised needs a human to say
// which layout is authoritative.
//
// **This slice is read-only on purpose.** It browses, groups and draws. Choosing
// a winner and reassigning the other parts onto it is a write, it has to be
// undoable, and it is the next slice; building the looking half first is what
// lets that one be designed as a transaction instead of as a gesture bolted onto
// a view.
//
// The tree is the input device: moving the *pointer* down it — not clicking —
// brings each variant to the front of the overlay, so a group can be read by
// swiping through it. Leaving the tree falls back to the selected row rather
// than to nothing, or the view would blank every time the mouse crossed a gap.
// @see docs/design/ARCHITECTURE.md §5a
// @see PartManager_KicadVariantView.h, PartManager_FootprintVariants.h
#pragma once

#include "controllers/PartManager_KicadController.h"
#include "kicad/PartManager_FootprintVariants.h"
#include <QDialog>
#include <vector>

class QTreeWidgetItem;

namespace Ui { class FootprintVariantsDialog; }

namespace PartManager
{

	class KicadVariantView;

	class FootprintVariantsDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit FootprintVariantsDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~FootprintVariantsDialog() override;

	protected:
		// The pointer leaving the tree is what puts the selected row back in front — see the
		// header. Watched on the viewport, because that is what receives the leave.
		bool eventFilter(QObject* watched, QEvent* event) override;

	private slots:
		// Reads the database and rebuilds the tree. Cheap enough to run on every show: it is one
		// pass over the parts plus a file-row lookup each.
		void reload();
		void onCurrentItemChanged(QTreeWidgetItem* current);
		// Live hover, the reason the tree has mouse tracking on.
		void onItemEntered(QTreeWidgetItem* item, int column);
		void onModeChanged();

	private:
		// Loads a package's variants into the view and highlights `variantKey` (empty: the first).
		void showPackage(int groupIndex, const QString& variantKey);
		// Both halves of the highlight — the view, and the status line under it.
		void highlight(const QString& variantKey);
		// The colour a variant is drawn in, by its position in the group. Stable for a given
		// database, because the grouping is.
		static QColor colourForIndex(int index);

		DatabaseHandle* m_handle = nullptr;
		std::vector<FootprintPackageGroup> m_groups;
		// Which group the view currently holds, so a hover inside it only re-highlights rather
		// than re-reading every file from the store.
		int m_shownGroup = -1;
		QString m_selectedVariant;

		Ui::FootprintVariantsDialog* m_ui;
		// Built in code rather than promoted in the .ui, the same way the part editor's KiCad
		// previews are: promotion would put a project header into a Designer file for one widget.
		KicadVariantView* m_view = nullptr;
	};

}
