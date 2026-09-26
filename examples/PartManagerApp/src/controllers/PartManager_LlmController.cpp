#include "controllers/PartManager_LlmController.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_Part.h"
#include "llm/PartManager_LlmTool.h"
#include "llm/PartManager_MouserToolset.h"
#include "llm/PartManager_PartToolset.h"

#include <BuiltinTools.h>
#include <ChatDockWidget.h>
#include <Client.h>
#include <OllamaManager.h>
#include <SettingsDialog.h>

#include <QJsonObject>
#include <QLatin1Char>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>

namespace PartManager
{
	namespace
	{
		// §14b. Local first: a parts database is a place where a feature that costs money per
		// click does not get used. The chat endpoint and the server's own root are different
		// paths on the same server — Client wants /api/chat, OllamaManager wants the root.
		const char* const OllamaBaseUrl = "http://localhost:11434";
		const char* const OllamaChatUrl = "http://localhost:11434/api/chat";

		// Measured 2026-09-26 against the real migration loop; ARCHITECTURE §14b has the table.
		// gpt-oss:20b got it right in 6 calls and recovered from a rejected enum on its own;
		// qwen3:8b is also correct but reasons at length before every call.
		//
		// **llama3.2 is deliberately not a fallback.** It ships with almost every Ollama install,
		// so Client::validateCurrentModel() — which takes models.first() when the configured
		// model is missing — lands on it by default, and it called create_part with empty
		// strings for every field and then printed an invented result. Impl::resolveModel() is
		// connected to the same signal *after* the client is constructed, so it runs after that
		// built-in correction and puts the choice back where §14b left it.
		const char* const PreferredModel = "gpt-oss:20b";
		const char* const FallbackModel = "qwen3:8b";

		// §14f. Off by default in QtLLM, both required here: a local model was observed looping
		// on a tool call, and the cap is what ends that rather than the user's patience. Twelve
		// is the number MigrationAgent runs with — twice the longest correct run, and then some.
		constexpr int MaxToolCallsPerTurn = 12;

		// `ollama serve` needs a moment before it answers. Ten tries at half a second is five
		// seconds: longer than a warm start, short enough not to look hung.
		constexpr int OllamaStartRetries = 10;
		constexpr int OllamaRetryDelayMs = 500;

		// Not translated, and not by oversight: this is addressed to the model and not to the
		// user, and the last line is what makes the *answers* follow the user's language.
		QString chatSystemPrompt()
		{
			return QStringLiteral(
				"You are the assistant built into PartManager, a local inventory for electronic "
				"components. The user's parts live in a database you reach only through the tools "
				"you were given; you cannot read or write files.\n"
				"Rules that matter:\n"
				"- Never invent an id. A categoryId or partId must have come out of a tool result "
				"in this conversation.\n"
				"- A tool that refuses tells you what it expected. Read that and correct the call "
				"instead of repeating it.\n"
				"- Look a part up with mouser_search before guessing its ratings.\n"
				"- After any tool that writes, say what you changed and name the part or category.\n"
				"- Answer in the language the user writes in.");
		}

		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// "manufacturer: Murata, MPN: GRM188R71H104KA93D" — only the fields the part actually
		// carries. A prompt that says "manufacturer: (unknown)" invites an answer about a part
		// called "(unknown)", which is the kind of junk §14c had to validate out of the tools.
		QString identityLine(const Part& part)
		{
			QStringList fields;
			if (!part.manufacturer.empty())
			{
				fields << LlmController::tr("manufacturer: %1").arg(toQt(part.manufacturer));
			}
			if (!part.mpn.empty())
			{
				fields << LlmController::tr("MPN: %1").arg(toQt(part.mpn));
			}
			if (!part.package.empty())
			{
				fields << LlmController::tr("package: %1").arg(toQt(part.package));
			}
			return fields.join(QStringLiteral(", "));
		}
	}

	// Everything the controller owns. A struct rather than a pile of members on the class
	// because the header must not name a QtLLM type: the app links QtLLM, the code that includes
	// this header does not have to.
	struct LlmController::Impl
	{
		Impl(LlmController& controller, DatabaseHandle& handle)
			: owner(controller), database(handle) {}

