// @file PartManager_MouserToolset.h
// @brief The Mouser tools an LLM may call: look a part up, and import the one it picked (§14a, §6).
//
// `mouser_search` is the Search API through `MouserClient`, ranked by
// `MouserSearchService::rankByMatch()` and trimmed to the fields a model can
// actually act on. The full DTO is ~40 fields; handing all of them over costs
// tokens on every candidate and buys nothing, so a result row carries the
// article number, the MPN, the manufacturer, the description, Mouser's own
// category and the package — the six a categorisation decision is made from.
//
// **`mouser_import_part` is one tool, not a script the model writes.** It runs
// the same path the New Part dialog does — prefill, type match, insert, then the
// datasheet and product photo downloaded into the filestore — because that path
// already knows the things a model does not: that `Price` carries its own
// currency, that the image URL lies about its extension, that a 200 can still be
// a block page, and that a Mouser article number belongs in `part_seller_link`
// and never in `part.mpn`. A model asked to assemble that from primitives gets
// it wrong in a way nobody sees until an order is staged.
//
// The key is `MOUSER_SEARCH_API` from the environment and nowhere else, exactly
// as `MouserClient` already requires — no tool takes a key as a parameter, so a
// model can neither read one nor be tricked into echoing one into the chat.
// @see docs/design/ARCHITECTURE.md §6, §14
// @see PartManager_MouserClient.h, PartManager_MouserSearchService.h, PartManager_LlmTool.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "llm/PartManager_LlmTool.h"
#include <vector>

namespace PartManager
{

	class PART_MANAGER_API MouserToolset
	{
		MouserToolset() = delete;
	public:
		// Tool names, and what each answers with:
		//
		//   mouser_search  {query, limit?}   -> results[]{mouserPartNumber, mpn, manufacturer,
		//                                        description, mouserCategory, package,
		//                                        hasDatasheet, hasImage}
		//                                      Tries the part-number endpoint first and falls back
		//                                      to keyword, the same order MouserSearchDialog uses —
		//                                      asking the model to choose an endpoint asks it to
		//                                      know the answer before it searches.
		//
		//   mouser_suggest_category {mouserPartNumber}
		//                                    -> {suggestedCategoryId, suggestedCategoryName,
		//                                        mouserCategory, confident}
		//                                      Runs PartTypeMatcher against the *user's own* type
		//                                      list. `confident` is false when nothing matched
		//                                      well, which is the model's cue to create a category
		//                                      rather than to force the part into a near miss.
		//
		//   mouser_import_part {mouserPartNumber, categoryId, downloadFiles?}
		//                                    -> {partId, categoryId, attributesWritten[],
		//                                        unmappedAttributes[], datasheetAttached,
		//                                        imageAttached, sellerLinkWritten}
		//                                      Refuses a categoryId that does not exist, and an
		//                                      mpn already in the database (answering with the
		//                                      existing partId) — re-running a migration must not
		//                                      fork the part.
		//
		// `downloadFiles` defaults to true. A test that does not want the network
		// past the search call passes false; everything else is identical, so the
		// tested path is the shipped path.
		static std::vector<LlmTool> tools(const LlmToolContext& context);
	};

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
