#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "controllers/PartManager_PartEditorController.h"
#include "database/PartManager_DatabaseHandle.h"
#include "filestore/PartManager_FileStore.h"
#include "kicad/PartManager_FootprintCompatibility.h"
#include "kicad/PartManager_KicadGeometry.h"
#include "persistence/PartManager_PartRepository.h"
#include "ui/PartManager_FootprintSuggestionDialog.h"

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <private/qzipwriter_p.h>

#include <filesystem>
#include <memory>
#include <string>

// §5a's suggestion on the **vendor-ZIP** path, which is the one the user actually walks: they
// download the archive themselves and hand it to PartManager. The offer is reachable from three
// buttons — the part editor's Import, and the manual-download branch of either Fetch dialog — and
// two of them did not call it at all, so the feature was invisible to exactly the flow it was
// built for. `FootprintSuggestionDialog::offerAfterArchiveImport()` is the one helper all three
// now go through, and this is what proves it: what the archive attached really is read back out
// of the filestore and compared, accepting re-points the row, declining changes nothing.
//
// Different from TST_FootprintCompatibility, which tests the metric on synthetic drawings. Here
// the footprints go in as bytes, through a real zip, through the importer, and come back out of
// the store — the half a metric test cannot see.
//
// Runs against a throwaway database in %TEMP%, never the user's own (ORIENTATION §6).
class TST_FootprintSuggestionArchive : public UnitTest::Test
{
	TEST_CLASS(TST_FootprintSuggestionArchive)
public:
	TST_FootprintSuggestionArchive()
		: Test("TST_FootprintSuggestionArchive")
	{
		ADD_TEST(TST_FootprintSuggestionArchive::anArchiveImportOffersTheFootprintAlreadyInTheDatabase);
		ADD_TEST(TST_FootprintSuggestionArchive::decliningLeavesTheImportExactlyAsItWas);
		ADD_TEST(TST_FootprintSuggestionArchive::anArchiveWithoutAFootprintAsksNothing);
		ADD_TEST(TST_FootprintSuggestionArchive::aDifferentPadCountIsNeverListed);
		ADD_TEST(TST_FootprintSuggestionArchive::nothingIsSuggestedWhenEveryCandidateIsFilteredOut);
		ADD_TEST(TST_FootprintSuggestionArchive::theSamePackageIsListedBeforeABetterScoringStranger);
	}

private:

	// A two-pad chip land pattern as a vendor writes it. `padWidth` is the only thing the two
	// fixtures differ by: 0.025 mm apart, which is inside FootprintTolerance::sizeMm and is the
	// real spread between two vendors' 0603 patterns — so the metric must call them compatible
	// while the bytes stay different, which is the case the dialog exists for.
	static std::string chipLand(const char* name, const char* padWidth)
	{
		return std::string("(footprint \"") + name + "\" (version 20221018) (layer \"F.Cu\")\n"
			"  (fp_line (start -1.5 -0.7) (end 1.5 -0.7)\n"
			"    (stroke (width 0.12) (type solid)) (layer \"F.SilkS\"))\n"
			"  (pad \"1\" smd rect (at -0.7875 0) (size " + padWidth + " 0.95)\n"
			"    (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n"
			"  (pad \"2\" smd rect (at 0.7875 0) (size " + padWidth + " 0.95)\n"
			"    (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n"
			")\n";
	}

	// Four pads where `chipLand` has two, in the same file dialect. Nothing a user can see in the
	// overlay turns this into a two-pad land pattern, which is why it must not be offered as one
	// — the fixture behind the pad-count filter.
	static std::string quadLand(const char* name)
	{
		return std::string("(footprint \"") + name + "\" (version 20221018) (layer \"F.Cu\")\n"
			"  (pad \"1\" smd rect (at -0.7875 -0.5) (size 0.9 0.5)\n"
			"    (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n"
			"  (pad \"2\" smd rect (at 0.7875 -0.5) (size 0.9 0.5)\n"
			"    (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n"
			"  (pad \"3\" smd rect (at 0.7875 0.5) (size 0.9 0.5)\n"
			"    (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n"
			"  (pad \"4\" smd rect (at -0.7875 0.5) (size 0.9 0.5)\n"
			"    (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n"
			")\n";
	}

