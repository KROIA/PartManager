#include "UnitTest_Gui.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPalette>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>

#include <iostream>

#ifdef Q_OS_WIN
	#include <windows.h>
#endif

namespace UnitTest
{
	namespace Gui
	{
		namespace
		{
			// QApplication keeps a reference to argc/argv for its whole lifetime, hence the statics.
			int g_argc = 1;
			char g_arg0[] = "unittest";
			char* g_argv[] = { g_arg0, nullptr };

			int g_stepDelayMs = 0;
			bool g_highlightEnabled = true;

			// A translucent frameless window laid over the widget about to be driven. An overlay, not
			// a stylesheet on the target: the widget under test may use its own stylesheet to mean
			// something (a red "negative stock" label, say), and a test must never overwrite that.
			void showMarker(QWidget* target, const QRect& localRect, int milliseconds)
			{
				if (!target || milliseconds <= 0 || !g_highlightEnabled)
				{
					return;
				}
				QWidget* marker = new QWidget(nullptr,
					Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
				marker->setAttribute(Qt::WA_TransparentForMouseEvents);
				marker->setAttribute(Qt::WA_ShowWithoutActivating);
				marker->setAttribute(Qt::WA_DeleteOnClose);
				marker->setAutoFillBackground(true);
				QPalette palette = marker->palette();
				palette.setColor(QPalette::Window, QColor(255, 96, 0));
				marker->setPalette(palette);
				marker->setWindowOpacity(0.35);

				const QRect rect = localRect.isValid() ? localRect : target->rect();
				marker->setGeometry(QRect(target->mapToGlobal(rect.topLeft()), rect.size()));
				marker->show();
				QTimer::singleShot(milliseconds, marker, &QWidget::close);
			}

			QString withoutAccelerator(const QString& text)
			{
				QString clean = text;
				clean.remove(QLatin1Char('&'));
				return clean.trimmed();
			}

			QWidget* findIn(QWidget* root, const QString& objectName)
			{
				if (!root)
				{
					return nullptr;
				}
				if (root->objectName() == objectName)
				{
					return root;
				}
				return root->findChild<QWidget*>(objectName);
			}

			// Letters map to their uppercase code point, which is what Qt uses. The event's text() is
			// what a QLineEdit actually inserts, so an imperfect key code is harmless.
			int keyForCharacter(QChar character)
			{
				return character.toUpper().unicode();
			}

			// Where a human would actually have to hit. The widget centre is wrong for a check box or
			// a radio button: a layout stretches them across the row, but only the indicator-plus-label
			// rect reacts, so a centred click lands in dead space and silently does nothing. Ask the
			// style for the real click rect instead of guessing.
			QPoint clickPoint(QWidget* target)
			{
				const bool isCheckBox = qobject_cast<QCheckBox*>(target) != nullptr;
				const bool isRadioButton = qobject_cast<QRadioButton*>(target) != nullptr;
				if (isCheckBox || isRadioButton)
				{
					QStyleOptionButton option;
					option.initFrom(target);
					if (QAbstractButton* button = qobject_cast<QAbstractButton*>(target))
					{
						option.text = button->text();
						option.icon = button->icon();
						option.iconSize = button->iconSize();
					}
					const QRect hit = target->style()->subElementRect(
						isCheckBox ? QStyle::SE_CheckBoxClickRect : QStyle::SE_RadioButtonClickRect,
						&option, target);
					if (hit.isValid() && !hit.isEmpty())
					{
						return hit.center();
					}
				}
				return target->rect().center();
			}

			bool isUsable(QWidget* target)
			{
				return target && target->isVisible() && target->isEnabled();
			}

			// Called by every action. Free when the delay is 0, which is the CI default.
			void step(QWidget* target, const QRect& localRect)
			{
				if (g_stepDelayMs <= 0)
				{
					return;
				}
				showMarker(target, localRect, g_stepDelayMs);
				// waitFor keeps pumping the event loop, so the marker paints and the widget under
				// test stays alive. A sleep here would freeze both.
				waitFor([]() { return false; }, g_stepDelayMs);
			}

			void sendMouse(QWidget* target, QEvent::Type type, const QPoint& local,
				Qt::MouseButton button, Qt::MouseButtons buttons)
			{
				QMouseEvent event(type, local, target->mapToGlobal(local), button, buttons, Qt::NoModifier);
				QApplication::sendEvent(target, &event);
			}

			// The widget that actually receives input for an item view: the viewport, not the frame.
			QWidget* inputTarget(QWidget* widget)
			{
				if (QAbstractItemView* view = qobject_cast<QAbstractItemView*>(widget))
				{
					return view->viewport();
				}
				return widget;
			}

			QString textOf(QWidget* widget)
			{
				if (QAbstractButton* button = qobject_cast<QAbstractButton*>(widget))
				{
					return button->text();
				}
				if (QLabel* label = qobject_cast<QLabel*>(widget))
				{
					return label->text();
				}
				if (QGroupBox* group = qobject_cast<QGroupBox*>(widget))
				{
					return group->title();
				}
				if (QLineEdit* edit = qobject_cast<QLineEdit*>(widget))
				{
					return edit->text();
				}
				return QString();
			}

			void dumpInto(QWidget* widget, int depth, QTextStream& stream)
			{
				if (!widget)
				{
					return;
				}
				stream << QString(depth * 2, QLatin1Char(' '))
					<< widget->metaObject()->className()
					<< " \"" << widget->objectName() << "\"";
				const QString text = textOf(widget);
				if (!text.isEmpty())
				{
					stream << " text=\"" << text << "\"";
				}
				if (!widget->isVisible())
				{
					stream << " [hidden]";
				}
				if (!widget->isEnabled())
				{
					stream << " [disabled]";
				}
				stream << "\n";
				for (QObject* child : widget->children())
				{
					dumpInto(qobject_cast<QWidget*>(child), depth + 1, stream);
				}
			}

			// Both trees and tables answer through the model, so one implementation covers every view.
			// QAbstractSpinBox::lineEdit() is protected, but the editor is a plain child widget.
			QLineEdit* editorOf(QWidget* spin)
			{
				return spin ? spin->findChild<QLineEdit*>() : nullptr;
			}

			// Commits a typed value the way clicking elsewhere does: focus out, which runs the
			// validator and fires editingFinished.
			//
			// NOT Return. Inside a QDialog, Return triggers the default button — so a spin box test
			// would close the very dialog it is filling in, and every later step would fail on a
			// widget that is no longer visible. Found exactly that way, against a real dialog.
			void commitEditor(QWidget* widget)
			{
				if (!widget)
				{
					return;
				}
				widget->clearFocus();
				QApplication::processEvents();
			}

			QModelIndex indexAt(QAbstractItemView* view, int row, int column)
			{
				if (!view || !view->model())
				{
					return QModelIndex();
				}
				return view->model()->index(row, column, view->rootIndex());
			}
		}

