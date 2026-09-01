#include "PartManager_AppStartup.h"

#include <QApplication>
#include <QFont>

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

}
