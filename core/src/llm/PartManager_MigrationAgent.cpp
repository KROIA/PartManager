#include "llm/PartManager_MigrationAgent.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"
#include "llm/PartManager_MouserToolset.h"
#include "llm/PartManager_PartToolset.h"
#include "mouser/PartManager_MouserSearchService.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "persistence/PartManager_PartRepository.h"
	#include "persistence/PartManager_PartTypeRepository.h"
	#include "SQLite.h"
#endif

#include <Agent.h>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>
#include <algorithm>

namespace PartManager
{
	namespace
	{
		// What the agent is allowed to spend a turn on. Ollama is free, so the cap exists only as
		// the shape every other agent carries.
		constexpr double NoCostCap = 0.0;

		// Per response. QtLLM defaults to 1024, which is enough for the answer and not always
		// enough for a reasoning model's tool call: gpt-oss:20b thinks in the same budget it
		// writes in, and a truncated call arrives as ordinary text that looks like a refusal.
		constexpr int MaxTokensPerTurn = 4096;

		bool resultIsOk(const QJsonObject& result)
		{
			return result.value(QStringLiteral("status")).toString() == QStringLiteral("ok");
		}
	}

	// The whole implementation, including finishing a run, lives here rather than as private
	// methods on MigrationAgent: the header is the contract, and a private helper on it would be
	// a second place to read before knowing what the class offers.
	struct MigrationAgent::Impl
	{
		LlmToolContext context;
		Config config;
		QtLLM::Agent* agent = nullptr;

		// The tools stay alive for the agent's whole life: registerTool() copies the schema but
		// the handler is a std::function holding the captured context, and the vector owns it.
		std::vector<LlmTool> tools;

		// Per-migration state. Reset by migrate(), read by finish().
		Done done;
		Result result;
		QElapsedTimer clock;
		QTimer* deadline = nullptr;
		bool running = false;

		// Resolves one migration. Runs exactly once per migrate(): a timeout landing in the same
		// event-loop pass as the answer, and an answer arriving after a timeout, both come
		// through here and the second one is dropped. A caller that also had to guard against a
		// second callback would be guarding against this function.
		void finish(bool modelOk, const QString& text)
		{
			if (!running)
			{
				return;
			}
			running = false;
			if (deadline)
			{
				deadline->stop();
				deadline->deleteLater();
				deadline = nullptr;
			}

			result.durationMs = clock.elapsed();
			result.model = agent->model();
			result.message = text;
			// The part row decides, not the sentence the model finished with: a model that says
			// it created a part it did not is the failure this class is capped against.
			result.ok = modelOk && result.partId != 0;

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
			// Read the category back off the part rather than trusting the tool answer that named
			// it: between the import and here the model may have moved the part with update_part.
			if (result.partId != 0 && context.isUsable())
			{
				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (PartRepository::findPart(db, result.partId, part))
				{
					result.categoryId = part.partTypeId;
					PartType type;
					if (PartTypeRepository::findType(db, part.partTypeId, type))
					{
						result.categoryName = QString::fromStdString(type.name);
					}
				}
				else
				{
					// The id came out of a tool answer, so a part that is not there means the row
					// was rolled back or deleted afterwards. Saying so beats reporting a part id
					// that nothing resolves.
					result.partId = 0;
					result.ok = false;
				}
			}
#endif

			if (modelOk && result.partId == 0)
			{
				result.message =
					QStringLiteral("the model finished without creating a part: %1").arg(text);
			}

			// Handed over one event-loop pass later, and for one measured reason: Ollama's
			// protocol emits `responseReady` and only *then* `statsReady`, so this function runs
			// inside the former and a Result assembled here reports 0 tokens on a turn that
			// plainly used some. Everything else is already decided above; the wait is only for
			// the counts. The agent is the context object, so a MigrationAgent deleted in the
			// meantime drops the callback instead of reaching through a dangling Impl.
			QTimer::singleShot(0, agent, [this]()
				{
					const QtLLM::UsageStats stats = agent->usageStats();
					result.inputTokens = std::max(result.inputTokens, stats.inputTokens);
					result.outputTokens = std::max(result.outputTokens, stats.outputTokens);

					// Taken by value and cleared first, so a callback that starts the next
					// migration finds the agent idle rather than re-entering this one.
					const Done callback = done;
					done = nullptr;
					if (callback)
					{
						callback(result);
					}
				});
		}
	};