		void setStepDelay(int milliseconds)
		{
			g_stepDelayMs = milliseconds > 0 ? milliseconds : 0;
		}

		int stepDelay()
		{
			return g_stepDelayMs;
		}

		void setHighlightEnabled(bool enabled)
		{
			g_highlightEnabled = enabled;
		}

		bool highlightEnabled()
		{
			return g_highlightEnabled;
		}

		void narrate(const QString& message)
		{
			if (g_stepDelayMs <= 0)
			{
				return;
			}
			std::cout << "  ~ " << message.toStdString() << std::endl;
			waitFor([]() { return false; }, g_stepDelayMs);
		}

		bool ensureApplication()
		{
			if (qApp)
			{
				return true;
			}
			// QApplication aborts rather than returns when no GUI is available, so there is nothing
			// to check afterwards — a headless session fails here, loudly, which is honest.
			new QApplication(g_argc, g_argv);
			return qApp != nullptr;
		}

		bool isAvailable()
		{
			return qApp != nullptr && QApplication::primaryScreen() != nullptr;
		}

		bool showAndWait(QWidget* window, int timeoutMs)
		{
			if (!window)
			{
				return false;
			}
			window->show();
			// Mapped is not the same as shown: clicking before the platform window exists is a race
			// that only shows up on a loaded machine.
			return waitFor([window]() { return window->isVisible() && window->windowHandle() != nullptr; },
				timeoutMs);
		}

