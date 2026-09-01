// @file PartManager_StockDialog.h
// @brief The ribbon's Restock / Take Out prompt (§7 Home > Stock) — quantity plus an optional note.
//
// One dialog for both directions: only the wording, the sign of the preview and
// the window title differ, and a second class for that would be two files to
// keep in step. It collects input and nothing else — the caller writes the
// transaction through StockController, so no repository call lives in ui/.
//
// Taking out more than the shelf holds is allowed (§3): the preview simply shows
// the resulting negative quantity in red instead of blocking the button.
// @see docs/design/ARCHITECTURE.md §3, §7, §12b
// @see PartManager_StockController.h
#pragma once

#include <QDialog>
#include <string>

namespace Ui { class StockDialog; }

namespace PartManager
{

	class StockDialog : public QDialog
	{
		Q_OBJECT
	public:
		enum class Mode
		{
			Restock,   // +quantity
			TakeOut    // -quantity
		};

		StockDialog(const QString& partName, int currentQuantity, Mode mode, QWidget* parent = nullptr);
		~StockDialog() override;

		// What the user entered. Only meaningful after exec() returned Accepted.
		int quantity() const;
		// The user's own text — never tr()'d, it goes straight into the transaction's note.
		QString note() const;
		// The picked StockReason constant. Restocking has only one, so the row is hidden there.
		std::string reason() const;

	private:
		// Restates the quantity this will leave on the shelf, red when that is negative.
		void updatePreview();

		Ui::StockDialog* m_ui;
		Mode m_mode;
		int m_currentQuantity;
	};

}
