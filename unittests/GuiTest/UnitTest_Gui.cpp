#include "UnitTest_Gui.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QRadioButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QDateTime>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QTimer>

#ifdef Q_OS_WIN
	#include <windows.h>
#endif

namespace UnitTest
{
	namespace Gui
	{
		namespace
		{
			// The QApplication a test binary may not have created itself. Static storage, because
			// QApplication keeps a reference to argc/argv for its whole lifetime.
			int g_argc = 1;
			char g_arg0[] = "unittest";
			char* g_argv[] = { g_arg0, nullptr };
			QApplication* g_ownedApplication = nullptr;

			// Recursive objectName lookup. Qt's own findChild() would do this for one root, but a
			// dialog that opened itself is not a child of anything the test holds.
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

			// Qt::Key for a printable character. Letters map to their uppercase code point, which is
			// what Qt uses; anything else falls back to the raw code point. The event's text() is what
			// a QLineEdit actually inserts, so an imperfect key code is harmless.
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
		}

		bool ensureApplication()
		{
			if (qApp)
			{
				return true;
			}
			// QApplication aborts rather than returns when no GUI is available, so there is nothing
			// to check afterwards — a headless session fails here, loudly, which is honest.
			g_ownedApplication = new QApplication(g_argc, g_argv);
			return qApp != nullptr;
		}

		bool isAvailable()
		{
			return qApp != nullptr && QApplication::primaryScreen() != nullptr;
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

		bool click(QWidget* target, Qt::MouseButton button)
		{
			if (!isUsable(target))
			{
				return false;
			}
			const QPoint local = clickPoint(target);
			const QPoint global = target->mapToGlobal(local);

			QMouseEvent press(QEvent::MouseButtonPress, local, global, button, button, Qt::NoModifier);
			QMouseEvent release(QEvent::MouseButtonRelease, local, global, button, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(target, &press);
			QApplication::sendEvent(target, &release);
			QApplication::processEvents();
			return true;
		}

		bool doubleClick(QWidget* target)
		{
			if (!click(target))
			{
				return false;
			}
			const QPoint local = clickPoint(target);
			const QPoint global = target->mapToGlobal(local);
			QMouseEvent doubleClickEvent(QEvent::MouseButtonDblClick, local, global,
				Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QMouseEvent release(QEvent::MouseButtonRelease, local, global,
				Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(target, &doubleClickEvent);
			QApplication::sendEvent(target, &release);
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
			for (const QChar character : text)
			{
				const int key = keyForCharacter(character);
				QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(character));
				QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, QString(character));
				QApplication::sendEvent(target, &press);
				QApplication::sendEvent(target, &release);
			}
			QApplication::processEvents();
			return true;
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

		bool clickReal(QWidget* target)
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

			const QPoint global = target->mapToGlobal(clickPoint(target));
			if (!SetCursorPos(global.x(), global.y()))
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

		void onNextWindow(const QString& objectName, const std::function<void(QWidget*)>& action,
			int timeoutMs)
		{
			// Polls from the event loop rather than blocking, which is the whole point: the caller is
			// about to sit inside a modal exec() and cannot run anything itself.
			QTimer* poller = new QTimer(qApp);
			const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
			poller->setInterval(25);
			QObject::connect(poller, &QTimer::timeout, poller, [poller, objectName, action, deadline]()
				{
					QWidget* window = findWidget(objectName);
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

		bool saveScreenshot(QWidget* target, const QString& filePath)
		{
			if (!target)
			{
				return false;
			}
			return target->grab().save(filePath);
		}
	}
}