		LlmController& owner;
		DatabaseHandle& database;
		QWidget* dialogParent = nullptr;
		QtLLM::Client* client = nullptr;
		// Guarded rather than owned outright: addDockWidget() reparents the panel to the window,
		// and from that moment the window deletes it. The destructor cleans up only the case
		// where nobody ever took it.
		QPointer<QtLLM::ChatDockWidget> chat;
		QtLLM::OllamaManager* ollama = nullptr;

		// Our own copy, because Client has setSystemPrompt() and no getter — the settings dialog
		// has to be able to show what is currently in force.
		QString systemPrompt;
		// Client has no provider() either, and the fallback rule below is an Ollama rule: a
		// Claude model list must not be second-guessed against gpt-oss:20b.
		bool usingOllama = true;

		bool providerReachable = false;
		bool modelResolved = false;
		bool busy = false;
		int startAttempts = 0;

		// One line under the conversation. Null-guarded everywhere because the panel is the
		// window's once it has been docked, and window teardown may reach it first.
		void status(const QString& text)
		{
			if (!chat.isNull())
			{
				chat->setStatusText(text);
			}
		}

		void setBusy(bool nowBusy)
		{
			if (busy == nowBusy)
			{
				return;
			}
			busy = nowBusy;
			emit owner.busyChanged(nowBusy);
		}

		// §14: if Ollama is not running the panel says so and offers to start it, rather than
		// failing on the first prompt with a network error nobody can act on.
		void onOllamaChecked(bool running);
		// The §14b model choice, applied after Client's own models.first() correction.
		void resolveModel(const QStringList& models);
		// Reads back whatever the QtLLM settings dialog was left holding. Never its API-key
		// field — see the note in openSettings().
		void applySettings(QtLLM::SettingsDialog& dialog);
	};

	void LlmController::Impl::onOllamaChecked(bool running)
	{
		if (!usingOllama)
		{
			return;
		}
		if (running)
		{
			providerReachable = true;
			startAttempts = 0;
			status(LlmController::tr("Ollama is running. Looking for a model…"));
			// Client asks on its own at construction, but that request went out while the server
			// was still down. Asking again is what turns "not running" into a resolved model.
			client->fetchAvailableModels();
			return;
		}

		providerReachable = false;
		if (startAttempts == 0)
		{
			status(LlmController::tr("Ollama is not running. Starting it…"));
			if (!QtLLM::OllamaManager::startServer())
			{
				status(LlmController::tr(
					"Ollama is not installed, or 'ollama serve' could not be started. The "
					"assistant needs it to answer."));
				return;
			}
		}
		if (startAttempts >= OllamaStartRetries)
		{
			status(LlmController::tr("Ollama did not start in time."));
			return;
		}
		++startAttempts;
		QTimer::singleShot(OllamaRetryDelayMs, &owner, [this]() { ollama->checkIsRunning(); });
	}

	void LlmController::Impl::resolveModel(const QStringList& models)
	{
		if (!usingOllama || models.isEmpty())
		{
			return;
		}

		QString chosen;
		for (const char* const candidate : { PreferredModel, FallbackModel })
		{
			const QString name = QString::fromLatin1(candidate);
			if (models.contains(name))
			{
				chosen = name;
				break;
			}
			// An Ollama model list carries the tag ("gpt-oss:20b"), but a user who pulled the
			// same weights under another tag still has them — match the name half too.
			const QString family = name.section(QLatin1Char(':'), 0, 0);
			for (const QString& offered : models)
			{
				if (offered.section(QLatin1Char(':'), 0, 0) == family)
				{
					chosen = offered;
					break;
				}
			}
			if (!chosen.isEmpty())
			{
				break;
			}
		}

		if (chosen.isEmpty())
		{
			// Neither measured-good model is installed. Client has already fallen back to
			// models.first() by now, which may well be llama3.2 — so say what is missing instead
			// of letting an unusable model answer as though it were fine (§14b).
			modelResolved = false;
			status(LlmController::tr(
				"Neither %1 nor %2 is installed. Run 'ollama pull %1' — the models that are "
				"installed cannot be relied on to call tools.")
				.arg(QString::fromLatin1(PreferredModel), QString::fromLatin1(FallbackModel)));
			return;
		}

		modelResolved = true;
		client->setModel(chosen);
		// The model id is not app chrome — it is the name the user typed into `ollama pull`.
		status(LlmController::tr("Ready — %1.").arg(chosen));
	}

