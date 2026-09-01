// @file UnitTest_Gui.h
// @brief Widget-level GUI driving for the KROIA UnitTest framework — click and type without pixels.
//
// PROTOTYPE, meant to move to C:\Users\KRIA\Documents\Visual Studio 2022\Projects\UnitTest.
// It depends on nothing but QtWidgets (plus user32 on Windows for the real-input layer), so
// dropping it into that repository adds no new dependency to it.
//
// TWO LAYERS, and the difference matters:
//
//   * synthetic (`click`, `type`, `drag`, ...) — builds `QMouseEvent`/`QKeyEvent` and sends it
//     straight to the widget. No cursor moves, no window needs focus, nothing steals the user's
//     desktop, and it runs at full speed. This is what a GUI unit test should use.
//   * real (`clickReal`, `typeReal`, `dragReal`) — asks the widget where it is (`mapToGlobal`),
//     drives the OS cursor there and injects a driver-level event. Slower, needs a visible focused
//     window, disturbable by anything else on screen — but it proves the whole stack, including
//     that the widget is genuinely hittable and not covered by something else.
//
// LAYOUT-PROOF BY CONSTRUCTION. Widgets are addressed by `objectName` (the name Designer already
// gives them) or by visible text, and every position is resolved from the widget at run time.
// Nothing here stores a coordinate, so moving a button moves the click with it. Renaming a widget
// breaks a test — that is the trade, and it is the right one.
//
// MODAL DIALOGS block on `exec()`, so a test cannot call it and then interact. Register what should
// happen with `onNextWindow()` / `onNextMessageBox()` BEFORE making the blocking call.
//
// Every function returns false rather than asserting, so the calling test decides what a failure
// means. A hidden or disabled widget always refuses input: a test that "clicks" something the user
// could not have clicked is worse than no test at all.
#pragma once

#include <QKeySequence>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QAbstractButton;
class QAbstractItemView;
class QAbstractSlider;
class QAction;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace UnitTest
{
	namespace Gui
	{
		// ---------------------------------------------------------------- session

		// Creates the QApplication if the test binary does not have one yet. Safe to call repeatedly.
		bool ensureApplication();
		// True when a GUI session exists and widgets can actually be shown. A test should skip, not
		// fail, when this is false.
		bool isAvailable();
		// Shows a window and waits until it is actually mapped — `show()` alone is not enough to
		// click into it.
		bool showAndWait(QWidget* window, int timeoutMs = 2000);

		// ---------------------------------------------------------------- finding

		// `root == nullptr` searches every top-level window, which is how a dialog that opened
		// itself is found.
		QWidget* findWidget(const QString& objectName, QWidget* root = nullptr);

		template<class T>
		T* find(const QString& objectName, QWidget* root = nullptr)
		{
			return qobject_cast<T*>(findWidget(objectName, root));
		}

		// By what the user reads, for widgets nobody bothered to name. Matches a button's text, a
		// label's text, a group box title; '&' accelerators are ignored.
		QWidget* findWidgetByText(const QString& text, QWidget* root = nullptr);
		// Menu/toolbar actions, by objectName first, then by visible text.
		QAction* findAction(const QString& nameOrText, QWidget* root = nullptr);

		// Every widget of a type, in creation order — for "the third row of chips" cases.
		QList<QWidget*> findChildrenOfClass(const QString& className, QWidget* root = nullptr);

		// ---------------------------------------------------------------- synthetic input

		bool click(QWidget* target, Qt::MouseButton button = Qt::LeftButton);
		// A specific point inside the widget, in widget coordinates — item views and custom widgets
		// need this; everything else should use click().
		bool clickAt(QWidget* target, const QPoint& localPos, Qt::MouseButton button = Qt::LeftButton);
		bool doubleClick(QWidget* target);
		bool doubleClickAt(QWidget* target, const QPoint& localPos);
		bool rightClick(QWidget* target);
		// Press, move in steps, release — the shape a splitter, slider or header divider expects.
		bool drag(QWidget* target, const QPoint& fromLocal, const QPoint& toLocal, int steps = 10);
		// Across widgets (drag and drop between views).
		bool dragBetween(QWidget* from, const QPoint& fromLocal, QWidget* to, const QPoint& toLocal,
			int steps = 10);
		bool hover(QWidget* target, const QPoint& localPos);
		bool wheel(QWidget* target, int deltaSteps);

