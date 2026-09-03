// @file PartManager_KicadLibraryDialog.h
// @brief The KiCad library screen (§5a) — regenerate, and resolve hand-edited artifacts.
//
// Two jobs on one screen: press Generate, and then deal with whatever came back
// as "edited in KiCad". That second list is the reason the screen exists at all
// — a generator with no way to see or resolve preserved edits would leave the
// user guessing why a symbol never updates.
//
// Each preserved row offers exactly the two choices §5a specifies:
//   - **Regenerate it** — discard the manual edit, take PartManager's version.
//   - **Keep my version** — re-baseline, so it stops being flagged without the
//     file changing at all.
// @see docs/design/ARCHITECTURE.md §5a
// @see PartManager_KicadController.h
#pragma once

#include "controllers/PartManager_KicadController.h"
#include <QDialog>
#include <vector>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace PartManager
{

	class KicadLibraryDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit KicadLibraryDialog(DatabaseHandle* handle, QWidget* parent = nullptr);

	private slots:
		void generate();
		// Discards the selected artifact's manual edit and writes PartManager's version.
		void regenerateSelected();
		// Accepts the selected artifact's on-disk version as the new baseline, unchanged.
		void keepSelected();
		void openFolder();
		// Writes the generated libraries into KiCad's own library tables — globally, or into a
		// single project's tables. See KicadLibTable for what "writes into" means.
		void install();
		void updateButtons();

	private:
		void showResult(const KicadGenerationResult& result);
		// Where the tables go: a KiCad config folder, or a project folder. Empty when the user
		// backed out or nothing suitable was found.
		QString chooseInstallTarget(bool& outIsGlobal);

		KicadController m_controller;
		QLabel* m_pathLabel;
		QLabel* m_librariesLabel;
		QPlainTextEdit* m_summary;
		QPushButton* m_installButton;
		QListWidget* m_preservedList;
		QPushButton* m_generateButton;
		QPushButton* m_regenerateButton;
		QPushButton* m_keepButton;
		std::vector<KicadSkippedItem> m_preserved;
	};

}