	void LlmController::Impl::applySettings(QtLLM::SettingsDialog& dialog)
	{
		const bool wantOllama = dialog.provider() == QtLLM::SettingsDialog::Provider::Ollama;
		if (wantOllama != usingOllama)
		{
			// setProvider() replaces the protocol, re-syncs the registered tools onto it and
			// clears the history — two providers' message formats do not interleave. The Claude
			// key is read here, from the environment, and never from dialog.apiKey().
			usingOllama = wantOllama;
			if (wantOllama)
			{
				client->setProvider(QtLLM::Provider::Ollama,
					dialog.ollamaUrl().isEmpty()
						? QString::fromLatin1(OllamaChatUrl) : dialog.ollamaUrl());
			}
			else
			{
				client->setProvider(QtLLM::Provider::Claude, dialog.endpointUrl(),
					qEnvironmentVariable("ANTHROPIC_API_KEY"));
				providerReachable = true;
				modelResolved = !client->model().isEmpty();
			}
			if (!chat.isNull())
			{
				chat->clearMessages();
			}
		}
		else if (wantOllama && !dialog.ollamaUrl().isEmpty())
		{
			client->setEndpointUrl(dialog.ollamaUrl());
		}

		if (!dialog.model().isEmpty())
		{
			client->setModel(dialog.model());
			modelResolved = true;
		}
		systemPrompt = dialog.systemPrompt();
		client->setSystemPrompt(systemPrompt);
		if (!chat.isNull())
		{
			chat->setShowToolCalls(dialog.showToolCalls());
			chat->setFontSizePercent(dialog.fontSizePercent());
		}
	}

