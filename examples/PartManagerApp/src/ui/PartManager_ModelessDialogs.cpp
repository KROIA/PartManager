#include "ui/PartManager_ModelessDialogs.h"

#include <QDialog>
#include <QHash>

namespace PartManager
{
	namespace
	{
		// Function-local rather than a file-scope object, so it cannot be used before it is
		// built and the one thing keeping the "one window per screen" promise sits in one place.
		QHash<QString, QDialog*>& openDialogs()
		{
			static QHash<QString, QDialog*> dialogs;
			return dialogs;
		}
	}

	QDialog* ModelessDialogs::raise(const QString& key)
	{
		const auto existing = openDialogs().constFind(key);
		if (existing == openDialogs().constEnd())
		{
			return nullptr;
		}

		QDialog* dialog = *existing;
		// A minimised window is still an open one, and raise() on a minimised window leaves it
		// minimised — the user would press the ribbon button and watch nothing happen.
		if (dialog->isMinimized())
		{
			dialog->showNormal();
		}
		dialog->raise();
		dialog->activateWindow();
		return dialog;
	}

	void ModelessDialogs::show(const QString& key, QDialog* dialog)
	{
		if (dialog == nullptr)
		{
			return;
		}

		dialog->setAttribute(Qt::WA_DeleteOnClose);
		openDialogs().insert(key, dialog);
		// destroyed(), not finished(): a dialog also goes when its parent goes, and an entry
		// left behind either way would have the next open raising a dangling pointer.
		QObject::connect(dialog, &QObject::destroyed, [key, dialog]()
			{
				// Matched on the pointer as well as the key: closeAll() may have cleared the
				// table already, and a later dialog may have claimed the same key since.
				if (openDialogs().value(key) == dialog)
				{
					openDialogs().remove(key);
				}
			});
		dialog->show();
	}

	void ModelessDialogs::closeAll()
	{
		// Deleted, not close()d: WA_DeleteOnClose defers the delete to the event loop, and the
		// caller is on its way out of one — anything these dialogs write on the way down has to
		// have been written by the time this returns. The table is cleared first because every
		// delete below runs the destroyed() handler in show().
		const QList<QDialog*> dialogs = openDialogs().values();
		openDialogs().clear();
		qDeleteAll(dialogs);
	}

}
