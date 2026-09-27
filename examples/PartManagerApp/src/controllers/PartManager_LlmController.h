// @file PartManager_LlmController.h
// @brief Owns the app's one LLM client and the chat panel it talks through (§14b, §14d).
//
// The chat is a third dock beside the browser and the partlists, because that is
// what it is: a panel the user turns on and off, not a modal conversation that
// stops the app. `QtLLM::ChatDockWidget` is already a `QDockWidget`, so the
// controller hands the window a ready dock rather than wrapping one.
//
// **Prompt injection is the point of the button, not a shortcut around it.**
// "Generate description" does not call the model behind the user's back — it
// injects the prompt into the chat through `ChatDockWidget::submitPrompt()`, so
// it renders as a user bubble, costs a visible turn, and leaves the answer, the
// tool calls and the cost in the same place every other answer lives. A feature
// that quietly spends tokens is a feature nobody can audit. `submitPrompt()`
// refuses while a turn is in flight, which is why `injectPrompt()` returns a
// bool and the buttons report it rather than queueing.
//
// The client is rebuilt on a §1b database switch rather than re-pointed: every
// registered tool handler captured the old `DatabaseHandle*`, and handing them a
// new one is exactly the live-handle-swap that `MainWindow` already refused.
// @see docs/design/ARCHITECTURE.md §14, §14b, §14d, §1b
// @see PartManager_PartToolset.h, PartManager_MouserToolset.h
#pragma once

#include "PartManager_global.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include <QObject>
#include <QString>
#include <memory>

namespace QtLLM { class ChatDockWidget; class Client; }

namespace PartManager
{

	class DatabaseHandle;
	class LlmUiBridge;
	struct Part;

	class LlmController : public QObject
	{
		Q_OBJECT
	public:
		// `ui` is the window the §14a UI toolset reads and drives — the selection, the §7a filter
		// boxes, the part editor. Null is a supported state and means those five tools are not
		// registered at all: a host with no Component Browser has nothing for them to report, and
		// a tool that answers every call with "there is no window" is worse than a tool the model
		// was never offered. Not owned; it outlives this controller, being the same object that
		// is passed as `dialogParent`.
		LlmController(DatabaseHandle& handle, QWidget* dialogParent, LlmUiBridge* ui = nullptr,
			QObject* parent = nullptr);
		~LlmController() override;

		// The panel to dock. Created once, owned by the controller until the
		// window takes it; never null.
		QtLLM::ChatDockWidget* chatDock() const;

		// True when the provider is reachable and a model is resolved. The View
		// menu still shows the panel when this is false — the panel says what is
		// missing, which is more use than a greyed-out menu entry that does not.
		bool isReady() const;

		// §14d. Renders `prompt` as a user message and sends it. False when a
		// turn is already in flight or the text is blank; the caller reports
		// that rather than queueing, so two impatient clicks cannot stack two
		// paid turns.
		bool injectPrompt(const QString& prompt);

		// The QtLLM settings dialog — provider, model, tool enable/disable,
		// usage charts, and the live background agents. Opened from the chat
		// panel's own settings button and from the app's Settings dialog.
		void openSettings();

		// The prompts behind the app's LLM buttons. Separate from the buttons so
		// the wording lives in one place and is translated once. Each returns
		// text a human could have typed — that is the contract of §14d.
		static QString describePartPrompt(const Part& part, const QString& categoryName);
		static QString migratePartPrompt(const QString& partNumber);
		static QString kicadQuestionPrompt(const Part& part);

	signals:
		// Drives the buttons that must not fire twice, and the status line.
		void busyChanged(bool busy);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
