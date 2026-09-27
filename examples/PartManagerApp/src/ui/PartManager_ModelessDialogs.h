// @file PartManager_ModelessDialogs.h
// @brief The registry behind the app's modeless dialogs — one window per screen, and all of
//        them gone before the database is.
//
// Every dialog used to be exec()-ed, which blocked the one thing the app is for: a user with
// an import wizard open could not look up what they already owned (§7). They are modeless
// now, and that costs three things exec() gave away for free — this file is those three:
//   - a second open of the same screen has to raise the first, not stack a duplicate that
//     autosaves over it;
//   - the work a caller did after exec() returned becomes a QDialog::finished connection;
//   - a window still open when the window owning the database goes has to be deleted
//     *before* it, because Qt deletes children later than it destroys members.
//
// PartEditorDialog carries the same three rules privately: it is keyed per part rather than
// per screen, so it kept its own registry rather than bending this one around it.
// @see docs/design/ARCHITECTURE.md §7, §10
// @see PartManager_PartEditorDialog.h
#pragma once

#include <QString>

class QDialog;

namespace PartManager
{

	class ModelessDialogs
	{
	public:
		// One key per screen, named here rather than at the call sites so the two ways in to
		// the same window cannot disagree about the spelling — Orders is on the ribbon *and* on
		// the partlist panel, and the point of a key is that both find the one window.
		static constexpr const char* Orders = "orders";
		static constexpr const char* Tags = "tags";
		static constexpr const char* TypeTemplates = "type-templates";
		static constexpr const char* KicadLibrary = "kicad-library";
		static constexpr const char* Settings = "settings";
		static constexpr const char* NewPart = "new-part";
		static constexpr const char* MouserSearch = "mouser-search";
		static constexpr const char* ImportPartList = "import-part-list";
		static constexpr const char* ExportCategories = "export-categories";
		static constexpr const char* ImportCategories = "import-categories";
		static constexpr const char* ReconcileCategories = "reconcile-categories";
		static constexpr const char* PartlistImport = "partlist-import";

		// The dialog already open under `key`, raised and activated, or null when there is
		// none. Callers ask first and build nothing until this says there is nothing to raise,
		// so a screen whose constructor does real work does not do it twice.
		static QDialog* raise(const QString& key);
		// Registers `dialog` under `key`, shows it modeless and hands it to Qt
		// (WA_DeleteOnClose). The entry drops itself when the dialog is destroyed, whichever of
		// the ways that happens.
		static void show(const QString& key, QDialog* dialog);
		// Deletes every registered dialog now — not close(), which only schedules it. The one
		// caller is ~MainWindow, before the member holding the DatabaseHandle is destroyed.
		static void closeAll();
	};

}
