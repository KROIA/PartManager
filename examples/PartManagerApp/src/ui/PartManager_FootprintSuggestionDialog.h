// @file PartManager_FootprintSuggestionDialog.h
// @brief "You already have a footprint like this one" — offered when a download lands (§5a).
//
// The moment a vendor archive hands over a footprint is the moment sharing is
// cheap: the part has no footprint yet, so choosing an existing one costs
// nothing and saves the database another near-duplicate. Naming footprints after
// the package (`KicadLibraryGenerator::footprintNameFor`) made the duplicates
// *visible*; this is what stops them being created in the first place, one
// decision at a time rather than by a bulk cleanup afterwards.
//
// **The metric suggests, the user decides, the overlay is the evidence.**
// `FootprintCompatibility` ranks what is already in the database against the
// download, best first — by score alone, never filtered to a pass list, because
// on real data the margin between accepting and rejecting is hundredths of a
// millimetre. Every row carries its measurements for the same reason: a number
// the user can check beats a verdict they have to trust.
//
// **Accepting re-points this part's `part_file` row at the file the other part
// already uses** — `FileStore::useStoredFile()`, the same call the §5a collapse
// will make. Deliberately *not* "attach the existing bytes": that route runs
// through `detachFile()`, which deletes a stored file the moment its last row
// goes, and it was measured taking the filestore from 139 files to 138 by
// destroying the vendor original of a part that already had a footprint. The
// user's rule is that the files stay on disk, so nothing here deletes one.
// @see docs/design/ARCHITECTURE.md §5a, §5c
// @see PartManager_FootprintCompatibility.h, PartManager_KicadVariantView.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include "kicad/PartManager_FootprintCompatibility.h"
#include <QByteArray>
#include <QDialog>
#include <QString>
#include <vector>

class QTreeWidgetItem;

namespace Ui { class FootprintSuggestionDialog; }

namespace PartManager
{

	class KicadVariantView;

	class FootprintSuggestionDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `partId` is the part being given a footprint. It is excluded from the candidates: a
		// part cannot share with itself, and the vendor-archive path would otherwise find its
		// own freshly attached footprint, see bytes identical to the "download", and conclude
		// there was nothing to ask about.
		FootprintSuggestionDialog(DatabaseHandle* handle, int partId, const QByteArray& downloaded,
			const QString& downloadedName, QWidget* parent = nullptr);
		~FootprintSuggestionDialog() override;

		// False when there is nothing worth asking about: no other footprints in the database,
		// or the download is byte-identical to one that is already there — in which case the
		// content-addressed store shares it without anyone being asked.
		bool hasSuggestions() const { return !m_candidates.empty(); }

		// The part whose footprint the user chose to share, or 0 when they kept the download.
		// Any part using that footprint will do — they all point at one stored file.
		int chosenSourcePartId() const;

		// The whole flow in one call, for the attach paths. Shows nothing and returns false
		// when there is nothing to suggest or the user kept the download — the caller then
		// attaches the downloaded bytes as it always did. Returns **true** when an existing
		// footprint has already been attached to `partId` by reference, in which case the
		// caller must not attach anything.
		static bool offer(QWidget* parent, DatabaseHandle* handle, int partId,
			const QByteArray& downloaded, const QString& downloadedName);

		// The same offer for the vendor-ZIP paths (§5c), where the archive has *already* attached
		// its footprint by the time we get here. Reads the attached bytes back out of the store
		// and hands them to `offer()` as the "download", so the three ZIP entry points — the
		// editor's Import button, and the manual-download branch of either Fetch dialog — all ask
		// the same question. Does nothing when the archive brought no footprint.
		//
		// **Returns nothing, deliberately.** On the pre-attach paths `offer()`'s `true` means "do
		// not attach the download"; here there is no attach left to skip. Accepting re-points the
		// row through `FileStore::useStoredFile()`, declining leaves the import exactly as it was,
		// and either way the archive's own footprint stays on disk. The symbol and 3D model the
		// archive also brought are never touched — only the footprint slot is.
		static void offerAfterArchiveImport(QWidget* parent, const PartEditorController& controller,
			int partId, bool footprintAttached);

	private slots:
		void onSelectionChanged(QTreeWidgetItem* current);
		void onAccept();

	private:
		// One footprint already in the database, with how well it fits the download.
		struct Candidate
		{
			QString relativePath;
			QString contentHash;
			QString usedBy;           // part names, for the row and the status line
			// Any one of the parts that already use this footprint — the row whose stored file
			// the chosen part is pointed at. They all name the same file.
			int examplePartId = 0;
			KicadDrawing drawing;
			FootprintComparison comparison;
		};

		void buildCandidates();
		void showCandidate(int index);

		DatabaseHandle* m_handle = nullptr;
		int m_partId = 0;
		QByteArray m_downloaded;
		QString m_downloadedName;
		KicadDrawing m_downloadedDrawing;
		std::vector<Candidate> m_candidates;
		int m_chosen = -1;
		bool m_tookExisting = false;

		Ui::FootprintSuggestionDialog* m_ui;
		// Built in code rather than promoted in the .ui, as everywhere else this view is used.
		KicadVariantView* m_view = nullptr;
	};

}
