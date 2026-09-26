// @file PartManager_KicadToolset.h
// @brief The KiCad tools an LLM may call: describe a part's library files, reuse one, regenerate (§14a, §5a).
//
// Two of the six things the assistant was asked for live here — answering
// questions about a part's KiCad library, and "changing the footprint or
// changing the symbol". The second turns out not to need a geometry editor:
// what makes a part's footprint wrong is almost always that it carries the
// wrong *file*, and the fix is to give it the one another part already has.
// That is `set_part_kicad_file`, and it is the same single-slot replace the
// part editor performs (`FileStore::replaceRoleFile`), not a second path.
//
// **No tool here takes a filesystem path.** §14f's rule is that the assistant
// reaches parts and nothing else; a tool that attached an arbitrary path would
// quietly reintroduce the file access that was deliberately left unregistered.
// A file therefore only ever arrives from another part in the same database.
// Importing a vendor archive stays a human action — the app already does it.
//
// **The describe tools report geometry, not files.** `KicadGeometry` already
// parses both formats for the preview panel, so a question like "does this
// footprint have a ground pad" is answered from the parsed shapes rather than by
// handing the model several kilobytes of s-expression to read for itself. That
// is cheaper, and it is the difference between an answer and a guess.
//
// `find_parts_sharing_a_footprint` is here because it is the question the second
// feature request is really made of: before a footprint can be shared between a
// resistor and a capacitor of the same size, something has to be able to say
// which parts already carry the same pad layout.
// @see docs/design/ARCHITECTURE.md §5a, §14, §14f
// @see PartManager_KicadGeometry.h, PartManager_KicadLibraryGenerator.h, PartManager_LlmTool.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "llm/PartManager_LlmTool.h"
#include <vector>

namespace PartManager
{

	class PART_MANAGER_API KicadToolset
	{
		KicadToolset() = delete;
	public:
		// Tool names, and what each answers with:
		//
		//   list_part_kicad_files {partId}
		//       -> {symbol{name,attached}, footprint{...}, model3d{...}, datasheet{...},
		//           kicadCategory, kicadRelevant}
		//          What a part actually carries, which is the first thing every other
		//          question here depends on. "Not attached" is a normal answer, not an error:
		//          most parts are in that state and §5a generates a symbol for them anyway.
		//
		//   describe_kicad_footprint {partId}
		//       -> {padCount, padNumbers[], throughHole, layers[], widthMm, heightMm,
		//           courtyardMm{w,h}, model3d{present,offsetMm,rotationDeg,scale}}
		//          Parsed through KicadGeometry, so the numbers are the ones the preview draws.
		//          Note the traps its header records and do not re-derive them: a footprint file
		//          measures Y downward, `(offset (xyz ...))` is millimetres while the legacy
		//          `(at (xyz ...))` is *inches*, and `(rotate ...)` is stored negated.
		//
		//   describe_kicad_symbol {partId}
		//       -> {pinCount, pins[]{number,name,type}, widthMm, heightMm, derivedFromFootprint}
		//          `derivedFromFootprint` is true when the part has no symbol of its own and the
		//          pins were read off its pads (§5a's M5 path) — the model must be able to tell
		//          a real symbol from a generated stand-in, because only one of them is worth
		//          answering questions about.
		//
		//   find_parts_sharing_a_footprint {partId, tolerantMm?}
		//       -> matches[]{partId, name, mpn, footprintName, identical, padCountMatches,
		//                    outlineDeltaMm}
		//          Parts whose footprint has the same pad count, the same pad numbers and an
		//          outline within `tolerantMm` (default 0.05). `identical` means the stored files
		//          are byte-for-byte the same, which is a different and much stronger claim than
		//          "compatible" — report both and never conflate them. This tool answers, it does
		//          not merge: deciding that two footprints *may* be shared is the open design
		//          question in `.claude/FeatureRequests.md` request 2, not something to settle here.
		//
		//   set_part_kicad_file {partId, role, sourcePartId}
		//       -> {role, fileName, replacedExisting}
		//          Gives `partId` the symbol / footprint / 3D model that `sourcePartId` already
		//          carries. `role` is an enum: `symbol` | `footprint` | `model3d`. Refuses a
		//          source that carries nothing in that role, and refuses to copy a part's file
		//          onto itself. Goes through FileStore's single-slot replace, so the previous row
		//          is read *before* the new one is inserted — asking afterwards returns the row
		//          just written and the old one is never detached.
		//
		//   generate_kicad_libraries {}
		//       -> {written[], skipped[]{part,reason}, preserved[], summary}
		//          §5a's generator, unchanged. A hand-edited artifact is preserved and reported,
		//          never overwritten.
		//
		// `set_part_kicad_file` and `generate_kicad_libraries` are writes and obey
		// `LlmToolContext::allowWrites`; the four describe/find tools do not.
		static std::vector<LlmTool> tools(const LlmToolContext& context);
	};

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
