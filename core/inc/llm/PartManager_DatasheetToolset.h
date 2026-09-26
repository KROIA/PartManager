// @file PartManager_DatasheetToolset.h
// @brief The datasheet tools an LLM may call: find the passage, then read it (§14a, §14g).
//
// The feature request asks for "reading datasheets if requested to answer
// questions". The shape that request needs is **not** a tool that returns a
// datasheet — a real one runs to tens of pages, and a local model's context
// window does not. It is a tool that finds the paragraph.
//
// So `search_datasheet` is the primary tool and `read_datasheet` is the fallback
// for when the model already knows which page it wants. Both go through
// `PdfText`, which extracts text and nothing else: no column reconstruction, no
// tables. A pinout printed as a grid comes back as interleaved words, which is
// why every answer carries its page number — the honest contract is "here is
// where it says that", not "here is the table".
//
// **A scanned datasheet is a normal answer, not an error.** Roughly one in five
// of the user's own datasheets is a picture of text (measured: 22 of 27 are
// extractable). Those come back `ok` with `scanned: true` and no text, so the
// model can say "this one is a scan, I cannot read it" instead of inventing a
// specification — which is the single worst thing this feature could do.
// @see docs/design/ARCHITECTURE.md §14, §14g
// @see PartManager_PdfText.h, PartManager_LlmTool.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "llm/PartManager_LlmTool.h"
#include <vector>

namespace PartManager
{

	class PART_MANAGER_API DatasheetToolset
	{
		DatasheetToolset() = delete;
	public:
		// Tool names, and what each answers with:
		//
		//   list_datasheets {categoryId?}
		//       -> parts[]{partId, name, mpn, hasDatasheet, fileName, pageCount?}
		//          Which parts have one at all. `hasDatasheet:false` is the common case and
		//          not an error — `DataSheetUrl` is empty for most Mouser parts (§6).
		//
		//   search_datasheet {partId, query, maxMatches?}
		//       -> {scanned, pageCount, matches[]{page, snippet}}
		//          **The primary tool.** Case-insensitive, a few hundred characters of context
		//          per hit, at most `maxMatches` (default 5) hits. No match is `matches: []`
		//          with `status: ok` — "the datasheet does not say" is an answer.
		//
		//   read_datasheet {partId, fromPage?, toPage?, maxChars?}
		//       -> {scanned, pageCount, fromPage, toPage, truncated, text}
		//          For when the model already knows the page. Capped by `maxChars` (default a
		//          few thousand) and it says `truncated` rather than silently cutting — a model
		//          that does not know it was cut off will answer from half a sentence.
		//
		// All three are reads, so none of them consults `allowWrites`. None takes
		// a filesystem path: a datasheet is reached through its part, like every
		// other file in this toolset family (§14f).
		static std::vector<LlmTool> tools(const LlmToolContext& context);
	};

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
