// @file PartManager_LlmTool.h
// @brief One LLM-callable tool — its schema and the C++ handler behind it (§14).
//
// A toolset hands back a vector of these instead of registering straight onto a
// `QtLLM::Client`, for the same reason `StepConverter` names a path instead of
// running the subprocess (§13): the handler is then a plain function of a
// `QJsonObject` and an open database, so a unit test drives it with no model, no
// network and no event loop. `registerOn()` is the small loop that turns a
// toolset into a live client, and it is the only part that needs a client at all.
//
// **Every handler must be total.** A tool result is fed straight back to the
// model, so a handler that throws, or that answers a bad argument with silence,
// costs a whole turn and usually a retry loop. Answer with `llmError()` and say
// what was wrong — a model corrects itself from an actionable message and cannot
// from a missing one.
// @see docs/design/ARCHITECTURE.md §14, §12a
// @see PartManager_PartToolset.h, PartManager_MouserToolset.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include <QJsonObject>
#include <QString>
#include <Tool.h>
#include <ProtocolBase.h>
#include <string>
#include <vector>

namespace QtLLM { class Client; }

namespace PartManager
{

	class DatabaseHandle;

	// What the database-facing tools are allowed to touch. Deliberately one open
	// handle rather than a connection plus a pile of paths: the handle already
	// owns the §1 folder layout, and a toolset that only got a connection could
	// not attach a file.
	struct PART_MANAGER_API LlmToolContext
	{
		// The open database every tool reads and writes. Never owned here.
		DatabaseHandle* database = nullptr;

		// False turns the whole toolset read-only: every mutating handler answers
		// `llmError("this assistant may not change the database")` without
		// touching anything. This is what a "just answer questions about my
		// library" agent runs with, and it is enforced in the handlers rather
		// than by leaving tools unregistered, so the model is told *why* rather
		// than left to guess at a missing capability.
		bool allowWrites = true;

		// True when the context is usable at all — a null or closed handle is the
		// normal state between databases (§1b), not a programming error.
		bool isUsable() const;
	};

	// A tool the model may call: the schema it sees and the code that runs.
	struct PART_MANAGER_API LlmTool
	{
		QtLLM::Tool schema;
		QtLLM::ToolHandler handler;
	};

	// Free helpers shared by every toolset. These are `QtLLM::toolOk`/`toolError`
	// with one addition: the project's own convention that an error names the
	// thing it could not find, because "no such category" and "no such category:
	// Varistor (have: Resistor, Capacitor, ...)" are one retry apart.
	PART_MANAGER_API QJsonObject llmOk(const QJsonObject& payload = {});
	PART_MANAGER_API QJsonObject llmError(const QString& message, const QJsonObject& extra = {});

	// The tool named `name`, or nullptr. Exists for tests, which address a tool
	// by its model-facing name rather than by position in the vector.
	PART_MANAGER_API const LlmTool* findLlmTool(const std::vector<LlmTool>& tools, const QString& name);

	// Registers every tool in `tools` on `client`. Separate from the toolset
	// builders so the builders stay client-free and testable.
	PART_MANAGER_API void registerLlmTools(QtLLM::Client& client, const std::vector<LlmTool>& tools);

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