	QString MigrationAgent::systemPrompt()
	{
		// Written against gpt-oss:20b on 2026-09-26 and deliberately procedural. A local model
		// asked to "add this part to the database" reaches for create_part first and fills it with
		// whatever it already believes; naming the order, and naming the tool that must not be
		// used, is what turns that into the measured five-to-six call loop.
		return QStringLiteral(
			"You are PartManager's part-migration agent. You are given one part number and you "
			"put that part into the user's parts database. You work through tools only and you "
			"never invent a value you were not told.\n"
			"\n"
			"Do this, in this order:\n"
			"1. Call mouser_search with the part number. Take the mouserPartNumber of the first "
			"result and use that exact string in every later call.\n"
			"2. Call mouser_suggest_category with that mouserPartNumber.\n"
			"3. If it answers confident=true, use its suggestedCategoryId. If it answers "
			"confident=false, call list_categories, and pick the category that really fits. If "
			"none fits, call create_category to make one and use the id it returns.\n"
			"4. Call mouser_import_part with that mouserPartNumber and that categoryId.\n"
			"5. When mouser_import_part answers status=ok, you are finished. Reply with one "
			"sentence naming the part and the category it went into, and call no further tools.\n"
			"\n"
			"Rules:\n"
			"- Every id you pass came out of a tool answer. Never make one up and never pass a "
			"category name where an id is asked for.\n"
			"- Do not call create_part or set_part_attribute for a Mouser part. "
			"mouser_import_part already writes the record, the attributes, the Mouser article "
			"number and the datasheet, and it writes them in places create_part cannot reach.\n"
			"- A tool answer with status=error says what was wrong. Fix that one argument and "
			"call the tool again. Never report a success a tool did not give you.");
	}

	MigrationAgent::MigrationAgent(const LlmToolContext& context, const Config& config,
		QObject* parent)
		: QObject(parent)
		, m_impl(new Impl())
	{
		m_impl->context = context;
		m_impl->config = config;

		QtLLM::AgentConfig agentConfig;
		agentConfig.name = QStringLiteral("Part migration");
		agentConfig.provider = QtLLM::Provider::Ollama;
		agentConfig.url = config.ollamaUrl;
		agentConfig.model = config.model;
		agentConfig.fallbackModels = config.fallbackModels;
		agentConfig.systemPrompt = systemPrompt();
		agentConfig.maxTokens = MaxTokensPerTurn;
		agentConfig.costCapUsd = NoCostCap;

		m_impl->agent = new QtLLM::Agent(agentConfig, this);

		// Both of these are off by default in QtLLM and both are load-bearing here. Validation
		// turns a mistyped argument into a correction the model reads before the handler runs;
		// the per-turn cap is the only thing that ends a loop, which is the failure mode of an
		// unattended agent (§14e, §14f).
		m_impl->agent->setValidateToolInput(true);
		m_impl->agent->setMaxToolCallsPerTurn(config.maxToolCalls);

		// The database tools named by migrationToolNames(), plus the three Mouser ones. Measured
		// 2026-09-26: fourteen tools ran the loop in five calls where four tools took six, so the
		// list is not trimmed for length (§14c).
		const std::vector<QString> wanted = PartToolset::migrationToolNames();
		for (const LlmTool& tool : PartToolset::tools(context))
		{
			if (std::find(wanted.begin(), wanted.end(), tool.schema.name()) != wanted.end())
			{
				m_impl->tools.push_back(tool);
			}
		}
		for (const LlmTool& tool : MouserToolset::tools(context))
		{
			m_impl->tools.push_back(tool);
		}
		registerLlmTools(*m_impl->agent, m_impl->tools);

		connect(m_impl->agent, &QtLLM::Client::toolInvoked, this,
			[this](const QString& toolName, const QJsonObject&)
			{
				m_impl->result.toolCalls.append(toolName);
				emit progress(toolName);
			});

		// The database row is the truth about what happened, not the model's closing sentence —
		// so the outcome is read off the tool results as they come back rather than parsed out of
		// prose afterwards.
		connect(m_impl->agent, &QtLLM::Client::toolCompleted, this,
			[this](const QString& toolName, const QJsonObject& result)
			{
				if (!resultIsOk(result))
				{
					return;
				}
				if (toolName == QStringLiteral("create_category"))
				{
					m_impl->result.categoryWasCreated =
						m_impl->result.categoryWasCreated
						|| result.value(QStringLiteral("created")).toBool();
				}
				else if (toolName == QStringLiteral("mouser_import_part")
					|| toolName == QStringLiteral("create_part"))
				{
					const QString idKey = toolName == QStringLiteral("create_part")
						? QStringLiteral("id") : QStringLiteral("partId");
					m_impl->result.partId = result.value(idKey).toInt();
					m_impl->result.categoryId = result.value(QStringLiteral("categoryId")).toInt();
					m_impl->result.categoryName =
						result.value(QStringLiteral("categoryName")).toString();
				}
			});

		connect(m_impl->agent, &QtLLM::Client::statsUpdated, this,
			[this](const QtLLM::UsageStats& stats)
			{
				m_impl->result.inputTokens = stats.inputTokens;
				m_impl->result.outputTokens = stats.outputTokens;
			});
	}

