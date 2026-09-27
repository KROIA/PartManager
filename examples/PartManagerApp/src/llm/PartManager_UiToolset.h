// @file PartManager_UiToolset.h
// @brief The tools that let the assistant see and drive the Component Browser (§7, §14a).
//
// Every other toolset answers questions about the *database*. This one answers
// the question the user actually asks first — "read the datasheet of the
// selected component" — which no database tool can, because the selection is
// not in the database. It lives in the app rather than in `core/llm/` for the
// same reason: `core/` is widget-free (§12a) and a selection is a widget fact.
//
// It reaches the window through `LlmUiBridge`, never through `MainWindow`, so
// the app's whole dialog and Qt3D stack stays out of this translation unit.
//
// **Ids are still validated against the database, not against the window.** A
// bad id must come back naming what was wrong (§14c rule 2), and "the tree has
// no item with that id" cannot tell a category that does not exist from one a
// §7a filter is hiding. The handler asks the repositories first and the window
// second, so those two answers stay separate — the second one is actionable and
// the first one is not.
// @see docs/design/ARCHITECTURE.md §7, §7a, §14, §14a, §14c
// @see PartManager_LlmUiBridge.h, PartManager_LlmTool.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "llm/PartManager_LlmTool.h"
#include <vector>

namespace PartManager
{

	class DatabaseHandle;
	class LlmUiBridge;

	// What the UI tools are allowed to touch: the one window, and the database its ids are
	// checked against. Shaped like `LlmToolContext` — the same `isUsable()`, the same "never
	// owned here" — because a handler that has learned one of these has learned both.
	//
	// There is no `allowWrites` flag. Nothing here writes to the database: the worst a tool in
	// this set can do is move the user's view, which they can see and undo, and the one that
	// opens a window opens the editor they would have double-clicked to.
	struct UiToolContext
	{
		// The window the tools read and drive. Never owned here.
		LlmUiBridge* ui = nullptr;
		// The database every id is checked against before it reaches the window. The same handle
		// the other toolsets hold, and never owned here either.
		DatabaseHandle* database = nullptr;

		// True when both halves are there and the database is open. A null bridge is the normal
		// state for any host that is not the main window — the test binary, a background agent —
		// and not a programming error.
		bool isUsable() const;
	};

	class UiToolset
	{
		UiToolset() = delete;
	public:
		// Tool names, and what each answers with:
		//
		//   ui_get_state {}
		//       -> {category{selected,categoryId,name}, part{selected,partId,name,mpn},
		//           filters{tableFilter,treeFilter,allCategoriesSearch,hideEmptyCategories}}
		//          **The one the user's own example turns on** — "read the datasheet of the
		//          selected component" is this tool, then search_datasheet on the id it gives
		//          back. Nothing is selected reports `selected: false` with a null id and a note
		//          saying so, never `0`: a model handed a bare zero treats it as a part id and
		//          goes looking for part zero.
		//
		//   ui_select_category {categoryId}
		//       -> {categoryId, name}
		//          Opens that category in the tree. Refuses an unknown id while naming the ones
		//          that exist, and separately refuses a real category the §7a filters are
		//          hiding — saying which filter to clear, because that one is fixable.
		//
		//   ui_select_part {partId}
		//       -> {partId, name, categoryChanged, filterCleared, clearedTableFilter}
		//          Selecting a part is not one step (see SelectPartResult). Both of the things it
		//          may have to change are the user's own view state, so both come back: a model
		//          that emptied a search box has to be able to say "I cleared your search to show
		//          it", and `clearedTableFilter` carries the text so it can offer it back.
		//
		//   ui_set_filter {tableFilter?, treeFilter?, allCategoriesSearch?, hideEmptyCategories?}
		//       -> changed[]{control, from, to}
		//          §7a's four controls. Every parameter optional and an omitted one means
		//          *unchanged* — which is why an empty string has to be a value and not an
		//          absence: "" is how a filter gets cleared. All four omitted is a refusal, not a
		//          no-op, because it is the shape a model produces when it has misread the tool.
		//
		//   ui_open_part_editor {partId}
		//       -> {partId, name, raisedExisting}
		//          The §10 editor, modeless, through PartEditorDialog::open(). One editor per
		//          part: a part already open is raised and `raisedExisting` says so, so the model
		//          reports "it was already open" rather than claiming it opened a second one.
		//
		// None of these writes to the database, so none consults `allowWrites`. They run on the
		// GUI thread — a tool handler is invoked from the client's own signal, which arrives on
		// the thread the window lives on — which is the whole reason they may touch widgets.
		static std::vector<LlmTool> tools(const UiToolContext& context);
	};

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