	// The shape of a real Component Search Engine download (TST_EcadArchive's fixture), trimmed to
	// what the importer looks at. Built here rather than shipped, so no binary lands in the repo.
	static bool writeArchive(const std::filesystem::path& zipPath, const std::string* footprint)
	{
		QZipWriter writer(QString::fromStdString(zipPath.string()));
		if (!writer.isWritable()) { return false; }
		writer.addFile(QStringLiteral("RC0603/KiCad/RC0603.kicad_sym"),
			QByteArray("(kicad_symbol_lib (version 20241209)\n\t(symbol \"RC0603\"\n\t)\n)\n"));
		if (footprint != nullptr)
		{
			writer.addFile(QStringLiteral("RC0603/KiCad/RESC1608X55N.kicad_mod"),
				QByteArray(footprint->data(), static_cast<int>(footprint->size())));
		}
		writer.close();
		return true;
	}

	// One throwaway database folder per case, removed and recreated so a previous run cannot leak
	// into this one. Null on failure, reason in outError.
	static std::unique_ptr<PartManager::DatabaseHandle> makeDatabase(const std::string& name,
		std::filesystem::path& outFolder, std::string& outError)
	{
		outFolder = std::filesystem::temp_directory_path()
			/ ("PartManager_TST_FootprintSuggestionArchive_" + name);
		std::error_code ec;
		std::filesystem::remove_all(outFolder, ec);
		std::filesystem::create_directories(outFolder, ec);
		return PartManager::DatabaseHandle::createNew(outFolder.string(), "Suggestion", outError);
	}

	static int makePart(SQLiteWrapper::SQLite& db, const std::string& name,
		const std::string& package = "0603")
	{
		PartManager::Part part;
		part.name = name;
		part.package = package;
		return PartManager::PartRepository::insertPart(db, part);
	}

	// The candidates, which are the *children* of the group headers — the tree's top level is
	// the favourites/others grouping, so topLevelItemCount() counts headers, not choices.
	static int candidateRowCount(const QTreeWidget* tree)
	{
		int rows = 0;
		for (int i = 0; i < tree->topLevelItemCount(); ++i)
		{
			rows += tree->topLevelItem(i)->childCount();
		}
		return rows;
	}

	// The drawing behind a fixture, for asserting what the *metric* says independently of what
	// the dialog does with it.
	static PartManager::KicadDrawing drawingOf(const std::string& text)
	{
		return PartManager::KicadGeometry::footprint(text);
	}

	// Clicks one of the dialog's two buttons by role. Both are added in code, so there is no
	// objectName to look them up by — and matching their text would tie the test to the language
	// the test binary happens to start in.
	static bool clickByRole(QWidget* dialog, QDialogButtonBox::ButtonRole role)
	{
		QDialogButtonBox* box = dialog->findChild<QDialogButtonBox*>();
		if (box != nullptr)
		{
			for (QAbstractButton* button : box->buttons())
			{
				if (box->buttonRole(button) == role)
				{
					return UnitTest::Gui::click(button);
				}
			}
		}
		// Never leave the modal open: exec() would hold the whole suite forever, and a hang says
		// far less about what broke than a failed assertion does.
		if (QDialog* asDialog = qobject_cast<QDialog*>(dialog)) { asDialog->reject(); }
		return false;
	}

public:

