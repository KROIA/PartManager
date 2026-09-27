#include "controllers/PartManager_LlmController.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_Part.h"
#include "llm/PartManager_DatasheetToolset.h"
#include "llm/PartManager_EcadDownloadToolset.h"
#include "llm/PartManager_KicadToolset.h"
#include "llm/PartManager_LlmTool.h"
#include "llm/PartManager_MouserToolset.h"
#include "llm/PartManager_PartToolset.h"
#include "llm/PartManager_UiToolset.h"
#include "settings/PartManager_Settings.h"

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

		// §14b. Not the default, but what Claude starts from when it is chosen. The URL is only
		// reached when the environment names no gateway of its own — see below.
		const char* const ClaudeDefaultModel = "claude-sonnet-5";
		const char* const AnthropicDefaultChatUrl = "https://api.anthropic.com/v1/messages";

		// §14f and §9: the key the environment supplies, and the only key that survives a restart —
		// nothing writes one to the settings file. Never logged, never printed, not even its
		// length. ANTHROPIC_FOUNDRY_API_KEY comes first because a machine that has both is on the
		// gateway; ANTHROPIC_API_KEY is the plain Anthropic account.
		QString anthropicApiKey()
		{
			const QString gatewayKey = qEnvironmentVariable("ANTHROPIC_FOUNDRY_API_KEY");
			return gatewayKey.isEmpty() ? qEnvironmentVariable("ANTHROPIC_API_KEY") : gatewayKey;
		}

		// Trailing slashes off — "…//v1/messages" is something a gateway is entitled to 404 on —
		// and then the Messages path on, unless the URL already ends in it.
		//
		// This is the one place that knows the difference between a gateway *base* and a chat
		// endpoint, because the settings field cannot tell them apart and neither can the user:
		// ANTHROPIC_FOUNDRY_BASE_URL carries no path (QtLLM's own FoundryDemo appends the same
		// suffix), the field opens showing a URL *with* the path, so a base URL pasted over it
		// looks like the same kind of thing and is not — and nothing says otherwise until the
		// first prompt fails on a path the gateway does not serve. Every Anthropic Messages
		// endpoint ends in /messages, so one that already does is left exactly as typed: a gateway
		// on a non-standard path stays reachable, it just has to name the path.
		QString normalizedChatUrl(QString url)
		{
			while (url.endsWith(QLatin1Char('/')))
			{
				url.chop(1);
			}
			if (url.isEmpty() || url.endsWith(QStringLiteral("/messages")))
			{
				return url;
			}
			return url + QStringLiteral("/v1/messages");
		}

		// Only a *default*: the endpoint field stays editable, and a URL the user types there is
		// remembered and wins over this. It goes through the same normalization either way.
		QString anthropicDefaultEndpoint()
		{
			const QString baseUrl = qEnvironmentVariable("ANTHROPIC_FOUNDRY_BASE_URL");
			return normalizedChatUrl(baseUrl.isEmpty()
				? QString::fromLatin1(AnthropicDefaultChatUrl) : baseUrl);
		}

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
				"you were given. The only files you can reach are the ZIP archives in their "
				"download folder, through list_downloaded_libraries and the two tools that take a "
				"name from it, and a datasheet PDF the user names for set_part_datasheet. You "
				"cannot open, read or write any other file.\n"
				"Rules that matter:\n"
				"- Never invent an id. A categoryId or partId must have come out of a tool result "
				"in this conversation.\n"
				"- A tool that refuses tells you what it expected. Read that and correct the call "
				"instead of repeating it.\n"
				"- Look a part up with mouser_search before guessing its ratings.\n"
				"- After any tool that writes, say what you changed and name the part or category.\n"
				// §14a. Without this the model has no idea a window exists, and answers "the
				// selected component" by picking a part out of a search it ran itself.
				"- You can see and drive the app's Component Browser. When the user says \"the "
				"selected part\", \"this component\" or anything else that points at their screen, "
				"call ui_get_state to find out which part and category they have selected instead "
				"of guessing one.\n"
				// §5c/§14f. Without this the model answers "I downloaded the library already" by
				// asking for a path — which is the one thing it may not be given.
				"- When the user says they already downloaded a symbol/footprint library or an "
				"ECAD zip, call list_downloaded_libraries (filter by mpn, or by "
				"modifiedWithinHours with current_date_time for \"today\"), then "
				"attach_downloaded_library with the part's id. Never ask them for a file path and "
				"never make one up: pass back the \"file\" name a listing gave you.\n"
				// §3/§14g. The one tool that does take a path, and the one answer that must not be
				// read as "stored, therefore readable".
				"- set_part_datasheet gives a part its datasheet, from a link or from a PDF the "
				"user names. It is the only tool that accepts a path, and only one they typed. If "
				"it answers \"readable\": false, the PDF is a scan: say it is attached but that "
				"you cannot read it, and never answer specifications out of it.\n"
				"- ui_select_part, ui_set_filter and ui_open_part_editor change what the user is "
				"looking at. Use them to show them what you mean, and say afterwards what you "
				"changed — those are their own search boxes.\n"
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
		// §14a's window bridge, or null when the host has no Component Browser. Not owned.
		LlmUiBridge* ui = nullptr;
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

		// Kept per provider rather than read back off the Client, because the Client only ever
		// holds the current one. Carrying a single model across a provider switch is what sent
		// gpt-oss:20b to Anthropic; claude-sonnet-5 sent to Ollama is just as wrong.
		QString ollamaUrl;
		QString ollamaModel;
		// Always normalized: every assignment to it goes through normalizedChatUrl(), which is
		// what lets savePreferences() compare it against the env default and get a like-for-like
		// answer. A raw assignment added later would silently start persisting a value that should
		// have stayed empty.
		QString claudeEndpoint;
		QString claudeModel;
		// **A fallback is not a preference.** `ollamaModel` is whatever is live, which is what the
		// dialog and the status line have to show — but resolveModel() also writes it when §14b's
		// own fallback picks qwen3:8b on a machine that has no gpt-oss:20b. Persisting *that* would
		// turn an automatic second choice into a pinned first one: the next Apply made for an
		// unrelated reason would store it, and it would then outrank gpt-oss:20b even after the
		// user pulls it, with nothing in the UI to say why. Only a model that came out of the
		// dialog's combo sets this flag, and only this flag makes a model outrank §14b or reach
		// the settings file.
		bool ollamaModelIsUserChoice = false;

		// A key typed into the settings dialog, for this session only. §14f still holds for the
		// part that matters: it is never written to AppPreferences, so the next launch is back to
		// whatever the environment says. Empty means "use the environment", which is what clearing
		// the field has to mean — authenticating with a deliberate empty string helps nobody.
		QString claudeApiKeyOverride;

		// The key actually in force. Everything that needs one goes through here so the typed key
		// and the environment cannot disagree between the client, the dialog and the status line.
		QString claudeApiKey() const
		{
			return claudeApiKeyOverride.isEmpty() ? anthropicApiKey() : claudeApiKeyOverride;
		}
		// ChatDockWidget has setFontSizePercent() and no getter, so the value the settings dialog
		// must show back has to be remembered here. showToolCalls() does have a getter; it is kept
		// beside its neighbour so one struct holds everything that gets written to the settings.
		int fontSizePercent = 100;
		bool showToolCalls = true;

		// Set only when a remembered Claude provider had to fall back for want of a key. Appended
		// to every status line rather than shown once: the Ollama probe answers asynchronously and
		// its "Ready — …" would otherwise be the last word, leaving no trace of why Claude is off.
		QString providerFallbackNotice;

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
				chat->setStatusText(providerFallbackNotice.isEmpty()
					? text : text + QLatin1Char(' ') + providerFallbackNotice);
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
		// Created on demand: starting on Claude must not start an Ollama server the user is not
		// using, and switching to Ollama later must still find out whether one is there.
		void ensureOllamaProbe();
		// §14b. Everything the settings file holds about the assistant, written on every Apply.
		// A value equal to the current default is written as empty, so a later change to
		// ANTHROPIC_FOUNDRY_BASE_URL or to §14b's preferred model is followed rather than frozen.
		void savePreferences() const;
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
		// A model the user picked is tried first: §14b's preference is what to do in the *absence*
		// of a choice, not an override of one. Only a choice, though — the model this function
		// last resolved on its own is deliberately not in front of PreferredModel, or the fallback
		// it landed on would outrank gpt-oss:20b for good once the user finally pulls it.
		const QStringList candidates{ ollamaModelIsUserChoice ? ollamaModel : QString(),
			QString::fromLatin1(PreferredModel), QString::fromLatin1(FallbackModel) };
		for (const QString& name : candidates)
		{
			if (name.isEmpty())
			{
				continue;
			}
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
		// Live, so the dialog and the status line show what is actually answering. Not a choice,
		// so ollamaModelIsUserChoice stays as it was: if this is §14b's fallback it must not
		// become the thing §14b is fallen back *from*.
		ollamaModel = chosen;
		client->setModel(chosen);
		// The model id is not app chrome — it is the name the user typed into `ollama pull`.
		status(LlmController::tr("Ready — %1.").arg(chosen));
	}

	void LlmController::Impl::ensureOllamaProbe()
	{
		if (ollama == nullptr)
		{
			ollama = new QtLLM::OllamaManager(QString::fromLatin1(OllamaBaseUrl), &owner);
			QObject::connect(ollama, &QtLLM::OllamaManager::isRunningChecked, &owner,
				[this](bool running) { onOllamaChecked(running); });
		}
		startAttempts = 0;
		status(LlmController::tr("Looking for Ollama…"));
		ollama->checkIsRunning();
	}

	void LlmController::Impl::savePreferences() const
	{
		// Read-modify-write rather than a fresh struct: the §9 preferences share this group, and
		// the assistant's dialog has no business blanking the user's language or backup schedule.
		AppPreferences preferences = Settings::getPreferences();
		preferences.llmProvider = usingOllama ? "ollama" : "claude";
		preferences.llmOllamaUrl = ollamaUrl == QString::fromLatin1(OllamaChatUrl)
			? std::string() : ollamaUrl.toStdString();
		// Only a model out of the dialog's combo is written. What resolveModel() settled on by
		// itself is left empty — "whatever §14b says now" — so a machine that had to fall back to
		// qwen3:8b goes back to gpt-oss:20b the moment it is pulled, instead of carrying the
		// fallback forward as a preference nobody made and nothing explains.
		preferences.llmOllamaModel = ollamaModelIsUserChoice
			? ollamaModel.toStdString() : std::string();
		preferences.llmClaudeEndpoint = claudeEndpoint == anthropicDefaultEndpoint()
			? std::string() : claudeEndpoint.toStdString();
		preferences.llmClaudeModel = claudeModel == QString::fromLatin1(ClaudeDefaultModel)
			? std::string() : claudeModel.toStdString();
		preferences.llmSystemPrompt = systemPrompt == chatSystemPrompt()
			? std::string() : systemPrompt.toStdString();
		preferences.llmShowToolCalls = showToolCalls;
		preferences.llmFontSizePercent = fontSizePercent;
		// No key goes in here and none ever will — §14f, and the note on AppPreferences.
		Settings::setPreferences(preferences);
	}

	void LlmController::Impl::applySettings(QtLLM::SettingsDialog& dialog)
	{
		const bool wantOllama = dialog.provider() == QtLLM::SettingsDialog::Provider::Ollama;
		// The user has been to the dialog, so whatever the launch-time fallback had to say about
		// a missing key is either no longer true or is about to be said again below.
		providerFallbackNotice.clear();

		// Filed against the provider the dialog is currently showing, not against the live one:
		// the dialog has one model combo and one endpoint field between two providers, and that
		// is precisely how gpt-oss:20b used to end up being sent to Anthropic.
		const QString editedEndpoint = wantOllama ? dialog.ollamaUrl() : dialog.endpointUrl();
		if (!editedEndpoint.isEmpty())
		{
			if (wantOllama)
			{
				// Not normalized: an Ollama chat URL ends in /api/chat, a different shape entirely.
				ollamaUrl = editedEndpoint;
			}
			else
			{
				claudeEndpoint = normalizedChatUrl(editedEndpoint);
			}
		}
		// **The combo is only the user's while the provider combo stays put.** QtLLM's
		// SettingsDialog::onProviderChanged() clears the model combo and writes its own text into
		// it — "claude-haiku-4-5" for Claude and "llama3.2:latest" for Ollama. Read back as a
		// choice, one switch to Ollama and back would pin llama3.2, the model §14b measured as
		// unusable and deliberately left out of the fallback list precisely because it gets picked
		// silently. So an Apply that changes the provider keeps the remembered model for the
		// provider being switched to; changing the model takes a second Apply, by which point the
		// text in the combo is the user's own.
		const bool providerChanged = wantOllama != usingOllama;
		if (!providerChanged && !dialog.model().isEmpty())
		{
			// And only a value that differs from what is already live is a decision. Apply pressed
			// for the font size with the combo untouched must not turn whatever resolveModel() last
			// settled on into a pinned choice — that is the whole distinction the flag exists for.
			if (wantOllama)
			{
				ollamaModelIsUserChoice = ollamaModelIsUserChoice || dialog.model() != ollamaModel;
			}
			(wantOllama ? ollamaModel : claudeModel) = dialog.model();
		}

		// The field opens pre-filled from the environment, so an untouched dialog hands back the
		// same string and nothing changes. A different one is the user overriding the environment
		// for this session; a cleared one is them asking for the environment back.
		claudeApiKeyOverride = dialog.apiKey();
		// Resolved after that, and on every Apply rather than cached at launch: the environment
		// may have gained the variable since, and this is the one place that can act on it
		// without a restart.
		const QString claudeKey = claudeApiKey();
		if (providerChanged)
		{
			// setProvider() replaces the protocol, re-syncs the registered tools onto it and
			// clears the history — two providers' message formats do not interleave.
			usingOllama = wantOllama;
			if (wantOllama)
			{
				client->setProvider(QtLLM::Provider::Ollama, ollamaUrl);
			}
			else
			{
				client->setProvider(QtLLM::Provider::Claude, claudeEndpoint, claudeKey);
				providerReachable = true;
			}
			if (!chat.isNull())
			{
				chat->clearMessages();
			}
			if (wantOllama)
			{
				// Nothing has asked the server what it offers yet if the app started on Claude,
				// and the answer is what resolveModel() needs to confirm a model.
				ensureOllamaProbe();
			}
		}
		else
		{
			// Same provider, edited endpoint. setProvider() would clear the conversation for
			// nothing, so the URL goes in on its own — and an edit made without switching
			// providers used to be dropped entirely.
			client->setEndpointUrl(wantOllama ? ollamaUrl : claudeEndpoint);
			if (!wantOllama)
			{
				client->setApiKey(claudeKey);
			}
		}

		const QString model = wantOllama ? ollamaModel : claudeModel;
		if (!model.isEmpty())
		{
			client->setModel(model);
			modelResolved = true;
		}
		systemPrompt = dialog.systemPrompt();
		client->setSystemPrompt(systemPrompt);
		showToolCalls = dialog.showToolCalls();
		fontSizePercent = dialog.fontSizePercent();
		if (!chat.isNull())
		{
			chat->setShowToolCalls(showToolCalls);
			chat->setFontSizePercent(fontSizePercent);
		}

		if (!wantOllama && claudeKey.isEmpty())
		{
			// Said here rather than left to the first prompt, which would fail with an
			// authentication error that names neither variable. Both ways out are offered: the
			// dialog's field lasts the session, the environment lasts. The variable names are
			// literals, not chrome — they are what the user has to type into their environment.
			status(LlmController::tr(
				"Claude needs a key. Type one into the assistant settings, or set %1 (or %2) in "
				"the environment.")
				.arg(QStringLiteral("ANTHROPIC_FOUNDRY_API_KEY"),
					QStringLiteral("ANTHROPIC_API_KEY")));
		}
		savePreferences();
	}

	LlmController::LlmController(DatabaseHandle& handle, QWidget* dialogParent, LlmUiBridge* ui,
		QObject* parent)
		: QObject(parent)
		, m_impl(new Impl(*this, handle))
	{
		m_impl->dialogParent = dialogParent;
		m_impl->ui = ui;

		// §14b. An empty setting is "whatever the default is now", never a stored empty value —
		// which is what lets a changed ANTHROPIC_FOUNDRY_BASE_URL be followed rather than
		// overridden by the copy of itself that was current when the dialog was last used.
		const AppPreferences preferences = Settings::getPreferences();
		m_impl->ollamaUrl = preferences.llmOllamaUrl.empty()
			? QString::fromLatin1(OllamaChatUrl) : toQt(preferences.llmOllamaUrl);
		// A stored model id can only have got there through the dialog — savePreferences() writes
		// nothing else — so reading one back restores the choice, not just the string.
		m_impl->ollamaModelIsUserChoice = !preferences.llmOllamaModel.empty();
		m_impl->ollamaModel = preferences.llmOllamaModel.empty()
			? QString::fromLatin1(PreferredModel) : toQt(preferences.llmOllamaModel);
		// Normalized on the way in as well: a bare gateway base stored by an older build becomes
		// the Messages endpoint here, and then matches the env default again, so the next Apply
		// writes it back as empty and the setting heals itself.
		m_impl->claudeEndpoint = preferences.llmClaudeEndpoint.empty()
			? anthropicDefaultEndpoint() : normalizedChatUrl(toQt(preferences.llmClaudeEndpoint));
		m_impl->claudeModel = preferences.llmClaudeModel.empty()
			? QString::fromLatin1(ClaudeDefaultModel) : toQt(preferences.llmClaudeModel);
		m_impl->systemPrompt = preferences.llmSystemPrompt.empty()
			? chatSystemPrompt() : toQt(preferences.llmSystemPrompt);
		m_impl->showToolCalls = preferences.llmShowToolCalls;
		m_impl->fontSizePercent = preferences.llmFontSizePercent;

		// A remembered Claude provider with no key in the environment would build a client that
		// fails on the first prompt with an authentication error naming nothing the user can act
		// on. Start on Ollama and say which variable is missing instead.
		const QString claudeKey = anthropicApiKey();
		const bool claudeKeyMissing = preferences.llmProvider == "claude" && claudeKey.isEmpty();
		m_impl->usingOllama = preferences.llmProvider != "claude" || claudeKeyMissing;
		if (claudeKeyMissing)
		{
			m_impl->providerFallbackNotice = tr(
				"Claude is switched off: neither %1 nor %2 is set in the environment.")
				.arg(QStringLiteral("ANTHROPIC_FOUNDRY_API_KEY"),
					QStringLiteral("ANTHROPIC_API_KEY"));
		}

		if (m_impl->usingOllama)
		{
			m_impl->client = new QtLLM::Client(QtLLM::Provider::Ollama, m_impl->ollamaUrl,
				QString(), this);
			m_impl->client->setModel(m_impl->ollamaModel);
		}
		else
		{
			m_impl->client = new QtLLM::Client(QtLLM::Provider::Claude, m_impl->claudeEndpoint,
				claudeKey, this);
			m_impl->client->setModel(m_impl->claudeModel);
			// Nothing to probe and nothing to resolve: a hosted provider is reachable or it is
			// not, and that is what the first request finds out.
			m_impl->providerReachable = true;
			m_impl->modelResolved = true;
		}
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
		// it is the audit trail that makes an assistant with write access acceptable at all, which
		// is why AppPreferences defaults it on and only an explicit tick turns it off.
		m_impl->chat->setShowToolCalls(m_impl->showToolCalls);
		m_impl->chat->setFontSizePercent(m_impl->fontSizePercent);
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
		// §14f: the KiCad tools are safe to leave on beside the others because none of them takes
		// a filesystem path — a file only ever arrives from another part in the same database.
		registerLlmTools(*m_impl->client, KicadToolset::tools(context));
		// §14g: the datasheet tools are reads only, and they obey the same §14f rule — the model
		// names a part, never a file. A datasheet that cannot be read (a scan, an encrypted file)
		// is answered as exactly that, because the failure this feature must not have is an empty
		// answer the model fills in from its own memory of what the part does.
		registerLlmTools(*m_impl->client, DatasheetToolset::tools(context));
		// §5c/§14f: the one toolset that reaches the filesystem, and the one place that rule is
		// relaxed. The model never supplies a path — it supplies a file name that came back from
		// list_downloaded_libraries, resolved against the user's download folder and nothing else.
		// Registered only when there is a folder to look in, for the same reason as the UI tools
		// below: three tools that answer every call with "there is no folder" are worse than none.
		const QStringList downloadRoots = EcadDownloadContext::defaultRoots();
		if (!downloadRoots.isEmpty())
		{
			EcadDownloadContext downloadContext;
			downloadContext.database = &m_impl->database;
			downloadContext.allowWrites = true;
			downloadContext.allowedRoots = downloadRoots;
			// Null when this host has no Component Browser, and then nothing has an editor open to
			// go stale. set_part_datasheet is the one tool here that writes a *part row*, so it is
			// the one that has to tell an open editor to re-read (§10).
			downloadContext.ui = m_impl->ui;
			registerLlmTools(*m_impl->client, EcadDownloadToolset::tools(downloadContext));
		}
		// §14a: the only toolset that is not in core/, because a selection is a widget fact and
		// core/ is widget-free (§12a). It goes through LlmUiBridge rather than MainWindow, so
		// nothing here — or in the test binary — pulls the app's dialog stack in behind it.
		// Registered only when there is a window: five tools that answer every call with "there is
		// no Component Browser" are worse than five tools the model was never offered.
		if (m_impl->ui != nullptr)
		{
			UiToolContext uiContext;
			uiContext.ui = m_impl->ui;
			uiContext.database = &m_impl->database;
			registerLlmTools(*m_impl->client, UiToolset::tools(uiContext));
		}

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

		if (m_impl->usingOllama)
		{
			m_impl->ensureOllamaProbe();
		}
		else
		{
			// Skipped on purpose when the user is on Claude: the probe starts `ollama serve` when
			// it finds nothing, and that is a server nobody asked for. The model id is the name
			// Anthropic knows it by, so only the frame around it is translated.
			m_impl->status(tr("Ready — %1.").arg(m_impl->claudeModel));
		}
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
		dialog.setOllamaUrl(m_impl->ollamaUrl);
		// The endpoint field stays editable, so what it must show is the URL Claude would actually
		// be talked to on — the remembered one, or the one ANTHROPIC_FOUNDRY_BASE_URL implies.
		// Left blank, as it was, there was nothing to correct and nothing to see.
		dialog.setEndpointUrl(m_impl->claudeEndpoint);
		// One combo between two providers, so it has to hold the model belonging to the provider
		// the dialog opens on. Filled from client->model() it showed gpt-oss:20b while Claude was
		// selected, and Apply then sent that id to Anthropic.
		dialog.setModel(m_impl->usingOllama ? m_impl->ollamaModel : m_impl->claudeModel);
		dialog.setSystemPrompt(m_impl->systemPrompt);
		dialog.setShowToolCalls(m_impl->showToolCalls);
		dialog.setFontSizePercent(m_impl->fontSizePercent);
		// §14f and §9: the field is pre-filled from ANTHROPIC_FOUNDRY_API_KEY, or failing that
		// ANTHROPIC_API_KEY, so it opens showing the key that is actually in force rather than
		// blank — including on a second visit, after a key has been typed. A typed key overrides
		// the environment **for this session only**; clearing the field hands it back.
		//
		// What has not changed is the part §14f is about: nothing here is ever written to
		// AppPreferences. A persisted key would land in a plain-text file in the user's data
		// folder, which is exactly what keeping it in the environment avoids, so the next launch
		// starts from the environment again. QtLLM's field is QLineEdit::Password echo, so this
		// is a masked field and not a key on screen.
		dialog.setApiKey(m_impl->claudeApiKey());
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
