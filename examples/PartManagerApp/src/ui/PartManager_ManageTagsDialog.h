// @file PartManager_ManageTagsDialog.h
// @brief The §2d Manage Tags dialog — create/rename/recolour/delete the shared tag vocabulary.
//
// §2d ships no built-in tags, so a fresh database has an empty vocabulary and
// the part editor's tag row has nothing to offer until this dialog is used.
// Reachable from the Parts ribbon tab's *Manage* group.
//
// Every action writes through immediately (there is no OK/Cancel over the list),
// which matches §10: these are all already-existing records the moment they are
// created.
// @see docs/design/ARCHITECTURE.md §2d, §10
// @see PartManager_PartEditorController.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include <QDialog>

namespace Ui { class ManageTagsDialog; }

namespace PartManager
{

	class ManageTagsDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit ManageTagsDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~ManageTagsDialog() override;

	private slots:
		void onNewTag();
		void onRename();
		void onRecolour();
		void onDelete();

	private:
		// Re-reads the vocabulary into the list and re-enables/disables the per-tag buttons.
		void refreshList();
		// The selected tag, or a default-constructed one (id == NoTagId) when nothing is selected.
		Tag selectedTag() const;

		Ui::ManageTagsDialog* m_ui;
		PartEditorController m_controller;
		// The rows behind the list items — selectedTag() reads from here, not off the widget.
		std::vector<Tag> m_tags;
	};

}
