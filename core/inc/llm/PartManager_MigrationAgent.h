// @file PartManager_MigrationAgent.h
// @brief The headless agent that turns a part number into a row in the database (§14e).
//
// A `QtLLM::Agent` — a conversation the user never sees — wired to the Mouser
// and database toolsets and pointed at one job: look a part number up, decide
// which category it belongs in, and create it. This is the first thing the LLM
// integration is asked to do and the one the unit tests drive, so it is a named
// class rather than a prompt assembled at the call site.
//
// **What was measured before this was designed (2026-09-26, local Ollama):**
//
//  - `llama3.2:3b` is not usable here. Given the part number it called
//    `create_part` first, with empty strings for every field, and then printed
//    an invented result. It answers a single trivial tool call correctly, which
//    is what makes this failure easy to miss.
//  - `qwen2.5-coder:14b` does not emit tool calls at all through Ollama's
//    `/api/chat` — it prints the call as JSON in the message body, which arrives
//    as ordinary assistant text and looks like the model refusing to use tools.
//  - `gpt-oss:20b` runs the whole search -> list -> create loop correctly in
//    ~100 s and six calls, *including recovering from a rejected enum value on
//    its own*, and is the default below for that reason.
//  - `qwen3:8b` reaches the same end state in five calls but takes ~690 s — it
//    reasons at length before every call. It is kept as the fallback because it
//    is correct, not because it is usable when speed matters.
//
// Because the default is a local model and local models drift, `Config::model`
// is a preference and not a promise: `fallbackModels` is walked when the server
// does not offer it, and `Result::model` records what actually ran. A test that
// asserts on behaviour should read that field before blaming the code.
//
// The agent is capped three ways — `maxToolCalls` per turn, a wall-clock
// timeout, and the tool list trimmed to `PartToolset::migrationToolNames()` plus
// the Mouser tools. All three exist because the failure mode of an unattended
// agent is a loop, not a wrong answer.
// @see docs/design/ARCHITECTURE.md §14, §14e, §6
// @see PartManager_PartToolset.h, PartManager_MouserToolset.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "llm/PartManager_LlmTool.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

namespace PartManager
{

	class PART_MANAGER_API MigrationAgent : public QObject
	{
		Q_OBJECT
	public:
		struct Config
		{
			// Measured good on this machine; see the header note before changing it.
			QString model = QStringLiteral("gpt-oss:20b");
			// Walked in order when the server does not offer `model`. Deliberately
			// does not list llama3.2 — it is offered by almost every Ollama install
			// and would be picked as a fallback that then fails silently.
			QStringList fallbackModels = { QStringLiteral("qwen3:8b") };
			QString ollamaUrl = QStringLiteral("http://localhost:11434/api/chat");
			// Per turn. Eight is two more than the longest correct run observed
			// (search, suggest, list, create category, create part, done).
			int maxToolCalls = 12;
			// Wall clock for the whole migration. A local 20B model on CPU spends
			// tens of seconds per turn, so this is minutes, not seconds.
			int timeoutMs = 300000;
			// Attach the datasheet and the product photo. False keeps the run off
			// Mouser's CDN without changing any other step.
			bool downloadFiles = true;
		};

		struct Result
		{
			bool ok = false;
			int partId = 0;             // 0 when nothing was created
			int categoryId = 0;
			QString categoryName;
			bool categoryWasCreated = false;
			QString message;            // the model's closing text, or the reason it failed
			QString model;              // what actually ran — see the header note
			QStringList toolCalls;      // tool names in call order, for the tests and the log
			int inputTokens = 0;
			int outputTokens = 0;
			qint64 durationMs = 0;
		};

		using Done = std::function<void(const Result&)>;

		MigrationAgent(const LlmToolContext& context, const Config& config, QObject* parent = nullptr);
		~MigrationAgent() override;

		// `partNumber` is a Mouser article number, a manufacturer part number or a
		// pasted product-page URL — the three shapes the user's own stock list is
		// in. The URL is reduced to its part number before the agent ever sees it
		// (`MouserSearchService::partNumberFromUrl`), because that is string work
		// and a token spent on it is a token wasted.
		//
		// `done` runs on the GUI thread. Exactly once: a timeout, a refusal and a
		// success all arrive through it, so a caller never has to also watch for
		// a signal.
		void migrate(const QString& partNumber, Done done);

		// Blocking form for tests and console tools: spins a local event loop
		// until `migrate()` resolves. GUI thread only, and never from inside a
		// tool handler — that is already inside one event loop.
		Result migrateBlocking(const QString& partNumber);

		// The system prompt the agent runs with. Public because it is the part
		// most likely to need tuning against a new model, and a test that pins
		// behaviour should be able to say which prompt it pinned.
		static QString systemPrompt();

	signals:
		// One per tool call, for the chat's status line and the test log.
		void progress(const QString& toolName);

	private:
		struct Impl;
		Impl* m_impl;
	};

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