	// The bug this suite was written for: a vendor ZIP attaches its own footprint, and the part
	// next door already has a compatible one. The offer has to appear, and accepting has to leave
	// both parts on a single stored file.
	TEST_FUNCTION(anArchiveImportOffersTheFootprintAlreadyInTheDatabase)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase("accept", folder, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		const int existing = makePart(db, "RC0603FR-074K7L");
		const int imported = makePart(db, "RC0603FR-0710KL");
		TEST_ASSERT(existing != 0 && imported != 0);

		const std::string held = chipLand("R_0603_1608Metric", "0.875");
		TEST_ASSERT_M(controller.attachRoleBytes(existing, PartManager::PartFileRole::KicadFootprint,
			held, "R_0603_1608Metric.kicad_mod", &error) != 0, "attach failed: " + error);

		const std::string downloaded = chipLand("RESC1608X55N", "0.9");
		const std::filesystem::path zipPath = folder / "LIB_RC0603FR-0710KL.zip";
		TEST_ASSERT_M(writeArchive(zipPath, &downloaded), "could not build the fixture archive");

		const PartManager::PartEditorController::EcadImportSummary summary =
			controller.importEcadArchive(imported, zipPath.string());
		TEST_ASSERT_M(summary.ok, "import failed: " + summary.errorMessage);
		TEST_ASSERT_M(summary.footprintAttached, "the archive's .kicad_mod was not attached");
		TEST_ASSERT_M(summary.symbolAttached, "the archive's .kicad_sym was not attached");

		PartManager::PartFile before;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, imported,
			PartManager::PartFileRole::KicadFootprint, before));
		PartManager::PartFile source;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, existing,
			PartManager::PartFileRole::KicadFootprint, source));
		TEST_ASSERT_M(before.contentHash != source.contentHash,
			"the fixtures must differ, or there would be nothing to suggest");

		// Registered before the blocking call, because exec() leaves the test nothing to run in.
		bool offered = false;
		int candidateRows = 0;
		bool clicked = false;
		UnitTest::Gui::onNextWindow(QStringLiteral("FootprintSuggestionDialog"),
			[&](QWidget* dialog)
			{
				offered = true;
				if (QTreeWidget* tree = dialog->findChild<QTreeWidget*>())
				{
					candidateRows = candidateRowCount(tree);
				}
				clicked = clickByRole(dialog, QDialogButtonBox::AcceptRole);
			}, 3000);
		// The net under the net: a database error inside offer() raises a warning box, and an
		// unanswered one would hold exec() open just as surely as the dialog would.
		UnitTest::Gui::onNextMessageBox(QStringLiteral("OK"), 2000);

		PartManager::FootprintSuggestionDialog::offerAfterArchiveImport(nullptr, controller,
			imported, summary.footprintAttached);

		TEST_ASSERT_M(offered, "the vendor-ZIP path did not offer the footprint next door");
		TEST_ASSERT_M(candidateRows == 1, "exactly the other part's footprint should be listed");
		TEST_ASSERT_M(clicked, "the dialog no longer has a button with AcceptRole");

		PartManager::PartFile after;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, imported,
			PartManager::PartFileRole::KicadFootprint, after));
		TEST_ASSERT_M(after.contentHash == source.contentHash,
			"accepting must point this part at the file the other one already uses");
		TEST_ASSERT_M(after.relativePath == source.relativePath, "same file, same path");

		// useStoredFile() deletes nothing — the archive's own footprint stays on disk, which is
		// the user's rule and the reason the sharing does not go through detachFile().
		const PartManager::FileStore store(handle->filestorePath());
		TEST_ASSERT_M(!store.absolutePath(before.relativePath).empty(),
			"the vendor original must survive being replaced in the slot");

		// The symbol and the 3D model the same archive brought are never touched.
		PartManager::PartFile symbol;
		TEST_ASSERT_M(PartManager::FileStore::roleFile(db, imported,
			PartManager::PartFileRole::KicadSymbol, symbol),
			"the suggestion must leave every slot but the footprint alone");

		// And the part that was shared *from* keeps its own row unchanged.
		PartManager::PartFile sourceAfter;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, existing,
			PartManager::PartFileRole::KicadFootprint, sourceAfter));
		TEST_ASSERT(sourceAfter.id == source.id);

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// The other half of the asymmetry the helper's header calls out: on this path the offer is
	// advisory, so "keep the downloaded one" must leave the import byte for byte as the importer
	// left it — there is no pending attach for a decline to release.
	TEST_FUNCTION(decliningLeavesTheImportExactlyAsItWas)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase("decline", folder, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		const int existing = makePart(db, "RC0603FR-074K7L");
		const int imported = makePart(db, "RC0603FR-0710KL");
		TEST_ASSERT(controller.attachRoleBytes(existing, PartManager::PartFileRole::KicadFootprint,
			chipLand("R_0603_1608Metric", "0.875"), "R_0603_1608Metric.kicad_mod", &error) != 0);

		const std::string downloaded = chipLand("RESC1608X55N", "0.9");
		const std::filesystem::path zipPath = folder / "LIB_RC0603FR-0710KL.zip";
		TEST_ASSERT(writeArchive(zipPath, &downloaded));
		const PartManager::PartEditorController::EcadImportSummary summary =
			controller.importEcadArchive(imported, zipPath.string());
		TEST_ASSERT_M(summary.footprintAttached, "import failed: " + summary.errorMessage);

		PartManager::PartFile before;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, imported,
			PartManager::PartFileRole::KicadFootprint, before));

		bool offered = false;
		UnitTest::Gui::onNextWindow(QStringLiteral("FootprintSuggestionDialog"),
			[&](QWidget* dialog)
			{
				offered = true;
				clickByRole(dialog, QDialogButtonBox::RejectRole);
			}, 3000);

		PartManager::FootprintSuggestionDialog::offerAfterArchiveImport(nullptr, controller,
			imported, summary.footprintAttached);

		TEST_ASSERT(offered);
		PartManager::PartFile after;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, imported,
			PartManager::PartFileRole::KicadFootprint, after));
		TEST_ASSERT_M(after.id == before.id && after.contentHash == before.contentHash,
			"declining must not touch the row the archive wrote");

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// An archive with no .kicad_mod in it. Nothing is shown — and the proof is that this case
	// returns at all: a dialog opened here would sit in exec() with nobody to answer it.
	TEST_FUNCTION(anArchiveWithoutAFootprintAsksNothing)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase("nofootprint", folder, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		// The database still holds a footprint, so the only reason not to ask is the summary.
		const int existing = makePart(db, "RC0603FR-074K7L");
		const int imported = makePart(db, "RC0603FR-0710KL");
		TEST_ASSERT(controller.attachRoleBytes(existing, PartManager::PartFileRole::KicadFootprint,
			chipLand("R_0603_1608Metric", "0.875"), "R_0603_1608Metric.kicad_mod", &error) != 0);

		const std::filesystem::path zipPath = folder / "LIB_symbol_only.zip";
		TEST_ASSERT(writeArchive(zipPath, nullptr));
		const PartManager::PartEditorController::EcadImportSummary summary =
			controller.importEcadArchive(imported, zipPath.string());
		TEST_ASSERT_M(summary.ok, "import failed: " + summary.errorMessage);
		TEST_ASSERT_M(!summary.footprintAttached, "the fixture archive has no footprint in it");

		bool offered = false;
		UnitTest::Gui::onNextWindow(QStringLiteral("FootprintSuggestionDialog"),
			[&offered](QWidget* dialog)
			{
				offered = true;
				if (QDialog* asDialog = qobject_cast<QDialog*>(dialog)) { asDialog->reject(); }
			}, 500);

		PartManager::FootprintSuggestionDialog::offerAfterArchiveImport(nullptr, controller,
			imported, summary.footprintAttached);
		// The poller above only runs from the event loop, which nothing entered — so give it the
		// chance it would have had, rather than passing because the timer never ticked.
		UnitTest::Gui::waitFor([]() { return false; }, 600);

		TEST_ASSERT_M(!offered, "an archive that brought no footprint must ask nothing");
		PartManager::PartFile none;
		TEST_ASSERT_M(!PartManager::FileStore::roleFile(db, imported,
			PartManager::PartFileRole::KicadFootprint, none),
			"nothing may end up in the footprint slot");

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// The three cases below build the dialog directly instead of going through `offer()`: they are
	// about what reaches the list, which is decided in the constructor, and a dialog that is never
	// shown needs nobody to answer it. The part being given a footprint has none yet — the
	// pre-attach path, and the one where the part is absent from `FootprintVariants::collect()`
	// entirely, so its package has to come from its own row.

	// Reported on real data: footprints "that don't even have the same pin counts" were being
	// offered. A different pad count is a different part and no overlay reading changes that, so
	// the row is not a decision the user can make — it must not be there at all.
	TEST_FUNCTION(aDifferentPadCountIsNeverListed)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase("padcount", folder, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		const int twoPad = makePart(db, "RC0603FR-074K7L");
		const int fourPad = makePart(db, "DFN-4-DUMMY");
		const int migrating = makePart(db, "RC0603FR-0710KL");
		TEST_ASSERT(twoPad != 0 && fourPad != 0 && migrating != 0);

		TEST_ASSERT_M(controller.attachRoleBytes(twoPad, PartManager::PartFileRole::KicadFootprint,
			chipLand("R_0603_1608Metric", "0.875"), "R_0603_1608Metric.kicad_mod", &error) != 0,
			"attach failed: " + error);
		TEST_ASSERT_M(controller.attachRoleBytes(fourPad, PartManager::PartFileRole::KicadFootprint,
			quadLand("DFN_4"), "DFN_4.kicad_mod", &error) != 0, "attach failed: " + error);

		const std::string downloaded = chipLand("RESC1608X55N", "0.9");
		PartManager::FootprintSuggestionDialog dialog(handle.get(), migrating,
			QByteArray(downloaded.data(), static_cast<int>(downloaded.size())),
			QStringLiteral("RESC1608X55N.kicad_mod"));

		TEST_ASSERT_M(dialog.hasSuggestions(), "the two-pad footprint next door is still a match");
		QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
		TEST_ASSERT(tree != nullptr);
		TEST_ASSERT_M(candidateRowCount(tree) == 1,
			"only the footprint with the same pad count may be listed");
		TEST_ASSERT_M(tree->topLevelItem(0)->child(0)->text(0) ==
			QStringLiteral("RC0603FR-074K7L"),
			"the row that survived must be the two-pad one, not the four-pad one");

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// What the filter made possible and nothing else could: a database that has footprints, all of
	// them the wrong shape. `hasSuggestions()` has to say no, or `offer()` shows an empty dialog.
	TEST_FUNCTION(nothingIsSuggestedWhenEveryCandidateIsFilteredOut)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase("allfiltered", folder, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		const int fourPad = makePart(db, "DFN-4-DUMMY");
		const int migrating = makePart(db, "RC0603FR-0710KL");
		TEST_ASSERT_M(controller.attachRoleBytes(fourPad, PartManager::PartFileRole::KicadFootprint,
			quadLand("DFN_4"), "DFN_4.kicad_mod", &error) != 0, "attach failed: " + error);

		const std::string downloaded = chipLand("RESC1608X55N", "0.9");
		const QByteArray bytes(downloaded.data(), static_cast<int>(downloaded.size()));
		{
			PartManager::FootprintSuggestionDialog dialog(handle.get(), migrating, bytes,
				QStringLiteral("RESC1608X55N.kicad_mod"));
			TEST_ASSERT_M(!dialog.hasSuggestions(),
				"a list emptied by the pad-count filter is not a suggestion");
			QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
			TEST_ASSERT(tree != nullptr && candidateRowCount(tree) == 0);
			TEST_ASSERT_M(tree->topLevelItemCount() == 0,
				"no candidates means no group headers either");
		}

		// And the path that matters: offer() must return without putting anything on screen.
		bool offered = false;
		UnitTest::Gui::onNextWindow(QStringLiteral("FootprintSuggestionDialog"),
			[&offered](QWidget* dialog)
			{
				offered = true;
				if (QDialog* asDialog = qobject_cast<QDialog*>(dialog)) { asDialog->reject(); }
			}, 500);
		const bool taken = PartManager::FootprintSuggestionDialog::offer(nullptr, handle.get(),
			migrating, bytes, QStringLiteral("RESC1608X55N.kicad_mod"));
		// The poller only runs from the event loop, which nothing entered — give it the chance it
		// would have had, rather than passing because the timer never ticked.
		UnitTest::Gui::waitFor([]() { return false; }, 600);

		TEST_ASSERT_M(!taken, "nothing was shown, so nothing can have been chosen");
		TEST_ASSERT_M(!offered, "an all-filtered list must not open a dialog with no rows in it");

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// The favourites group. The stranger here measures *better* than the same-package candidate —
	// identical pads against a 0.025 mm size difference — so score alone would list it first. The
	// grouping is what puts the 0603 on top, and the score still orders the rows inside a group.
	TEST_FUNCTION(theSamePackageIsListedBeforeABetterScoringStranger)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase("favourites", folder, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		const int samePackage = makePart(db, "RC0603FR-074K7L", "0603");
		const int stranger = makePart(db, "RC0805FR-0710KL", "0805");
		const int migrating = makePart(db, "RC0603FR-0710KL", "0603");
		TEST_ASSERT(samePackage != 0 && stranger != 0 && migrating != 0);

		const std::string favourite = chipLand("R_0603_1608Metric", "0.875");
		const std::string closer = chipLand("R_0805_2012Metric", "0.9");
		TEST_ASSERT_M(controller.attachRoleBytes(samePackage,
			PartManager::PartFileRole::KicadFootprint, favourite,
			"R_0603_1608Metric.kicad_mod", &error) != 0, "attach failed: " + error);
		TEST_ASSERT_M(controller.attachRoleBytes(stranger,
			PartManager::PartFileRole::KicadFootprint, closer,
			"R_0805_2012Metric.kicad_mod", &error) != 0, "attach failed: " + error);

		const std::string downloaded = chipLand("RESC1608X55N", "0.9");
		// The premise, asserted rather than assumed: without the grouping the stranger would be
		// row one. If this ever stops holding, the test below proves nothing.
		const PartManager::KicadDrawing downloadedDrawing = drawingOf(downloaded);
		const double favouriteScore = PartManager::FootprintCompatibility::compare(
			downloadedDrawing, drawingOf(favourite)).score;
		const double strangerScore = PartManager::FootprintCompatibility::compare(
			downloadedDrawing, drawingOf(closer)).score;
		TEST_ASSERT_M(strangerScore > favouriteScore,
			"the fixture must make the other package the better-scoring one");

		PartManager::FootprintSuggestionDialog dialog(handle.get(), migrating,
			QByteArray(downloaded.data(), static_cast<int>(downloaded.size())),
			QStringLiteral("RESC1608X55N.kicad_mod"));

		TEST_ASSERT(dialog.hasSuggestions());
		QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
		TEST_ASSERT(tree != nullptr);
		TEST_ASSERT_M(tree->topLevelItemCount() == 2, "one favourites group and one for the rest");
		TEST_ASSERT_M(candidateRowCount(tree) == 2, "both footprints have two pads, so both stay");

		QTreeWidgetItem* favourites = tree->topLevelItem(0);
		// The package name is user data and is never translated, so matching it does not tie the
		// test to the language the binary happens to start in.
		TEST_ASSERT_M(favourites->text(0).contains(QStringLiteral("0603")),
			"the first group must name the package it is the favourites of");
		TEST_ASSERT_M(favourites->isExpanded(), "a collapsed favourites group hides the point");
		TEST_ASSERT_M(favourites->childCount() == 1 &&
			favourites->child(0)->text(0) == QStringLiteral("RC0603FR-074K7L"),
			"the same-package footprint belongs in the favourites group");
		TEST_ASSERT_M(!favourites->flags().testFlag(Qt::ItemIsSelectable),
			"a group header is not a choice and must not be selectable");

		QTreeWidgetItem* others = tree->topLevelItem(1);
		TEST_ASSERT_M(others->childCount() == 1 &&
			others->child(0)->text(0) == QStringLiteral("RC0805FR-0710KL"),
			"the better-scoring stranger goes below, not above");

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}
};

TEST_INSTANTIATE(TST_FootprintSuggestionArchive);
