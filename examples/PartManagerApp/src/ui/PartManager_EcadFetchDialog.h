// @file PartManager_EcadFetchDialog.h
// @brief "Get the KiCad symbol and footprint for this part" (§5c) — automatic first, manual after.
//
// Two routes to the same two files, in the order that costs the user least:
//
//   1. **EasyEDA/LCSC**, automatic and keyless. The only ECAD source with an
//      open API — SamacSys (what Mouser's own ECAD button calls) publishes none,
//      and SnapEDA gates theirs behind a request form, so neither can be
//      automated no matter what credentials are held. Coverage is LCSC's
//      catalogue, which is excellent for jellybean parts and thin for
//      Würth/TE/Molex.
//   2. **A vendor ZIP the user downloads by hand**, which is where route 1's
//      misses land. Mouser's product page is behind a JavaScript bot challenge
//      so the app cannot fetch the ZIP itself, but it can open the page, watch
//      the Downloads folder, and recognise the archive when it lands —
//      which is the whole of the manual work minus the file picker.
//
// The user sees the symbol and the footprint drawn before attaching either.
// A converted footprint with a wrong land pattern is expensive on a board, and
// looking at it is the check no assertion replaces — the same reason the
// conversion reports every shape it could not translate rather than dropping it
// quietly.
//
// Produces bytes; it attaches nothing itself. The caller owns the part.
// @see docs/design/ARCHITECTURE.md §5a, §5c
// @see PartManager_EasyEdaClient.h, PartManager_EasyEdaConverter.h, PartManager_EcadArchive.h
#pragma once

#include <QDateTime>
#include <QDialog>
#include <QString>

class QFileSystemWatcher;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace PartManager
{

	class KicadPreviewWidget;

	class EcadFetchDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `mouserUrl` may be empty — the "Open on Mouser" button is then hidden, and the manual
		// route is the file picker plus the Downloads watcher.
		EcadFetchDialog(const QString& mpn, const QString& manufacturer, const QString& mouserUrl,
			QWidget* parent = nullptr);
		~EcadFetchDialog() override;

		// What Attach accepted. Both may be empty — a source that only had one of the two is a
		// normal outcome, and half a component still beats none.
		QByteArray symbolBytes() const;
		QString symbolFilename() const;
		QByteArray footprintBytes() const;
		QString footprintFilename() const;
		// Set instead of the two above when the result came from a vendor ZIP, because that route
		// also carries a 3D model and the caller imports the archive whole.
		QString archivePath() const;

	private slots:
		void openOnMouser();
		void chooseArchive();
		void scanDownloads();

	private:
		void startLookup();
		void showConversion();
		void showArchive(const QString& zipPath);
		void beginWatchingDownloads();
		void updateButtons();

		struct Private;
		Private* d;
	};

}