	LlmController::LlmController(DatabaseHandle& handle, QWidget* dialogParent, QObject* parent)
		: QObject(parent)
		, m_impl(new Impl(*this, handle))
	{
		m_impl->dialogParent = dialogParent;
		m_impl->systemPrompt = chatSystemPrompt();

		m_impl->client = new QtLLM::Client(QtLLM::Provider::Ollama,
			QString::fromLatin1(OllamaChatUrl), QString(), this);
		m_impl->client->setModel(QString::fromLatin1(PreferredModel));
		m_impl->client->setSystemPrompt(m_impl->systemPrompt);
		// §14f: both of these are off by default in the library and both are required here.
		m_impl->client->setValidateToolInput(true);
		m_impl->client->setMaxToolCallsPerTurn(MaxToolCallsPerTurn);

		m_impl->chat = new QtLLM::ChatDockWidget();
		// The dock's object name is what saveState()/restoreState() keys the layout off; the
		// window title is what the View menu's toggle is labelled with.
		m_impl->chat->setObjectName(QStringLiteral("llmChatDock"));
		m_impl->chat->setWindowTitle(tr("Assistant"));
		m_impl->chat->setClient(m_impl->client);
		// §14: the user needs to see which tools touched their database. Not a debug aid here —
		// it is the audit trail that makes an assistant with write access acceptable at all.
		m_impl->chat->setShowToolCalls(true);
		m_impl->chat->setAssistantName(tr("Assistant"));
		m_impl->chat->setSendButtonText(tr("Send"));
		m_impl->chat->setCancelButtonText(tr("Cancel"));
		m_impl->chat->setSettingsButtonTooltip(tr("Provider, model, tools and usage."));
		m_impl->chat->setInputPlaceholderText(tr("Ask about your parts, or paste a part number…"));
		m_impl->chat->setLoadingText(tr("Thinking…"));
		m_impl->chat->setCancelledText(tr("Request cancelled."));
		m_impl->chat->setBusyWarningText(tr("Please wait — the assistant is still answering."));

		// §14a: the toolsets hand back vectors and register nothing themselves, so this is the
		// one place a live client meets them. Writes are allowed — this is the user's own chat
		// about their own database, and every call is drawn as a card in the conversation.
		LlmToolContext context;
		context.database = &m_impl->database;
		context.allowWrites = true;
		registerLlmTools(*m_impl->client, PartToolset::tools(context));
		registerLlmTools(*m_impl->client, MouserToolset::tools(context));

		// §14f: the filesystem built-ins (read_text_file, write_text_file, list_directory) are
		// deliberately absent — the assistant reaches parts through typed tools and nothing else,
		// and a general file-write tool removes that guarantee while adding nothing.
		// AskUserQuestion needs the chat to draw its card into; CurrentDateTime is here because a
		// model has no clock and will otherwise date a note to its training cutoff.
		QtLLM::BuiltinTools::Context builtinContext;
		builtinContext.chat = m_impl->chat.data();
		builtinContext.dialogParent = dialogParent;
		QtLLM::BuiltinTools::registerTools(m_impl->client,
			{ QtLLM::BuiltinTool::AskUserQuestion, QtLLM::BuiltinTool::CurrentDateTime },
			builtinContext);

		connect(m_impl->chat.data(), &QtLLM::ChatDockWidget::messageSent,
			this, [this](const QString& text) { m_impl->client->sendPrompt(text); });
		// QtLLM's Client has no abort for a request already on the wire, so Cancel stops the
		// waiting and not the turn: the panel goes idle and a late reply still arrives. Saying
		// so beats a Cancel button that silently does nothing.
		connect(m_impl->chat.data(), &QtLLM::ChatDockWidget::cancelRequested, this, [this]()
			{
				if (!m_impl->chat.isNull())
				{
					m_impl->chat->setLoading(false);
				}
				m_impl->setBusy(false);
				m_impl->status(tr("Stopped waiting. A reply already on its way will still arrive."));
			});
		connect(m_impl->chat.data(), &QtLLM::ChatDockWidget::settingsRequested,
			this, &LlmController::openSettings);

		connect(m_impl->client, &QtLLM::Client::requestStarted, this, [this]()
			{
				if (!m_impl->chat.isNull())
				{
					m_impl->chat->setLoading(true);
				}
				m_impl->setBusy(true);
			});
		connect(m_impl->client, &QtLLM::Client::requestFinished, this, [this]()
			{
				if (!m_impl->chat.isNull())
				{
					m_impl->chat->setLoading(false);
				}
				m_impl->setBusy(false);
			});
		connect(m_impl->client, &QtLLM::Client::responseReady, this, [this](const QString& text)
			{
				if (!m_impl->chat.isNull())
				{
					m_impl->chat->addAssistantMessage(text);
					m_impl->chat->clearStatus();
				}
			});
		connect(m_impl->client, &QtLLM::Client::errorOccurred, this, [this](const QString& message)
			{
				// Into the conversation, not into a message box: the error belongs where the
				// turn that caused it is, and a modal over a dock the user may have hidden is a
				// worse way to say "Ollama is not running".
				if (!m_impl->chat.isNull())
				{
					m_impl->chat->addAssistantMessage(tr("The request failed: %1").arg(message));
				}
			});
		connect(m_impl->client, &QtLLM::Client::toolInvoked,
			this, [this](const QString& toolName, const QJsonObject&)
			{
				// The tool name is the model-facing identifier, not app chrome, so only the
				// frame around it is translated.
				m_impl->status(tr("Running %1…").arg(toolName));
			});
		connect(m_impl->client, &QtLLM::Client::toolCallLimitReached, this, [this](int limit)
			{
				m_impl->status(tr("Stopped after %1 tool calls in one turn.").arg(limit));
			});
		connect(m_impl->client, &QtLLM::Client::statsUpdated,
			this, [this](const QtLLM::UsageStats& stats)
			{
				if (!m_impl->chat.isNull())
				{
					m_impl->chat->updateTokenUsage(stats.sessionInputTokens,
						stats.sessionOutputTokens);
				}
			});
		// Connected here rather than inside the library: Client::validateCurrentModel() already
		// listens to this signal and falls back to models.first(). Later connections to the same
		// signal fire later, so this one gets the last word (see the PreferredModel note).
		connect(m_impl->client, &QtLLM::Client::modelsAvailable,
			this, [this](const QStringList& models) { m_impl->resolveModel(models); });

		m_impl->ollama = new QtLLM::OllamaManager(QString::fromLatin1(OllamaBaseUrl), this);
		connect(m_impl->ollama, &QtLLM::OllamaManager::isRunningChecked,
			this, [this](bool running) { m_impl->onOllamaChecked(running); });
		m_impl->status(tr("Looking for Ollama…"));
		m_impl->ollama->checkIsRunning();
	}

