// @file PartManager_LlmUiBridge.h
// @brief What the §14a assistant is allowed to see and drive of the Home tab (§7, §7a).
//
// The tools that answer "the part I have selected" need the category tree, the
// part table and §7a's four filter boxes. What they must *not* need is
// `MainWindow.h`: that header names the controllers, Qt3D, the partlist panel
// and a dozen dialogs, so a toolset including it would pull the whole app into
// its translation unit — and into the test binary, which compiles app sources
// one file at a time and would then have to link all of it.
//
// So the window hands out an interface and the toolset sees fifteen methods, an
// int and a QString. Nothing here names a QtLLM type either, which is why this
// header carries no `QTLLM_LIBRARY_AVAILABLE` guard: `MainWindow` derives from
// it whether or not the build has a model to talk to, and a guard would make
// the header say one thing to moc and another to the compiler.
//
// The interface is deliberately the *window's own* vocabulary rather than a
// prettier one invented for the model. Translating between them is the
// toolset's job, and a bridge that reshaped the answer would be a second place
// where "what is selected" is decided.
// @see docs/design/ARCHITECTURE.md §7, §7a, §14, §14a
// @see PartManager_UiToolset.h, PartManager_MainWindow.h
#pragma once

#include <QString>

namespace PartManager
{

	// What MainWindow::selectPart() had to do to put a part in front of the user. Selecting a
	// part is not one step: the table only ever holds one category (§7b), and the §7a table
	// filter can hide a row inside the right one. Both of those are the user's own view state,
	// so changing either is reported rather than done quietly — a caller that moved the
	// category and emptied a search box has to be able to say so.
	struct SelectPartResult
	{
		// False means the id names no part, and nothing was changed — not a half-moved view.
		bool found = false;
		// True when the part's row is the current one now. It can be false with `found` true:
		// the §7a tree filter can leave the part's category out of the tree entirely, and
		// there is then nothing to select without overriding a second filter the user set.
		bool selected = false;
		// The tree moved to the part's category.
		bool categoryChanged = false;
		// The table filter was hiding the part and had to be emptied to reveal it.
		bool filterCleared = false;
	};

	class LlmUiBridge
	{
	public:
		virtual ~LlmUiBridge() = default;

		// ---- what the Home tab is showing -------------------------------------------------

		// The open category, or NoParentType (0) when the tree has no selection.
		virtual int selectedCategoryId() const = 0;
		virtual QString selectedCategoryName() const = 0;
		// The selected row's part, 0 when none. `outName` takes its name along the way and is
		// required here rather than defaulted: a default argument on a virtual is bound to the
		// static type, so the two declarations could quietly disagree.
		virtual int selectedPartId(QString* outName) const = 0;

		// ---- aiming it somewhere else ------------------------------------------------------

		// Makes `typeId` the current category, expanding whatever it is buried under. False when
		// no tree item carries that id — the category does not exist, or a §7a filter is leaving
		// it out, which is a different problem from a bad id and has to stay distinguishable.
		virtual bool selectCategory(int typeId) = 0;
		// Selects the part, moving the category and clearing the table filter if that is what it
		// takes. See SelectPartResult for what it reports back and why it has to.
		virtual SelectPartResult selectPart(int partId) = 0;

		// ---- §7a's four filter controls ----------------------------------------------------
		// The setters go through the widgets rather than around them, so the debounce, the
		// syntax-error border and the AppPreferences write all still happen exactly as they do
		// when a person types.

		virtual QString treeFilter() const = 0;
		virtual void setTreeFilter(const QString& text) = 0;
		virtual QString tableFilter() const = 0;
		virtual void setTableFilter(const QString& text) = 0;
		virtual bool allCategoriesSearch() const = 0;
		virtual void setAllCategoriesSearch(bool enabled) = 0;
		virtual bool hideEmptyCategories() const = 0;
		virtual void setHideEmptyCategories(bool enabled) = 0;

		// ---- opening the editor -------------------------------------------------------------

		// The §10 part editor on `partId`, through PartEditorDialog::open() — modeless, so this
		// returns as soon as the window is up rather than blocking the chat that asked for it.
		// `raisedExisting` says the part was already open and that editor was raised instead of a
		// second one built; it is never null on the way in. False means the editor could not be
		// opened at all (no database), which is not the same as "already open".
		virtual bool openPartEditor(int partId, bool* raisedExisting) = 0;

		// ---- keeping an open editor honest --------------------------------------------------

		// **A part editor is modeless, so it outlives the call that opened it — and holds its own
		// in-memory copy of the part.** A tool that writes that part's row is therefore writing
		// behind an open editor's back, and the editor's next §10 autosave writes its stale copy
		// straight back over the change. A tool that has written a part calls this afterwards; it
		// reloads the open editor from the database, so the copy stops being stale.
		//
		// False means that part was not open, which is the normal case and not a failure.
		//
		// Not pure, unlike everything above it: an implementor with no part editor has nothing to
		// reload, and "nothing was open" is the honest answer rather than a stub. It is also the
		// only method here a *headless* host could sensibly be handed.
		virtual bool reloadOpenPartEditor(int /*partId*/) { return false; }
	};

}
