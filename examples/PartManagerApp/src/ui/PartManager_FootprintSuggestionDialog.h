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
// **A part that already has one is asked about on demand instead** — the part
// editor's "Use a shared footprint…" button, `Trigger::UserRequested`. Every
// part imported before this feature existed carries its own private copy and
// would never be offered anything, because the only moments that asked were the
// ones that no longer happen to it. See `Trigger` for the two behaviours that
// differ; everything else — the metric, the ordering, the overlay, the re-point
// — is the same question asked from a different place.
//
// **The metric suggests, the user decides, the overlay is the evidence.**
// `FootprintCompatibility` ranks what is already in the database against the
// download, best first — by score alone, never filtered to a pass list, because
// on real data the margin between accepting and rejecting is hundredths of a
// millimetre. Every row carries its measurements for the same reason: a number
// the user can check beats a verdict they have to trust.
//
// **The one exception is the pad count**, which is not a margin but a count: a
// candidate with a different number of pads is dropped before it reaches the
// list. Nothing in the overlay can turn a two-pad land pattern into a four-pad
// one, so such a row is a decision the user is not able to make — reported on
// real data, where they were the bulk of what was offered.
//
// **Same-package candidates are listed first, as a favourites group.** A part
// being given a footprint nearly always wants one from its own package, and the
// score alone scattered those among strangers that happened to measure closer.
// The ordering *within* each group is still the score, untouched.
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

class QPushButton;
class QTreeWidgetItem;

namespace Ui { class FootprintSuggestionDialog; }

namespace PartManager
{

	class KicadVariantView;

	class FootprintSuggestionDialog : public QDialog
	{
		Q_OBJECT
	public:
		// Which path opened the dialog. Two things follow from it, and both are about what an
		// answer of "nothing" is allowed to mean.
		enum class Trigger
		{
			// A download or a vendor ZIP just arrived and the reference is those bytes. A
			// candidate that is byte-identical ends the search: the content-addressed store
			// will share that file on its own the moment the bytes are attached, so there is
			// genuinely nothing to ask, and an empty result is answered by attaching as usual.
			AfterImport,
			// The user pressed the button, and the reference is the footprint the part is
			// already carrying. A byte-identical candidate is now the *one* thing that cannot
			// be a different footprint, so it is skipped and the rest of the list survives —
			// and an empty result is a sentence the caller owes them, see `emptyReason()`.
			UserRequested
		};

		// What a user-requested offer ended in. The four "nothing to show" answers are separate
		// values because the call site has to say *which* one it was: on this path the user
		// pressed a button and a dialog that simply never appears is the bug they already hit.
		enum class Outcome
		{
			Replaced,             // the part's row now names the file another part already uses
			Declined,             // the user kept what they had, or the re-point was refused
			NoFootprintAttached,  // nothing to measure against — the button should have been off
			ReferenceUnreadable,  // the attached file holds no pad geometry this can compare
			NoOtherFootprints,    // no other part in this database has a footprint at all
			OnlyIdenticalOnes,    // the others are all the same stored file this part uses
			AllFilteredOut        // every other footprint has a different pad count
		};

		// `partId` is the part being given a footprint. It is excluded from the candidates: a
		// part cannot share with itself, and the vendor-archive path would otherwise find its
		// own freshly attached footprint, see bytes identical to the "download", and conclude
		// there was nothing to ask about.
		//
		// `reference` is what the candidates are measured against — the arriving bytes on
		// `AfterImport`, the part's current footprint on `UserRequested`. The default keeps the
		// three import call sites reading as they did.
		FootprintSuggestionDialog(DatabaseHandle* handle, int partId, const QByteArray& reference,
			const QString& referenceName, Trigger trigger = Trigger::AfterImport,
			QWidget* parent = nullptr);
		~FootprintSuggestionDialog() override;

		// False when there is nothing worth asking about: no other footprints in the database,
		// none with the same pad count, or — on `AfterImport` only — one that is byte-identical
		// to the download, which the content-addressed store shares without anyone being asked.
		bool hasSuggestions() const { return !m_candidates.empty(); }

		// Why the list is empty. Only meaningful while `hasSuggestions()` is false, and only
		// the `UserRequested` path reads it: `AfterImport` treats every empty result the same
		// way, by attaching what arrived.
		Outcome emptyReason() const { return m_emptyReason; }

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

		// The user-requested counterpart, for the part editor's button. Reads the footprint the
		// part carries *now* as the reference and asks the same question about it.
		//
		// **Returns the outcome rather than a bool**, because the caller has to speak when the
		// answer is nothing: the dialog keeps its "I have nothing to say, so I say nothing"
		// contract, and the part editor turns each empty reason into the sentence that explains
		// it. Nothing is deleted here either — accepting re-points the row and the footprint the
		// part used before stays on disk, exactly as on the import paths.
		static Outcome offerReplacement(QWidget* parent, const PartEditorController& controller,
			int partId);

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
			// The package the parts using this footprint carry — `FootprintVariants::group()`
			// buckets by it, so every part behind one candidate names the same one. Compared
			// against this part's own package to decide which group the row belongs to.
			QString package;
			// Any one of the parts that already use this footprint — the row whose stored file
			// the chosen part is pointed at. They all name the same file.
			int examplePartId = 0;
			KicadDrawing drawing;
			FootprintComparison comparison;
		};

		void buildCandidates();
		// One top-level node and the candidate rows under it, by index into `m_candidates`.
		void addCandidateGroup(const QString& title, const std::vector<size_t>& indices);
		// The first row a user can actually choose. Not `topLevelItem(0)` — that is a group
		// header, which is deliberately unselectable.
		QTreeWidgetItem* firstCandidateItem() const;
		void showCandidate(int index);

		// The footprint `partId` carries right now, as bytes plus the name to show for it. False
		// when the slot is empty. Shared by the two static entry points so the read-back — the
		// row, its path, the whole file — is written once.
		static bool readAttachedFootprint(const PartEditorController& controller, int partId,
			QByteArray& outBytes, QString& outName);
		// Points `partId`'s footprint row at the file `sourcePartId` uses. False with a warning
		// already shown when the database refused it.
		static bool applyChosen(QWidget* parent, DatabaseHandle* handle, int partId,
			int sourcePartId);

		DatabaseHandle* m_handle = nullptr;
		int m_partId = 0;
		Trigger m_trigger = Trigger::AfterImport;
		QByteArray m_reference;
		QString m_referenceName;
		// Starts at the answer that holds before a single candidate has been seen, and is
		// narrowed as `buildCandidates()` gets further. Never read while there are candidates.
		Outcome m_emptyReason = Outcome::NoOtherFootprints;
		// Read from the part's own row, not from `FootprintVariants::collect()`: on the
		// pre-attach path this part has no footprint yet, so collect() never lists it and there
		// would be nothing to match the candidates' packages against. Empty is a real answer —
		// a part with no package has no favourites.
		QString m_partPackage;
		KicadDrawing m_referenceDrawing;
		std::vector<Candidate> m_candidates;
		int m_chosen = -1;
		bool m_tookExisting = false;

		Ui::FootprintSuggestionDialog* m_ui;
		// Enabled only while a candidate row is current, so a group header cannot be accepted.
		QPushButton* m_useExisting = nullptr;
		// Built in code rather than promoted in the .ui, as everywhere else this view is used.
		KicadVariantView* m_view = nullptr;
	};

}