	LlmController::~LlmController()
	{
		// Only when the window never took it. Once addDockWidget() has run the dock is a child of
		// the main window, and deleting it here would be the second delete.
		if (!m_impl->chat.isNull() && m_impl->chat->parent() == nullptr)
		{
			delete m_impl->chat.data();
		}
	}

	QtLLM::ChatDockWidget* LlmController::chatDock() const
	{
		return m_impl->chat.data();
	}

	bool LlmController::isReady() const
	{
		return m_impl->providerReachable && m_impl->modelResolved;
	}

	bool LlmController::injectPrompt(const QString& prompt)
	{
		if (m_impl->chat.isNull())
		{
			return false;
		}
		// §14d: straight through submitPrompt(), so the text is drawn as a user bubble and sent
		// as a visible turn. It refuses on its own while a turn is in flight, and that refusal is
		// what the caller reports — queueing here is how two impatient clicks become two paid
		// turns.
		return m_impl->chat->submitPrompt(prompt);
	}

	void LlmController::openSettings()
	{
		QtLLM::SettingsDialog dialog(m_impl->dialogParent);
		dialog.setWindowTitle(tr("Assistant Settings"));
		dialog.setProvider(m_impl->usingOllama
			? QtLLM::SettingsDialog::Provider::Ollama
			: QtLLM::SettingsDialog::Provider::Claude);
		dialog.setOllamaUrl(QString::fromLatin1(OllamaChatUrl));
		dialog.setModel(m_impl->client->model());
		dialog.setSystemPrompt(m_impl->systemPrompt);
		dialog.setShowToolCalls(m_impl->chat.isNull() ? true : m_impl->chat->showToolCalls());
		// §14f and §9: the library's dialog carries an API-key field of its own, and this
		// deliberately neither fills it in nor reads it back. A Claude key comes from
		// ANTHROPIC_API_KEY in the environment and from nowhere else, exactly as the Mouser keys
		// do — a key typed into a settings dialog lands in a plain-text file in the user's data
		// folder, which is the thing keeping it in the environment avoids.
		dialog.setApiKey(QString());
		// Fills the Tools tab (every registered tool, enable/disable) and the usage charts. The
		// Agents tab binds itself to AgentRegistry and needs no wiring.
		dialog.setClient(m_impl->client);
		dialog.setUsageHistory(m_impl->client->usageHistory());

		connect(&dialog, &QtLLM::SettingsDialog::settingsApplied,
			this, [this, &dialog]() { m_impl->applySettings(dialog); });
		dialog.exec();
	}

	QString LlmController::describePartPrompt(const Part& part, const QString& categoryName)
	{
		// §14d: text a human could have typed, and one that asks for the description *and* for
		// it to be stored — the part editor closes before this is sent (see
		// PartEditorDialog::generateDescription), so nothing is left behind to overwrite it.
		// The part name, the category name and the identity fields are all user data, so only
		// the frame around them is translated.
		const QString identity = identityLine(part);
		QString prompt = tr("Write a description for the part \"%1\" in my \"%2\" category.")
			.arg(toQt(part.name), categoryName);
		if (!identity.isEmpty())
		{
			prompt += QLatin1Char(' ') + tr("What I know about it: %1.").arg(identity);
		}
		prompt += QLatin1Char('\n');
		prompt += tr("Two or three sentences: what it is, the ratings that matter when picking "
			"one, and what it is normally used for. Look it up with mouser_search if you need "
			"the details. Then save the text with update_part on part id %1.").arg(part.id);
		return prompt;
	}

	QString LlmController::migratePartPrompt(const QString& partNumber)
	{
		return tr("Add %1 to my database: look it up on Mouser, put it in the category it belongs "
			"in — create one if nothing fits — and create the part with its datasheet and photo. "
			"Tell me which category you chose and why.").arg(partNumber);
	}

	QString LlmController::kicadQuestionPrompt(const Part& part)
	{
		const QString identity = identityLine(part);
		QString prompt = tr("Which KiCad symbol and footprint should I use for \"%1\"?")
			.arg(toQt(part.name));
		if (!identity.isEmpty())
		{
			prompt += QLatin1Char(' ') + tr("What I know about it: %1.").arg(identity);
		}
		prompt += QLatin1Char('\n');
		prompt += tr("Name the standard KiCad library symbol and footprint if there is one, say "
			"what the pin order is, and tell me what to check before I trust it.");
		return prompt;
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