		QWidget* findWidget(const QString& objectName, QWidget* root)
		{
			if (root)
			{
				return findIn(root, objectName);
			}
			for (QWidget* window : QApplication::topLevelWidgets())
			{
				if (QWidget* found = findIn(window, objectName))
				{
					return found;
				}
			}
			return nullptr;
		}

		QWidget* findWidgetByText(const QString& text, QWidget* root)
		{
			const QString wanted = withoutAccelerator(text);
			QList<QWidget*> roots;
			if (root)
			{
				roots.append(root);
			}
			else
			{
				roots = QApplication::topLevelWidgets();
			}
			for (QWidget* start : roots)
			{
				if (withoutAccelerator(textOf(start)) == wanted)
				{
					return start;
				}
				for (QWidget* child : start->findChildren<QWidget*>())
				{
					if (withoutAccelerator(textOf(child)) == wanted)
					{
						return child;
					}
				}
			}
			return nullptr;
		}

		QAction* findAction(const QString& nameOrText, QWidget* root)
		{
			const QString wanted = withoutAccelerator(nameOrText);
			QList<QWidget*> roots;
			if (root)
			{
				roots.append(root);
			}
			else
			{
				roots = QApplication::topLevelWidgets();
			}
			for (QWidget* start : roots)
			{
				QList<QAction*> actions = start->findChildren<QAction*>();
				actions.append(start->actions());
				for (QAction* action : actions)
				{
					if (action->objectName() == nameOrText
						|| withoutAccelerator(action->text()) == wanted)
					{
						return action;
					}
				}
			}
			return nullptr;
		}

		QList<QWidget*> findChildrenOfClass(const QString& className, QWidget* root)
		{
			QList<QWidget*> result;
			QList<QWidget*> roots;
			if (root)
			{
				roots.append(root);
			}
			else
			{
				roots = QApplication::topLevelWidgets();
			}
			for (QWidget* start : roots)
			{
				if (start->inherits(className.toUtf8().constData()))
				{
					result.append(start);
				}
				for (QWidget* child : start->findChildren<QWidget*>())
				{
					if (child->inherits(className.toUtf8().constData()))
					{
						result.append(child);
					}
				}
			}
			return result;
		}

		bool clickAt(QWidget* target, const QPoint& localPos, Qt::MouseButton button)
		{
			if (!isUsable(target))
			{
				return false;
			}
			step(target, QRect(localPos - QPoint(12, 10), QSize(24, 20)));
			sendMouse(target, QEvent::MouseButtonPress, localPos, button, button);
			sendMouse(target, QEvent::MouseButtonRelease, localPos, button, Qt::NoButton);
			QApplication::processEvents();
			return true;
		}

		bool click(QWidget* target, Qt::MouseButton button)
		{
			if (!isUsable(target))
			{
				return false;
			}
			return clickAt(target, clickPoint(target), button);
		}

		bool rightClick(QWidget* target)
		{
			return click(target, Qt::RightButton);
		}

		bool doubleClickAt(QWidget* target, const QPoint& localPos)
		{
			if (!clickAt(target, localPos))
			{
				return false;
			}
			sendMouse(target, QEvent::MouseButtonDblClick, localPos, Qt::LeftButton, Qt::LeftButton);
			sendMouse(target, QEvent::MouseButtonRelease, localPos, Qt::LeftButton, Qt::NoButton);
			QApplication::processEvents();
			return true;
		}

		bool doubleClick(QWidget* target)
		{
			if (!isUsable(target))
			{
				return false;
			}
			return doubleClickAt(target, clickPoint(target));
		}