	MigrationAgent::~MigrationAgent()
	{
		// Deleting the agent tears its network stack down with it and fires no pending callback,
		// so a migration in flight simply stops. `done` is not called: the object that would have
		// received it is going away too.
		delete m_impl;
	}

	void MigrationAgent::migrate(const QString& partNumber, Done done)
	{
		if (m_impl->running)
		{
			Result busy;
			busy.message = QStringLiteral("a migration is already running");
			if (done)
			{
				done(busy);
			}
			return;
		}

		const QString trimmed = partNumber.trimmed();
		if (trimmed.isEmpty())
		{
			Result empty;
			empty.message = QStringLiteral("no part number to migrate");
			if (done)
			{
				done(empty);
			}
			return;
		}
		if (!m_impl->context.isUsable())
		{
			Result closed;
			closed.message = QStringLiteral("no database is open");
			if (done)
			{
				done(closed);
			}
			return;
		}

		m_impl->done = done;
		m_impl->result = Result();
		m_impl->running = true;
		m_impl->clock.start();

		// Each migration is its own conversation. Without this the second one starts with the
		// first one's tool results still in context, and a model that has just seen a successful
		// import answers the next part number by pointing at it.
		m_impl->agent->clearConversation();

		// The URL shape the user's own stock list is in is reduced to a part number here, not by
		// the model: it is string work, and a token spent on it is a token wasted.
		const std::string fromUrl = MouserSearchService::partNumberFromUrl(trimmed.toStdString());
		const QString query = fromUrl.empty() ? trimmed : QString::fromStdString(fromUrl);

		// The wall clock, running even while the model is thinking and no tool has been called.
		// A local 20B model on CPU spends tens of seconds per turn, so a migration that has gone
		// wrong looks exactly like one that is simply slow until this fires.
		m_impl->deadline = new QTimer(this);
		m_impl->deadline->setSingleShot(true);
		connect(m_impl->deadline, &QTimer::timeout, this, [this]()
			{
				m_impl->finish(false, QStringLiteral("timed out after %1 s")
					.arg(m_impl->config.timeoutMs / 1000));
			});
		m_impl->deadline->start(m_impl->config.timeoutMs);

		m_impl->agent->ask(QStringLiteral("Put this part into the database: %1").arg(query),
			[this](const QString& text, bool ok)
			{
				m_impl->finish(ok, text);
			});
	}

	MigrationAgent::Result MigrationAgent::migrateBlocking(const QString& partNumber)
	{
		Result result;
		bool finished = false;
		QEventLoop loop;
		migrate(partNumber, [&result, &finished, &loop](const Result& answer)
			{
				result = answer;
				finished = true;
				loop.quit();
			});
		// A refusal resolves inside migrate() itself, before there is a loop to quit — entering
		// one then would wait for an event that has already happened.
		if (!finished)
		{
			loop.exec();
		}
		return result;
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