		// One key press/release per character, so validators and `textChanged` fire exactly as they
		// would for a human typist.
		bool type(QWidget* target, const QString& text);
		// Select-all then type — the honest way to replace the contents of a field.
		bool clearAndType(QWidget* target, const QString& text);
		bool keyClick(QWidget* target, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
		// e.g. QKeySequence("Ctrl+S"). Sends only the final chord.
		bool keySequence(QWidget* target, const QKeySequence& sequence);

		// ---------------------------------------------------------------- common widgets

		// Clicks the box rather than calling setChecked(), so anything listening to `toggled` runs.
		bool setChecked(QAbstractButton* button, bool checked);
		bool selectComboText(QComboBox* combo, const QString& itemText);
		bool selectComboIndex(QComboBox* combo, int index);
		// Types the value into the editor and commits it, which exercises the validator and any
		// editingFinished handler. setValue() would skip both.
		bool setSpinValue(QSpinBox* spin, int value);
		bool setSpinValue(QDoubleSpinBox* spin, double value);
		bool setSliderValue(QAbstractSlider* slider, int value);
		bool selectTab(QTabWidget* tabs, const QString& tabText);
		bool triggerAction(QAction* action);
		// Walks a menu path by visible text, e.g. {"File", "Export", "CSV..."}.
		bool triggerMenuPath(QWidget* menuBarOrWidget, const QStringList& path);

		// ---------------------------------------------------------------- item views

		// Row/column lookups by what is on screen, so a test never hard-codes an index it cannot see.
		int rowWithText(QAbstractItemView* view, const QString& text, int column = -1);
		QString cellText(QAbstractItemView* view, int row, int column);
		int rowCount(QAbstractItemView* view);
		int columnCount(QAbstractItemView* view);

		// Scrolls the cell into view, then clicks its centre. Works for QTableView/QTableWidget,
		// QListView and QTreeView alike, because it goes through the model index, not the widget.
		bool clickCell(QAbstractItemView* view, int row, int column = 0);
		bool doubleClickCell(QAbstractItemView* view, int row, int column = 0);
		bool clickRowWithText(QAbstractItemView* view, const QString& text, int searchColumn = -1);
		bool selectRow(QAbstractItemView* view, int row);

		// Trees by visible text path, e.g. {"Transistor", "MOSFET"} — expands what it must.
		QTreeWidgetItem* findTreeItem(QTreeWidget* tree, const QStringList& textPath);
		bool clickTreeItem(QTreeWidget* tree, const QStringList& textPath);
		bool expandTreeItem(QTreeWidget* tree, const QStringList& textPath);
		bool clickListItem(QListWidget* list, const QString& itemText);

		// Drags one row onto another — reorderable lists (InternalMove) need real drag geometry.
		bool dragRow(QAbstractItemView* view, int fromRow, int toRow);

		// ---------------------------------------------------------------- windows and dialogs

		// Pumps the event loop until the predicate holds or the timeout expires. Never sleep in a
		// GUI test; sleeping blocks the very loop the widget needs to react.
		bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 2000);
		QWidget* waitForWidget(const QString& objectName, int timeoutMs = 2000);
		QWidget* waitForWindowOfClass(const QString& className, int timeoutMs = 2000);
		bool waitForClosed(QWidget* window, int timeoutMs = 2000);

		// Runs `action` as soon as a matching window appears. Register BEFORE the blocking call that
		// opens it. Does nothing if the window never appears.
		void onNextWindow(const QString& objectName, const std::function<void(QWidget*)>& action,
			int timeoutMs = 5000);
		void onNextWindowOfClass(const QString& className, const std::function<void(QWidget*)>& action,
			int timeoutMs = 5000);
		// The common case: a QMessageBox nobody named. `buttonText` is matched loosely ("OK", "Yes").
		void onNextMessageBox(const QString& buttonText, int timeoutMs = 5000);

		bool closeWindow(QWidget* window);

		// ---------------------------------------------------------------- real input

		// Raises and activates the widget's window, then injects driver-level events at the widget's
		// own screen position. Windows only for now; returns false elsewhere.
		bool clickReal(QWidget* target);
		bool doubleClickReal(QWidget* target);
		bool dragReal(QWidget* target, const QPoint& fromLocal, const QPoint& toLocal, int steps = 20);
		// Types into whatever currently has focus.
		bool typeReal(const QString& text);
		bool moveCursorReal(QWidget* target, const QPoint& localPos);

		// ---------------------------------------------------------------- diagnostics

		// Only for diagnosing a failure — a passing GUI test needs no picture.
		bool saveScreenshot(QWidget* target, const QString& filePath);
		// The single most useful thing to print when a lookup fails: what the tree actually contains.
		QString dumpWidgetTree(QWidget* root = nullptr);
	}
}
