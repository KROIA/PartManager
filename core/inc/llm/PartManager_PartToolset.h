// @file PartManager_PartToolset.h
// @brief The database tools an LLM may call: categories, parts, attributes, tags (§14a).
//
// Everything here is a thin, *validating* wrapper over the repositories — it
// adds no persistence of its own and issues no SQL (§12a still holds: SQL lives
// in `persistence/`). What it does add is the part a model needs and a C++
// caller does not: an answer that says why, and an argument check strict enough
// that a wrong guess cannot land in the database.
//
// **Three rules were learned by measurement (2026-09-26) and are load-bearing:**
//
//  1. `create_category` is **idempotent on (name, parentId)**. Handed a
//     non-idempotent create, a local model that had just created "Varistors"
//     created it again, and again — eight calls before the cap stopped it. An
//     idempotent create ends that loop on the second call instead.
//  2. Ids are never invented. `create_part` takes a `categoryId` that must come
//     from `list_categories`/`create_category`, and refuses one that does not
//     exist, naming the categories that do. A category *name* is not accepted:
//     two branches may legitimately carry the same leaf name (§2b).
//  3. Enum parameters are checked here too, not only by
//     `Client::setValidateToolInput()`. The same model answered a `glyph`
//     enum with an emoji; the validator is what turns that into a correction
//     instead of a bad row.
//
// @see docs/design/ARCHITECTURE.md §2, §2a, §2b, §2d, §14
// @see PartManager_LlmTool.h, PartManager_PartTypeRepository.h, PartManager_PartRepository.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "llm/PartManager_LlmTool.h"
#include <vector>

namespace PartManager
{

	class PART_MANAGER_API PartToolset
	{
		PartToolset() = delete;
	public:
		// Every database tool, in the order a migration naturally walks them.
		// The context is captured by value into each handler, so the returned
		// vector stays valid as long as `context.database` does.
		//
		// Tool names, and what each answers with:
		//
		//   list_categories   {}                            -> categories[]{id,name,parentId,domain,partCount}
		//   get_category      {categoryId}                  -> name, parentId, domain, kicadCategory,
		//                                                       nameTemplate, attributes[]{key,label,unit,
		//                                                       datatype,required,searchable,enumOptions},
		//                                                       fileSlots[]{role,label,required}
		//   create_category   {name, parentId?, domain?,     -> {id, created}  — created=false when it
		//                      description?}                    already existed (see rule 1 above)
		//   add_category_attribute {categoryId, key, label,  -> {id}
		//                      datatype, unit?, required?,
		//                      searchable?, enumOptions?}
		//   search_parts      {query, categoryId?, limit?}   -> parts[]{id,name,mpn,manufacturer,
		//                                                       categoryId,categoryName,stockQty}
		//   get_part          {partId}                       -> the part, its resolved attributes,
		//                                                       its tags and its attached files
		//   create_part       {categoryId, name, mpn?,       -> {id}
		//                      manufacturer?, description?,
		//                      package?, stockQty?}
		//   update_part       {partId, name?, mpn?,          -> {id}  — only the keys present are written
		//                      manufacturer?, description?,
		//                      package?, stockMinQty?}
		//   set_part_attribute {partId, key, value, unit?}   -> {key, storedValue, storedUnit}
		//                                                       value goes through ValueParser, so "4k7"
		//                                                       is stored as 4700 — never the typed text
		//   list_tags         {}                             -> tags[]{id,name,category}
		//   set_part_tags     {partId, tags[]}               -> {applied[], unknown[]}
		//
		// Read-only tools work with `context.allowWrites == false`; every other
		// one answers llmError() and writes nothing.
		static std::vector<LlmTool> tools(const LlmToolContext& context);

		// The subset a component migration needs, by name. Registering the whole
		// set and disabling the rest beats registering less: a small model does
		// measurably worse the longer the tool list gets, and this keeps the
		// choice of subset in one named place instead of in every caller.
		static std::vector<QString> migrationToolNames();
	};

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
