// @file PartManager_EcadDownloadToolset.h
// @brief The tools that find a vendor ECAD download and attach it to a part (§5c, §14f).
//
// The user's own sentence is the whole specification: *"migrate this part — I
// already downloaded the ECAD zip into my downloads folder"*. Before this, the
// assistant could do every step of that except the one the user had already
// done for it.
//
// **This is the one place §14f's "no tool takes a filesystem path" is relaxed,
// and it is relaxed narrowly.** The model still never supplies a path. It
// supplies a *file name that came back from `list_downloaded_libraries`*, and
// this toolset resolves that name against the folders it is allowed to look in:
//
//   - a name carrying a path separator, a drive letter or `..` is refused, by a
//     message that names the rule rather than by a silent empty result;
//   - the resolved canonical path is checked to still sit inside an allowed
//     root, so a symlink cannot walk out of one;
//   - only the roots in `EcadDownloadContext::allowedRoots` — the system
//     Downloads folder, plus `AppPreferences::llmDownloadFolder` when the user
//     has set one — and only one extension per tool (`.zip`, or `.pdf` for the
//     datasheet).
//
// **`set_part_datasheet` relaxes it once more, and only it:** a datasheet may
// also be given as an *absolute* path, because the user asked for exactly that
// ("a PDF path") and a datasheet is usually saved wherever they were looking
// rather than in Downloads. It is still `.pdf` only, the file must exist, and
// the refusal names which of those failed. Nothing else here accepts a path.
//
// What still holds, and is the reason the relaxation is acceptable: no tool here
// reads arbitrary file *content* back to the model, lists an arbitrary
// directory, or writes anything anywhere but the filestore. The reachable set is
// "an archive the user downloaded, or a PDF they point at", and the reachable
// operation is "classify it" or "attach it to a part I named".
//
// `attach_downloaded_library` goes through `PartEditorController::importEcadArchive()`
// — the same call the ECAD download dialog makes — rather than reimplementing the
// filestore half of §5c. The filename matching rule is `EcadArchive::matchesPartNumber()`
// for the same reason: the dialog's folder watch uses it too, and an assistant
// offering an archive the dialog would not have taken is a disagreement neither
// of them can explain.
//
// It is app-layer rather than `core/llm/` because it drives a controller that
// lives in the app, and because `QStandardPaths::DownloadLocation` is a fact
// about a desktop session rather than about a parts library.
// @see docs/design/ARCHITECTURE.md §5c, §14, §14a, §14f
// @see PartManager_EcadArchive.h, PartManager_PartEditorController.h, PartManager_LlmTool.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "llm/PartManager_LlmTool.h"
#include <QStringList>
#include <vector>

namespace PartManager
{

	class DatabaseHandle;
	class LlmUiBridge;

	// What the download tools may touch: one database, and a closed list of folders. Shaped like
	// `LlmToolContext` — the same `isUsable()`, the same `allowWrites`, nothing owned here — so a
	// handler that has learned one of these has learned both.
	struct EcadDownloadContext
	{
		// The database the part ids are checked against, and the one an archive is attached into.
		// Never owned here.
		DatabaseHandle* database = nullptr;

		// The window, when there is one. Used for exactly one thing: telling an open part editor
		// to re-read a part this toolset has just written. The editor is modeless, so it outlives
		// the call that opened it and its next §10 autosave would otherwise put its stale copy of
		// the row straight back over the change. Null is the normal state for a host with no
		// Component Browser, and means there is no editor that could be stale. Never owned here.
		LlmUiBridge* ui = nullptr;

		// False turns the attach tool into a refusal that says why, the way every other §14f
		// write does. The two read tools stay on: listing and inspecting change nothing.
		bool allowWrites = true;

		// The folders a file name may resolve inside, absolute and in priority order. **Empty is
		// a valid state and means the tools are offered nothing at all** — this list is the whole
		// of the filesystem this assistant can see, so it is built explicitly by the host rather
		// than defaulted inside a handler where nothing could review it.
		QStringList allowedRoots;

		// True when the database half is usable. The roots are checked separately: a missing
		// folder is a thing to report to the user, not a reason to leave the tools unregistered.
		bool isUsable() const;

		// The roots the app runs with: the system Downloads folder (falling back to the home
		// folder the way EcadFetchDialog does, because a stripped-down Windows profile can report
		// no Downloads location at all), plus `AppPreferences::llmDownloadFolder` when the user
		// has set one. Existing folders only — a configured path that is gone is dropped here
		// rather than turning every listing into an error about it.
		static QStringList defaultRoots();
	};

	class EcadDownloadToolset
	{
		EcadDownloadToolset() = delete;
	public:
		// Tool names, and what each answers with:
		//
		//   list_downloaded_libraries {modifiedWithinHours?, modifiedAfter?, nameContains?,
		//                              mpn?, maxResults?}
		//       -> {folders[], files[]{file, sizeBytes, modified}, matched, returned}
		//          Every filter optional; all of them omitted lists the newest archives in the
		//          folder, which is the common case ("I just downloaded it"). `mpn` runs
		//          EcadArchive::matchesPartNumber(), so `74HC4051PW-Q100,11` finds
		//          `LIB_74HC4051PW-Q100,11(5).zip`. **`modified` is ISO-8601, never "2 hours
		//          ago"**: the model has the CurrentDateTime built-in and can subtract, and a
		//          relative time computed for it is one more thing to get wrong.
		//          `file` is what the other two tools take — a bare name, never a path.
		//
		//   inspect_downloaded_library {file}
		//       -> {file, hasAnything, symbol{}, footprint{}, model3d{}, legacyKicadOnly,
		//           ignoredEntries}
		//          Read-only: EcadArchive::inspect() over the archive's entry list. Nothing is
		//          extracted, nothing is attached. This is what turns "is this the right zip?"
		//          into an answer rather than into a write the user has to undo.
		//
		//   attach_downloaded_library {partId, file}
		//       -> {partId, name, file, symbolAttached, footprintAttached, modelAttached,
		//           legacyKicadOnly, ignoredEntries}
		//          The write, through PartEditorController::importEcadArchive(). Each file
		//          replaces whatever held its slot, and the bytes are copied into the filestore,
		//          so the ZIP can be deleted afterwards. `legacyKicadOnly` is reported in words
		//          as well as in the flag: the archive *does* have KiCad support and PartManager
		//          still took nothing, which otherwise reads as a broken importer.
		//
		//   set_part_datasheet {partId, url? | file?}
		//       -> {partId, name, source, file, sizeBytes, readable, looksScanned, pageCount,
		//           editorReloaded}
		//          §3/§6's two existing paths — `PartEditorController::downloadDatasheet()` for a
		//          URL, `attachDatasheet()` for a local PDF — behind one tool, because "attach the
		//          datasheet" is one intent and which of the two it is, is a property of what the
		//          user has, not a decision the model should be asked to model. Exactly one of the
		//          two per call; both or neither is a refusal that says so. Either replaces an
		//          existing datasheet, detaching the old row first.
		//
		//          It reports whether the PDF's **text** came out (`PdfText`, §14g), because that
		//          is what decides whether search_datasheet can answer anything from it: a scan is
		//          stored perfectly and readable by nobody, and a model that finds out only two
		//          turns later answers from its own memory of the part instead.
		static std::vector<LlmTool> tools(const EcadDownloadContext& context);
	};

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
