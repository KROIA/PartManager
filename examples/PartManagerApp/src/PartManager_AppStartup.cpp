#include "PartManager_AppStartup.h"
#include "settings/PartManager_Settings.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QPalette>
#include <QTranslator>
#include <QWheelEvent>

namespace PartManager
{

	void applyHighDpiAttributes()
	{
		QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
		QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
		QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
			Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
	}

	void repairDefaultUiFont(QApplication& app)
	{
		// No desktop UI font is anywhere near this small; Windows' own is 8.25pt.
		constexpr qreal SuspiciouslySmallPointSize = 6.0;

		QFont font = app.font();
		const qreal ratio = app.devicePixelRatio();
		if (ratio <= 1.0 || font.pointSizeF() <= 0.0
			|| font.pointSizeF() >= SuspiciouslySmallPointSize)
		{
			return;
		}
		font.setPointSizeF(font.pointSizeF() * ratio);
		app.setFont(font);
	}

	namespace
	{
		// See installScrollGuard(). Lives for the application's lifetime, parented to it.
		class ScrollGuard : public QObject
		{
		public:
			explicit ScrollGuard(QObject* parent) : QObject(parent) {}

		protected:
			bool eventFilter(QObject* watched, QEvent* event) override
			{
				if (event->type() != QEvent::Wheel)
				{
					return false;
				}
				// Only the two value editors. A combo box's *popup* is a QAbstractItemView and
				// not matched here, so an open drop-down still scrolls.
				if (!qobject_cast<QComboBox*>(watched) && !qobject_cast<QAbstractSpinBox*>(watched))
				{
					return false;
				}
				event->ignore();
				return true;
			}
		};
	}

	void installScrollGuard(QApplication& app)
	{
		app.installEventFilter(new ScrollGuard(&app));
	}

	namespace
	{
		// The palette Qt handed us at startup, so "System" can put back exactly what the platform
		// style produced rather than an approximation of it. Captured on the first applyTheme()
		// call, which is before anything has had a chance to change it.
		QPalette& systemPalette()
		{
			static QPalette palette;
			return palette;
		}

		bool& systemPaletteCaptured()
		{
			static bool captured = false;
			return captured;
		}

		// A dark palette built from Qt's own roles. Every widget in the app is a stock Qt widget
		// (RibbonWidget included), so setting these is enough — no per-widget stylesheet.
		QPalette darkPalette()
		{
			const QColor window(0x2D, 0x2D, 0x30);
			const QColor base(0x25, 0x25, 0x26);
			const QColor alternate(0x2A, 0x2A, 0x2C);
			const QColor text(0xE6, 0xE6, 0xE6);
			const QColor disabled(0x7F, 0x7F, 0x7F);
			const QColor highlight(0x0E, 0x63, 0x9C);

			QPalette palette;
			palette.setColor(QPalette::Window, window);
			palette.setColor(QPalette::WindowText, text);
			palette.setColor(QPalette::Base, base);
			palette.setColor(QPalette::AlternateBase, alternate);
			palette.setColor(QPalette::ToolTipBase, window);
			palette.setColor(QPalette::ToolTipText, text);
			palette.setColor(QPalette::Text, text);
			palette.setColor(QPalette::Button, window);
			palette.setColor(QPalette::ButtonText, text);
			palette.setColor(QPalette::BrightText, Qt::red);
			palette.setColor(QPalette::Link, highlight);
			palette.setColor(QPalette::Highlight, highlight);
			palette.setColor(QPalette::HighlightedText, Qt::white);
			// Without these a disabled control keeps the light theme's grey and becomes unreadable
			// on a dark background — the one thing a naive dark palette always gets wrong.
			palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
			palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
			palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
			return palette;
		}

		// An explicit light palette, so "Light" is light even on a system themed dark.
		QPalette lightPalette()
		{
			QPalette palette;
			palette.setColor(QPalette::Window, QColor(0xF0, 0xF0, 0xF0));
			palette.setColor(QPalette::WindowText, Qt::black);
			palette.setColor(QPalette::Base, Qt::white);
			palette.setColor(QPalette::AlternateBase, QColor(0xF7, 0xF7, 0xF7));
			palette.setColor(QPalette::ToolTipBase, Qt::white);
			palette.setColor(QPalette::ToolTipText, Qt::black);
			palette.setColor(QPalette::Text, Qt::black);
			palette.setColor(QPalette::Button, QColor(0xF0, 0xF0, 0xF0));
			palette.setColor(QPalette::ButtonText, Qt::black);
			palette.setColor(QPalette::Highlight, QColor(0x30, 0x8C, 0xC6));
			palette.setColor(QPalette::HighlightedText, Qt::white);
			return palette;
		}

		// The translator currently installed, so switching languages removes the old one first.
		// Two installed translators would leave the previous language winning for any string the
		// new one does not cover.
		QTranslator*& activeTranslator()
		{
			static QTranslator* translator = nullptr;
			return translator;
		}
	}

	void applyTheme(QApplication& app, const std::string& themeName)
	{
		if (!systemPaletteCaptured())
		{
			systemPalette() = app.palette();
			systemPaletteCaptured() = true;
		}

		if (themeName == ThemeName::Dark)
		{
			app.setPalette(darkPalette());
		}
		else if (themeName == ThemeName::Light)
		{
			app.setPalette(lightPalette());
		}
		else
		{
			// System, and anything a future version wrote that this build does not know.
			app.setPalette(systemPalette());
		}
	}

	bool applyLanguage(QApplication& app, const std::string& languageCode)
	{
		if (activeTranslator() != nullptr)
		{
			app.removeTranslator(activeTranslator());
			delete activeTranslator();
			activeTranslator() = nullptr;
		}
		// English is the source language: there is no .qm for it and installing nothing is
		// exactly right, not a failure.
		if (languageCode.empty() || languageCode == "en")
		{
			return true;
		}

		QTranslator* translator = new QTranslator();
		// Beside the executable and in a translations/ subfolder — where windeployqt and a
		// hand-built .qm respectively end up.
		const QString name = QStringLiteral("PartManager_") + QString::fromStdString(languageCode);
		const bool loaded = translator->load(name, app.applicationDirPath())
			|| translator->load(name, app.applicationDirPath() + QStringLiteral("/translations"));
		if (!loaded)
		{
			// No .qm yet is the normal state until §8's translations are actually built, so the
			// app stays in English rather than reporting an error the user cannot act on.
			delete translator;
			return false;
		}
		app.installTranslator(translator);
		activeTranslator() = translator;
		return true;
	}

}
