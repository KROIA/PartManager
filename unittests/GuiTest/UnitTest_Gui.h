// @file UnitTest_Gui.h
// @brief Widget-level GUI driving for the KROIA UnitTest framework — click and type without pixels.
//
// PROTOTYPE, meant to move to C:\Users\KRIA\Documents\Visual Studio 2022\Projects\UnitTest.
// It deliberately depends on nothing but QtWidgets (plus user32 on Windows for the real-input
// layer), so dropping it into that repository adds no new dependency to it.
//
// Two layers, and the difference matters:
//
//   * `click()` / `type()` — SYNTHETIC. Builds `QMouseEvent`/`QKeyEvent` and sends it straight to
//     the widget. No cursor moves, no window needs focus, nothing steals the user's desktop, and
//     it runs at full speed. This is what a GUI unit test should use.
//   * `clickReal()` / `typeReal()` — REAL. Asks the widget where it is (`mapToGlobal`), drives the
//     OS cursor there and injects a driver-level event. Slower, needs a visible focused window,
//     and can be disturbed by anything else on screen — but it proves the whole stack, including
//     that the widget is actually hittable and not covered by something else.
//
// Both layers resolve position from the widget at run time, so a layout change moves the click
// with the button. Nothing here stores a coordinate. Widgets are addressed by `objectName` — the
// name Designer already gives them — so renaming is the only thing that breaks a test.
//
// Modal dialogs block on `exec()`, so a test cannot simply call it and then interact. `onNextWindow()`
// registers what to do once the dialog appears, before the blocking call is made.
#pragma once

#include <QString>
#include <QWidget>

#include <functional>

class QApplication;

namespace UnitTest
{
	namespace Gui
	{
		// Creates the QApplication if the test binary does not have one yet. Safe to call repeatedly.
		// False means no GUI is available at all (headless session) — a test should then report itself
		// as skipped rather than failed.
		bool ensureApplication();

		// True when a GUI session exists and widgets can actually be shown.
		bool isAvailable();

		// The Designer name lookup. `root == nullptr` searches every top-level window, which is how
		// a dialog that opened itself is found.
		QWidget* findWidget(const QString& objectName, QWidget* root = nullptr);

		template<class T>
		T* find(const QString& objectName, QWidget* root = nullptr)
		{
			return qobject_cast<T*>(findWidget(objectName, root));
		}

		// --- synthetic input: fast, headless-friendly, does not touch the user's cursor ---

		// Sends press+release to the widget's centre. False when the widget is null, hidden or disabled —
		// "I clicked a button the user could not have clicked" is a test failure, not a pass.
		bool click(QWidget* target, Qt::MouseButton button = Qt::LeftButton);
		bool doubleClick(QWidget* target);
		// One key press/release per character, so validators and `textChanged` fire exactly as they
		// would for a human typist.
		bool type(QWidget* target, const QString& text);
		bool keyClick(QWidget* target, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

		// --- real input: proves the widget is genuinely reachable on screen ---

		// Raises and activates the widget's window, moves the OS cursor to the widget's centre and
		// injects a real click. Windows only for now; returns false elsewhere.
		bool clickReal(QWidget* target);
		// Types into whatever currently has focus, at driver level.
		bool typeReal(const QString& text);

		// --- waiting, without sleeps ---

		// Pumps the event loop until the predicate holds or the timeout expires.
		bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 2000);
		// Waits for a widget with this objectName to exist and be visible.
		QWidget* waitForWidget(const QString& objectName, int timeoutMs = 2000);

		// Runs `action` as soon as a window with `objectName` appears. Register it BEFORE the call
		// that opens a modal dialog — `exec()` will not return until the dialog is closed, and this
		// is what closes it. Does nothing if the window never appears.
		void onNextWindow(const QString& objectName, const std::function<void(QWidget*)>& action,
			int timeoutMs = 5000);

		// Only for diagnosing a failure — a passing GUI test should need no picture.
		bool saveScreenshot(QWidget* target, const QString& filePath);
	}
}