		bool drag(QWidget* target, const QPoint& fromLocal, const QPoint& toLocal, int steps)
		{
			return dragBetween(target, fromLocal, target, toLocal, steps);
		}

		bool dragBetween(QWidget* from, const QPoint& fromLocal, QWidget* to, const QPoint& toLocal,
			int steps)
		{
			if (!isUsable(from) || !isUsable(to))
			{
				return false;
			}
			const int stepCount = steps > 0 ? steps : 1;
			sendMouse(from, QEvent::MouseButtonPress, fromLocal, Qt::LeftButton, Qt::LeftButton);
			const QPoint globalFrom = from->mapToGlobal(fromLocal);
			const QPoint globalTo = to->mapToGlobal(toLocal);
			for (int step = 1; step <= stepCount; ++step)
			{
				// Interpolated moves, because a splitter or a slider integrates the movement rather
				// than reading the end point — one jump makes some widgets ignore the drag entirely.
				const QPoint global = globalFrom + (globalTo - globalFrom) * step / stepCount;
				QWidget* under = to;
				QMouseEvent move(QEvent::MouseMove, under->mapFromGlobal(global), global,
					Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
				QApplication::sendEvent(under, &move);
				QApplication::processEvents();
			}
			sendMouse(to, QEvent::MouseButtonRelease, toLocal, Qt::LeftButton, Qt::NoButton);
			QApplication::processEvents();
			return true;
		}

		bool hover(QWidget* target, const QPoint& localPos)
		{
			if (!isUsable(target))
			{
				return false;
			}
			QMouseEvent move(QEvent::MouseMove, localPos, target->mapToGlobal(localPos),
				Qt::NoButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(target, &move);
			QApplication::processEvents();
			return true;
		}

		bool wheel(QWidget* target, int deltaSteps)
		{
			if (!isUsable(target))
			{
				return false;
			}
			QWidget* receiver = inputTarget(target);
			const QPoint local = receiver->rect().center();
			const QPoint angle(0, deltaSteps * 120);   // 120 units per notch, as the platform reports
			QWheelEvent event(local, receiver->mapToGlobal(local), QPoint(), angle,
				Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
			QApplication::sendEvent(receiver, &event);
			QApplication::processEvents();
			return true;
		}

		bool type(QWidget* target, const QString& text)
		{
			if (!isUsable(target))
			{
				return false;
			}
			target->setFocus(Qt::OtherFocusReason);
			step(target, target->rect());
			for (const QChar character : text)
			{
				const int key = keyForCharacter(character);
				QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(character));
				QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, QString(character));
				QApplication::sendEvent(target, &press);
				QApplication::sendEvent(target, &release);
				if (g_stepDelayMs > 0)
				{
					// Letter by letter in slow motion, so the field is seen filling in rather than
					// blinking from empty to full.
					QApplication::processEvents();
					waitFor([]() { return false; }, g_stepDelayMs / 6 + 20);
				}
			}
			QApplication::processEvents();
			return true;
		}

		bool clearAndType(QWidget* target, const QString& text)
		{
			if (!isUsable(target))
			{
				return false;
			}
			target->setFocus(Qt::OtherFocusReason);
			if (!keyClick(target, Qt::Key_A, Qt::ControlModifier))
			{
				return false;
			}
			return type(target, text);
		}

		bool keyClick(QWidget* target, Qt::Key key, Qt::KeyboardModifiers modifiers)
		{
			if (!isUsable(target))
			{
				return false;
			}
			target->setFocus(Qt::OtherFocusReason);
			QKeyEvent press(QEvent::KeyPress, key, modifiers);
			QKeyEvent release(QEvent::KeyRelease, key, modifiers);
			QApplication::sendEvent(target, &press);
			QApplication::sendEvent(target, &release);
			QApplication::processEvents();
			return true;
		}

		bool keySequence(QWidget* target, const QKeySequence& sequence)
		{
			if (sequence.isEmpty())
			{
				return false;
			}
			const int combined = sequence[sequence.count() - 1];
			const Qt::KeyboardModifiers modifiers =
				static_cast<Qt::KeyboardModifiers>(combined & Qt::KeyboardModifierMask);
			const Qt::Key key = static_cast<Qt::Key>(combined & ~Qt::KeyboardModifierMask);
			return keyClick(target, key, modifiers);
		}

		bool setChecked(QAbstractButton* button, bool checked)
		{
			if (!button || button->isChecked() == checked)
			{
				return button != nullptr;
			}
			// Clicked, not setChecked(): anything listening to toggled/clicked has to run, or the
			// test proves nothing about what a user would experience.
			return click(button) && button->isChecked() == checked;
		}

		bool selectComboText(QComboBox* combo, const QString& itemText)
		{
			if (!isUsable(combo))
			{
				return false;
			}
			const int index = combo->findText(itemText);
			return index >= 0 && selectComboIndex(combo, index);
		}

		bool selectComboIndex(QComboBox* combo, int index)
		{
			if (!isUsable(combo) || index < 0 || index >= combo->count())
			{
				return false;
			}
			combo->setCurrentIndex(index);
			QApplication::processEvents();
			return combo->currentIndex() == index;
		}

		bool setSpinValue(QSpinBox* spin, int value)
		{
			if (!isUsable(spin))
			{
				return false;
			}
			// Through the editor, so the validator and editingFinished both run — setValue() skips
			// exactly the code a spin box test is there to cover.
			if (!clearAndType(editorOf(spin), QString::number(value)))
			{
				return false;
			}
			commitEditor(spin);
			return spin->value() == value;
		}

		bool setSpinValue(QDoubleSpinBox* spin, double value)
		{
			if (!isUsable(spin))
			{
				return false;
			}
			const QString text = QString::number(value, 'g', 10);
			if (!clearAndType(editorOf(spin), text))
			{
				return false;
			}
			commitEditor(spin);
			return true;
		}

		bool setSliderValue(QAbstractSlider* slider, int value)
		{
			if (!isUsable(slider))
			{
				return false;
			}
			slider->setValue(value);
			QApplication::processEvents();
			return slider->value() == value;
		}

		bool selectTab(QTabWidget* tabs, const QString& tabText)
		{
			if (!isUsable(tabs))
			{
				return false;
			}
			const QString wanted = withoutAccelerator(tabText);
			for (int index = 0; index < tabs->count(); ++index)
			{
				if (withoutAccelerator(tabs->tabText(index)) == wanted)
				{
					// Click the tab bar rather than setCurrentIndex(), so the bar's own handling runs.
					QTabBar* bar = tabs->tabBar();
					return bar && clickAt(bar, bar->tabRect(index).center())
						&& tabs->currentIndex() == index;
				}
			}
			return false;
		}

		bool triggerAction(QAction* action)
		{
			if (!action || !action->isEnabled())
			{
				return false;
			}
			action->trigger();
			QApplication::processEvents();
			return true;
		}

		bool triggerMenuPath(QWidget* menuBarOrWidget, const QStringList& path)
		{
			if (!menuBarOrWidget || path.isEmpty())
			{
				return false;
			}
			QList<QAction*> actions = menuBarOrWidget->actions();
			QAction* found = nullptr;
			for (int level = 0; level < path.size(); ++level)
			{
				const QString wanted = withoutAccelerator(path.at(level));
				found = nullptr;
				for (QAction* action : actions)
				{
					if (withoutAccelerator(action->text()) == wanted)
					{
						found = action;
						break;
					}
				}
				if (!found)
				{
					return false;
				}
				if (level + 1 < path.size())
				{
					QMenu* submenu = found->menu();
					if (!submenu)
					{
						return false;
					}
					actions = submenu->actions();
				}
			}
			return triggerAction(found);
		}

		int rowCount(QAbstractItemView* view)
		{
			return view && view->model() ? view->model()->rowCount(view->rootIndex()) : 0;
		}

		int columnCount(QAbstractItemView* view)
		{
			return view && view->model() ? view->model()->columnCount(view->rootIndex()) : 0;
		}

		QString cellText(QAbstractItemView* view, int row, int column)
		{
			const QModelIndex index = indexAt(view, row, column);
			return index.isValid() ? index.data(Qt::DisplayRole).toString() : QString();
		}

		int rowWithText(QAbstractItemView* view, const QString& text, int column)
		{
			const int rows = rowCount(view);
			const int columns = columnCount(view);
			for (int row = 0; row < rows; ++row)
			{
				if (column >= 0)
				{
					if (cellText(view, row, column) == text)
					{
						return row;
					}
					continue;
				}
				for (int col = 0; col < columns; ++col)
				{
					if (cellText(view, row, col) == text)
					{
						return row;
					}
				}
			}
			return -1;
		}

		bool clickCell(QAbstractItemView* view, int row, int column)
		{
			const QModelIndex index = indexAt(view, row, column);
			if (!isUsable(view) || !index.isValid())
			{
				return false;
			}
			// A cell scrolled out of sight has no valid rect, so the click would land on whatever
			// happens to be at (0,0) — scroll first, always.
			view->scrollTo(index);
			QApplication::processEvents();
			return clickAt(view->viewport(), view->visualRect(index).center());
		}

		bool doubleClickCell(QAbstractItemView* view, int row, int column)
		{
			const QModelIndex index = indexAt(view, row, column);
			if (!isUsable(view) || !index.isValid())
			{
				return false;
			}
			view->scrollTo(index);
			QApplication::processEvents();
			return doubleClickAt(view->viewport(), view->visualRect(index).center());
		}

		bool clickRowWithText(QAbstractItemView* view, const QString& text, int searchColumn)
		{
			const int row = rowWithText(view, text, searchColumn);
			return row >= 0 && clickCell(view, row, searchColumn >= 0 ? searchColumn : 0);
		}

		bool selectRow(QAbstractItemView* view, int row)
		{
			const QModelIndex index = indexAt(view, row, 0);
			if (!view || !index.isValid())
			{
				return false;
			}
			view->setCurrentIndex(index);
			view->selectionModel()->select(index,
				QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
			QApplication::processEvents();
			return true;
		}

		QTreeWidgetItem* findTreeItem(QTreeWidget* tree, const QStringList& textPath)
		{
			if (!tree || textPath.isEmpty())
			{
				return nullptr;
			}
			QTreeWidgetItem* current = nullptr;
			for (const QString& wanted : textPath)
			{
				QTreeWidgetItem* next = nullptr;
				const int count = current ? current->childCount() : tree->topLevelItemCount();
				for (int index = 0; index < count && !next; ++index)
				{
					QTreeWidgetItem* candidate = current ? current->child(index)
						: tree->topLevelItem(index);
					// startsWith, not ==: a tree label often carries a count suffix the caller should
					// not have to spell out, e.g. "MOSFET (2)".
					if (candidate->text(0) == wanted || candidate->text(0).startsWith(wanted + " ("))
					{
						next = candidate;
					}
				}
				if (!next)
				{
					return nullptr;
				}
				next->setExpanded(true);
				current = next;
			}
			return current;
		}

		bool expandTreeItem(QTreeWidget* tree, const QStringList& textPath)
		{
			QTreeWidgetItem* item = findTreeItem(tree, textPath);
			if (!item)
			{
				return false;
			}
			item->setExpanded(true);
			QApplication::processEvents();
			return true;
		}

		bool clickTreeItem(QTreeWidget* tree, const QStringList& textPath)
		{
			QTreeWidgetItem* item = findTreeItem(tree, textPath);
			if (!isUsable(tree) || !item)
			{
				return false;
			}
			tree->scrollToItem(item);
			QApplication::processEvents();
			const QRect rect = tree->visualItemRect(item);
			return !rect.isEmpty() && clickAt(tree->viewport(), rect.center());
		}

		bool clickListItem(QListWidget* list, const QString& itemText)
		{
			if (!isUsable(list))
			{
				return false;
			}
			const QList<QListWidgetItem*> matches = list->findItems(itemText, Qt::MatchExactly);
			if (matches.isEmpty())
			{
				return false;
			}
			list->scrollToItem(matches.first());
			QApplication::processEvents();
			return clickAt(list->viewport(), list->visualItemRect(matches.first()).center());
		}

		bool dragRow(QAbstractItemView* view, int fromRow, int toRow)
		{
			const QModelIndex from = indexAt(view, fromRow, 0);
			const QModelIndex to = indexAt(view, toRow, 0);
			if (!isUsable(view) || !from.isValid() || !to.isValid())
			{
				return false;
			}
			view->scrollTo(from);
			QApplication::processEvents();
			return drag(view->viewport(), view->visualRect(from).center(),
				view->visualRect(to).center(), 15);
		}

		bool waitFor(const std::function<bool()>& predicate, int timeoutMs)
		{
			QElapsedTimer timer;
			timer.start();
			while (timer.elapsed() < timeoutMs)
			{
				if (predicate && predicate())
				{
					return true;
				}
				QApplication::processEvents(QEventLoop::AllEvents, 10);
			}
			return predicate ? predicate() : false;
		}

		QWidget* waitForWidget(const QString& objectName, int timeoutMs)
		{
			QWidget* found = nullptr;
			waitFor([&found, &objectName]()
				{
					QWidget* candidate = findWidget(objectName);
					if (candidate && candidate->isVisible())
					{
						found = candidate;
						return true;
					}
					return false;
				}, timeoutMs);
			return found;
		}

		QWidget* waitForWindowOfClass(const QString& className, int timeoutMs)
		{
			QWidget* found = nullptr;
			waitFor([&found, &className]()
				{
					for (QWidget* window : QApplication::topLevelWidgets())
					{
						if (window->isVisible() && window->inherits(className.toUtf8().constData()))
						{
							found = window;
							return true;
						}
					}
					return false;
				}, timeoutMs);
			return found;
		}

		bool waitForClosed(QWidget* window, int timeoutMs)
		{
			QPointer<QWidget> guard(window);
			return waitFor([guard]() { return guard.isNull() || !guard->isVisible(); }, timeoutMs);
		}

		namespace
		{
			// Polls from the event loop rather than blocking: the caller is about to sit inside a
			// modal exec() and cannot run anything itself.
			void pollForWindow(const std::function<QWidget*()>& locate,
				const std::function<void(QWidget*)>& action, int timeoutMs)
			{
				QTimer* poller = new QTimer(qApp);
				const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
				poller->setInterval(25);
				QObject::connect(poller, &QTimer::timeout, poller, [poller, locate, action, deadline]()
					{
						QWidget* window = locate ? locate() : nullptr;
						if (window && window->isVisible())
						{
							poller->stop();
							poller->deleteLater();
							if (action)
							{
								action(window);
							}
							return;
						}
						if (QDateTime::currentMSecsSinceEpoch() > deadline)
						{
							poller->stop();
							poller->deleteLater();
						}
					});
				poller->start();
			}
		}

		void onNextWindow(const QString& objectName, const std::function<void(QWidget*)>& action,
			int timeoutMs)
		{
			pollForWindow([objectName]() { return findWidget(objectName); }, action, timeoutMs);
		}

		void onNextWindowOfClass(const QString& className, const std::function<void(QWidget*)>& action,
			int timeoutMs)
		{
			pollForWindow([className]() -> QWidget*
				{
					for (QWidget* window : QApplication::topLevelWidgets())
					{
						if (window->isVisible() && window->inherits(className.toUtf8().constData()))
						{
							return window;
						}
					}
					return nullptr;
				}, action, timeoutMs);
		}

		void onNextMessageBox(const QString& buttonText, int timeoutMs)
		{
			onNextWindowOfClass(QStringLiteral("QMessageBox"), [buttonText](QWidget* window)
				{
					QMessageBox* box = qobject_cast<QMessageBox*>(window);
					if (!box)
					{
						return;
					}
					const QString wanted = withoutAccelerator(buttonText);
					for (QAbstractButton* button : box->buttons())
					{
						if (withoutAccelerator(button->text()) == wanted)
						{
							click(button);
							return;
						}
					}
					// Nothing matched: close it anyway, or the test hangs on a dialog nobody can see.
					box->reject();
				}, timeoutMs);
		}

		bool closeWindow(QWidget* window)
		{
			if (!window)
			{
				return false;
			}
			const bool closed = window->close();
			QApplication::processEvents();
			return closed;
		}

		bool clickReal(QWidget* target)
		{
#ifdef Q_OS_WIN
			if (!moveCursorReal(target, clickPoint(target)))
			{
				return false;
			}
			mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
			mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
			// The injected event arrives through the window system, so the queue has to drain before
			// the caller can assert on anything.
			waitFor([] { return false; }, 150);
			return true;
#else
			Q_UNUSED(target);
			return false;   // only Windows so far; X11/Wayland/macOS need their own injection
#endif
		}

		bool doubleClickReal(QWidget* target)
		{
#ifdef Q_OS_WIN
			if (!clickReal(target))
			{
				return false;
			}
			mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
			mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
			waitFor([] { return false; }, 150);
			return true;
#else
			Q_UNUSED(target);
			return false;
#endif
		}

		bool dragReal(QWidget* target, const QPoint& fromLocal, const QPoint& toLocal, int steps)
		{
#ifdef Q_OS_WIN
			if (!moveCursorReal(target, fromLocal))
			{
				return false;
			}
			mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
			const QPoint globalFrom = target->mapToGlobal(fromLocal);
			const QPoint globalTo = target->mapToGlobal(toLocal);
			const int stepCount = steps > 0 ? steps : 1;
			for (int step = 1; step <= stepCount; ++step)
			{
				const QPoint global = globalFrom + (globalTo - globalFrom) * step / stepCount;
				SetCursorPos(global.x(), global.y());
				waitFor([] { return false; }, 10);
			}
			mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
			waitFor([] { return false; }, 150);
			return true;
#else
			Q_UNUSED(target); Q_UNUSED(fromLocal); Q_UNUSED(toLocal); Q_UNUSED(steps);
			return false;
#endif
		}

		bool moveCursorReal(QWidget* target, const QPoint& localPos)
		{
#ifdef Q_OS_WIN
			if (!isUsable(target))
			{
				return false;
			}
			QWidget* window = target->window();
			window->raise();
			window->activateWindow();
			QApplication::processEvents();
			const QPoint global = target->mapToGlobal(localPos);
			return SetCursorPos(global.x(), global.y()) != FALSE;
#else
			Q_UNUSED(target); Q_UNUSED(localPos);
			return false;
#endif
		}

		bool typeReal(const QString& text)
		{
#ifdef Q_OS_WIN
			for (const QChar character : text)
			{
				const SHORT scan = VkKeyScanW(character.unicode());
				if (scan == -1)
				{
					return false;   // not typable on this keyboard layout
				}
				const BYTE key = static_cast<BYTE>(scan & 0xFF);
				const bool shift = (scan >> 8) & 1;
				if (shift)
				{
					keybd_event(VK_SHIFT, 0, 0, 0);
				}
				keybd_event(key, 0, 0, 0);
				keybd_event(key, 0, KEYEVENTF_KEYUP, 0);
				if (shift)
				{
					keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
				}
				waitFor([] { return false; }, 20);
			}
			return true;
#else
			Q_UNUSED(text);
			return false;
#endif
		}

		bool saveScreenshot(QWidget* target, const QString& filePath)
		{
			if (!target)
			{
				return false;
			}
			return target->grab().save(filePath);
		}

		QString dumpWidgetTree(QWidget* root)
		{
			QString out;
			QTextStream stream(&out);
			if (root)
			{
				dumpInto(root, 0, stream);
			}
			else
			{
				for (QWidget* window : QApplication::topLevelWidgets())
				{
					dumpInto(window, 0, stream);
				}
			}
			return out;
		}
	}
}
