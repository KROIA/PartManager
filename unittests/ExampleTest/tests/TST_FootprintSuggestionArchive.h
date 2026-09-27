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
#include <QStringList>
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
		ADD_TEST(TST_FootprintSuggestionArchive::otherPackagesAreSplitIntoOneNodePerPackage);
		ADD_TEST(TST_FootprintSuggestionArchive::arrowKeysStepFromLeafToLeafOnly);
		ADD_TEST(TST_FootprintSuggestionArchive::aUserRequestedOfferKeepsTheCandidatesAnIdenticalOneWouldHaveWiped);
		ADD_TEST(TST_FootprintSuggestionArchive::anImportOfferStaysSilentOnTheSameDatabase);
		ADD_TEST(TST_FootprintSuggestionArchive::aUserRequestedOfferSaysWhyItFoundNothing);
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

	// The candidates, which are the tree's *leaves*. Counted recursively rather than as the
	// headers' children, because "Other packages" now holds a node per package and its children
	// are those nodes, not choices — and the favourites group is still two levels deep.
	static int candidateRowCount(const QTreeWidgetItem* item)
	{
		if (item->childCount() == 0) { return 1; }
		int rows = 0;
		for (int i = 0; i < item->childCount(); ++i)
		{
			rows += candidateRowCount(item->child(i));
		}
		return rows;
	}

	static int candidateRowCount(const QTreeWidget* tree)
	{
		int rows = 0;
		for (int i = 0; i < tree->topLevelItemCount(); ++i)
		{
			rows += candidateRowCount(tree->topLevelItem(i));
		}
		return rows;
	}

	// Every leaf in the order the user walks them with the Down key: depth first, groups in tree
	// order. What the arrow keys must reproduce and what nothing else in the tree may join.
	static QStringList leafTexts(const QTreeWidgetItem* item)
	{
		if (item->childCount() == 0) { return QStringList(item->text(0)); }
		QStringList texts;
		for (int i = 0; i < item->childCount(); ++i)
		{
			texts += leafTexts(item->child(i));
		}
		return texts;
	}

	static QStringList leafTexts(const QTreeWidget* tree)
	{
		QStringList texts;
		for (int i = 0; i < tree->topLevelItemCount(); ++i)
		{
			texts += leafTexts(tree->topLevelItem(i));
		}
		return texts;
	}

	// The drawing behind a fixture, for asserting what the *metric* says independently of what
	// the dialog does with it.
	static PartManager::KicadDrawing drawingOf(const std::string& text)
	{
		return PartManager::KicadGeometry::footprint(text);
	}

	// What the three-level cases are measured against: the 0.9 mm reference the fixtures below
	// are spread around.
	static std::string downloadedChipLand()
	{
		return chipLand("RESC1608X55N", "0.9");
	}

	// A database whose "Other packages" group has to become three levels — two 0805 footprints,
	// one 1206, and one on a part carrying no package at all — plus a 0603 favourite for the
	// group that stays two levels. The pad widths are the fixture's real content: they set the
	// scores, and with them both orderings this suite has to prove.
	static std::unique_ptr<PartManager::DatabaseHandle> threeLevelDatabase(const std::string& name,
		std::filesystem::path& outFolder, std::string& outError, int& outMigrated)
	{
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase(name, outFolder, outError);
		if (handle == nullptr) { return handle; }

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		struct Fixture
		{
			const char* part;
			const char* package;
			const char* footprint;
			const char* padWidth;
		};
		// Against the 0.9 mm reference: 1206 matches it exactly, the nameless part is 0.01 mm
		// out, the two 0805s are 0.025 and 0.05. So the package nodes must come out
		// 1206 → (no package) → 0805, which is neither alphabetical nor insertion order — and
		// the unnamed package lands in the *middle*, which is the only arrangement that shows it
		// is ordered by its best fit rather than swept to the end for having no name.
		static const Fixture fixtures[] = {
			{ "RC0603FR-074K7L", "0603", "R_0603_1608Metric",   "0.875" },
			{ "RC0805FR-0710KL", "0805", "R_0805_2012Metric",   "0.875" },
			{ "RC0805FR-0722KL", "0805", "R_0805_2012Metric_B", "0.85"  },
			{ "RC1206FR-0733KL", "1206", "R_1206_3216Metric",   "0.9"   },
			{ "NO-PACKAGE-PART", "",     "R_Nameless",          "0.89"  },
		};
		for (const Fixture& fixture : fixtures)
		{
			const int partId = makePart(db, fixture.part, fixture.package);
			if (partId == 0 || controller.attachRoleBytes(partId,
				PartManager::PartFileRole::KicadFootprint,
				chipLand(fixture.footprint, fixture.padWidth),
				std::string(fixture.footprint) + ".kicad_mod", &outError) == 0)
			{
				handle->close();
				return nullptr;
			}
		}
		// Last, and with no footprint of its own: the pre-attach path, where the part's package
		// has to come from its own row because `collect()` has never heard of it.
		outMigrated = makePart(db, "RC0603FR-0710KL", "0603");
		return handle;
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

		// One level deeper on this side: the strangers are split by package, so the group's own
		// child is the "0805" node and the part hangs under that. The package name is user data
		// and untranslated, which is what makes matching it safe here.
		QTreeWidgetItem* others = tree->topLevelItem(1);
		TEST_ASSERT_M(others->childCount() == 1 &&
			others->child(0)->text(0) == QStringLiteral("0805"),
			"the strangers are grouped under a node named after their package");
		TEST_ASSERT_M(others->child(0)->childCount() == 1 &&
			others->child(0)->child(0)->text(0) == QStringLiteral("RC0805FR-0710KL"),
			"the better-scoring stranger goes below, not above");

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// Reported on the user's own library: "Other packages" was one flat run of rows, and the
	// package is what they scan it by. It splits into a node per package — three levels — while
	// the favourites group stays two, because every row in it shares one package by construction
	// and a sub-level there would be a single node holding the whole group.
	TEST_FUNCTION(otherPackagesAreSplitIntoOneNodePerPackage)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		int migrating = 0;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			threeLevelDatabase("threelevel", folder, error, migrating);
		TEST_ASSERT_M(handle != nullptr, "fixture database failed: " + error);
		TEST_ASSERT(migrating != 0);

		const std::string downloaded = downloadedChipLand();
		// The premise, asserted rather than assumed: the node order below is only meaningful if
		// the fixture really does score 1206 best, then the nameless part, then the 0805s. If a
		// change to the metric ever reshuffles these, the ordering assertions prove nothing.
		const PartManager::KicadDrawing reference = drawingOf(downloaded);
		const double exact = PartManager::FootprintCompatibility::compare(reference,
			drawingOf(chipLand("R_1206_3216Metric", "0.9"))).score;
		const double nameless = PartManager::FootprintCompatibility::compare(reference,
			drawingOf(chipLand("R_Nameless", "0.89"))).score;
		const double closer0805 = PartManager::FootprintCompatibility::compare(reference,
			drawingOf(chipLand("R_0805_2012Metric", "0.875"))).score;
		const double further0805 = PartManager::FootprintCompatibility::compare(reference,
			drawingOf(chipLand("R_0805_2012Metric_B", "0.85"))).score;
		TEST_ASSERT_M(exact > nameless && nameless > closer0805 && closer0805 > further0805,
			"the fixture must spread the four strangers out in that order");

		PartManager::FootprintSuggestionDialog dialog(handle.get(), migrating,
			QByteArray(downloaded.data(), static_cast<int>(downloaded.size())),
			QStringLiteral("RESC1608X55N.kicad_mod"));

		TEST_ASSERT(dialog.hasSuggestions());
		QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
		TEST_ASSERT(tree != nullptr);
		TEST_ASSERT_M(tree->topLevelItemCount() == 2, "one favourites group and one for the rest");
		TEST_ASSERT_M(candidateRowCount(tree) == 5, "all five footprints have two pads");

		// The favourites group is untouched: two levels, its one row directly under the header.
		QTreeWidgetItem* favourites = tree->topLevelItem(0);
		TEST_ASSERT_M(favourites->childCount() == 1 &&
			favourites->child(0)->childCount() == 0,
			"the favourites group must stay two levels deep");
		TEST_ASSERT(favourites->child(0)->text(0) == QStringLiteral("RC0603FR-074K7L"));

		QTreeWidgetItem* others = tree->topLevelItem(1);
		TEST_ASSERT_M(others->childCount() == 3,
			"three packages among the strangers means three nodes");
		TEST_ASSERT_M(others->child(0)->text(0) == QStringLiteral("1206"),
			"the package holding the best-fitting candidate comes first");
		TEST_ASSERT_M(others->child(2)->text(0) == QStringLiteral("0805"),
			"and the one holding the worst comes last");
		// The part with no package at all. Its node's title is translated, so what is asserted is
		// that it *has* one — a node called "" is the failure this is guarding against.
		QTreeWidgetItem* namelessNode = others->child(1);
		TEST_ASSERT_M(!namelessNode->text(0).isEmpty(),
			"a candidate whose part has no package still needs a node with a name on it");
		TEST_ASSERT_M(namelessNode->childCount() == 1 &&
			namelessNode->child(0)->text(0) == QStringLiteral("NO-PACKAGE-PART"),
			"and the part with no package belongs under it");

		// Inside a package node the score still orders the rows, exactly as it did in the flat
		// list — the grouping only decides which node they sit in.
		TEST_ASSERT_M(others->child(2)->childCount() == 2 &&
			others->child(2)->child(0)->text(0) == QStringLiteral("RC0805FR-0710KL") &&
			others->child(2)->child(1)->text(0) == QStringLiteral("RC0805FR-0722KL"),
			"the better-scoring 0805 comes first inside its own package");

		for (int i = 0; i < others->childCount(); ++i)
		{
			TEST_ASSERT_M(others->child(i)->isExpanded(),
				"a collapsed package node hides the rows it was made to organise");
			TEST_ASSERT_M(!others->child(i)->flags().testFlag(Qt::ItemIsSelectable),
				"a package node is not a footprint and must not be choosable");
			TEST_ASSERT_M(others->child(i)->flags().testFlag(Qt::ItemIsEnabled),
				"and it must stay enabled, or it greys out and reads as broken");
		}

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// Reported by the user: Up and Down stopped on the group headers. Clearing `ItemIsSelectable`
	// is not enough — Qt moves the *current item* by flags it does not consult — so the dialog
	// answers the two keys itself. With the package nodes there are now two kinds of row to step
	// over, and every leaf still has to be reachable, the first and the last included.
	TEST_FUNCTION(arrowKeysStepFromLeafToLeafOnly)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		int migrating = 0;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			threeLevelDatabase("arrowkeys", folder, error, migrating);
		TEST_ASSERT_M(handle != nullptr, "fixture database failed: " + error);

		const std::string downloaded = downloadedChipLand();
		PartManager::FootprintSuggestionDialog dialog(handle.get(), migrating,
			QByteArray(downloaded.data(), static_cast<int>(downloaded.size())),
			QStringLiteral("RESC1608X55N.kicad_mod"));
		TEST_ASSERT(dialog.hasSuggestions());

		QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
		TEST_ASSERT(tree != nullptr);
		const QStringList leaves = leafTexts(tree);
		TEST_ASSERT_M(leaves.size() == 5, "the fixture has five choices in it");

		// Shown, because synthetic key events are refused to a hidden widget — and because a key
		// press is only worth testing where a user could have made it.
		TEST_ASSERT_M(UnitTest::Gui::showAndWait(&dialog), "the dialog never became visible");

		// The constructor preselects the best favourite, which is the first leaf.
		TEST_ASSERT(tree->currentItem() != nullptr);
		TEST_ASSERT_M(tree->currentItem()->text(0) == leaves.at(0),
			"the dialog opens on its first choice");

		// Down the whole list. Every stop must be a leaf, in the order the tree shows them —
		// the headers and the package nodes are the rows this used to land on.
		for (int i = 1; i < leaves.size(); ++i)
		{
			TEST_ASSERT(UnitTest::Gui::keyClick(tree, Qt::Key_Down));
			QTreeWidgetItem* current = tree->currentItem();
			TEST_ASSERT_M(current != nullptr && current->childCount() == 0,
				"Down landed on a row that is not a choice");
			TEST_ASSERT_M(current->text(0) == leaves.at(i),
				"Down skipped a leaf or reached them out of order");
		}

		// Past the end: the last leaf stays current. Clearing it here would empty the overlay
		// just for running out of list.
		TEST_ASSERT(UnitTest::Gui::keyClick(tree, Qt::Key_Down));
		TEST_ASSERT_M(tree->currentItem() != nullptr &&
			tree->currentItem()->text(0) == leaves.last(),
			"Down at the end of the list must stay on the last leaf");

		for (int i = leaves.size() - 2; i >= 0; --i)
		{
			TEST_ASSERT(UnitTest::Gui::keyClick(tree, Qt::Key_Up));
			QTreeWidgetItem* current = tree->currentItem();
			TEST_ASSERT_M(current != nullptr && current->childCount() == 0,
				"Up landed on a row that is not a choice");
			TEST_ASSERT_M(current->text(0) == leaves.at(i),
				"Up skipped a leaf or reached them out of order");
		}
		TEST_ASSERT(UnitTest::Gui::keyClick(tree, Qt::Key_Up));
		TEST_ASSERT_M(tree->currentItem() != nullptr &&
			tree->currentItem()->text(0) == leaves.at(0),
			"Up at the top of the list must stay on the first leaf");

		// Left and Right are untouched, which is what still collapses and expands a group. The
		// current row is put on the header directly rather than reached with Left, so that this
		// keeps testing the two keys and not Qt's rule for where Left moves from a leaf.
		QTreeWidgetItem* others = tree->topLevelItem(1);
		tree->setCurrentItem(others);
		TEST_ASSERT(UnitTest::Gui::keyClick(tree, Qt::Key_Left));
		TEST_ASSERT_M(!others->isExpanded(), "Left must still collapse a group");

		// And Down out of a collapsed group skips what it is hiding: the rows under it are not
		// on screen, so they are not the next row in the direction of travel either.
		TEST_ASSERT(UnitTest::Gui::keyClick(tree, Qt::Key_Down));
		TEST_ASSERT_M(tree->currentItem() == others,
			"there is no leaf below a collapsed last group, so current stays put");

		TEST_ASSERT(UnitTest::Gui::keyClick(tree, Qt::Key_Right));
		TEST_ASSERT_M(others->isExpanded(), "Right must still expand it again");
		TEST_ASSERT(UnitTest::Gui::keyClick(tree, Qt::Key_Down));
		TEST_ASSERT_M(tree->currentItem() != nullptr &&
			tree->currentItem()->childCount() == 0,
			"and Down from the header then reaches the first leaf under it");

		dialog.close();
		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// The three cases below are about `Trigger`, and the fixture is one database built so the two
	// triggers must answer differently: the part already shares its footprint with a neighbour —
	// which is what accepting an earlier suggestion leaves behind, so it is the ordinary state, not
	// a corner — and a third part holds a different, compatible one.
	//
	// Byte-identical candidate present, and it is the *first* thing the search meets.
	static std::unique_ptr<PartManager::DatabaseHandle> sharedPlusOneDatabase(const std::string& name,
		std::filesystem::path& outFolder, std::string& outError, int& outMigrated, int& outOther)
	{
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase(name, outFolder, outError);
		if (handle == nullptr) { return handle; }

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		const int neighbour = makePart(db, "RC0603FR-074K7L");
		outMigrated = makePart(db, "RC0603FR-0710KL");
		outOther = makePart(db, "RC0603FR-0722KL");

		// Neighbour and migrated carry the *same bytes*, so the content-addressed store has them
		// on one file already — the state "accept a suggestion" produces.
		const std::string shared = chipLand("R_0603_1608Metric", "0.875");
		if (controller.attachRoleBytes(neighbour, PartManager::PartFileRole::KicadFootprint,
				shared, "R_0603_1608Metric.kicad_mod", &outError) == 0
			|| controller.attachRoleBytes(outMigrated, PartManager::PartFileRole::KicadFootprint,
				shared, "R_0603_1608Metric.kicad_mod", &outError) == 0
			|| controller.attachRoleBytes(outOther, PartManager::PartFileRole::KicadFootprint,
				chipLand("RESC1608X55N", "0.9"), "RESC1608X55N.kicad_mod", &outError) == 0)
		{
			handle->close();
			return nullptr;
		}
		return handle;
	}

	// The bug the button was added for. `buildCandidates()` ends the whole search on a
	// byte-identical candidate, which is right after an import and wrong here: the user pressed a
	// button asking for something *different*, and the identical file is the one candidate that
	// certainly is not one. Skipping it must leave the rest of the list standing.
	TEST_FUNCTION(aUserRequestedOfferKeepsTheCandidatesAnIdenticalOneWouldHaveWiped)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		int migrating = 0;
		int other = 0;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			sharedPlusOneDatabase("userrequested", folder, error, migrating, other);
		TEST_ASSERT_M(handle != nullptr, "fixture database failed: " + error);
		TEST_ASSERT(migrating != 0 && other != 0);

		// The reference is what the part carries now, which is the shared file itself.
		const std::string current = chipLand("R_0603_1608Metric", "0.875");
		PartManager::FootprintSuggestionDialog dialog(handle.get(), migrating,
			QByteArray(current.data(), static_cast<int>(current.size())),
			QStringLiteral("R_0603_1608Metric.kicad_mod"),
			PartManager::FootprintSuggestionDialog::Trigger::UserRequested);

		TEST_ASSERT_M(dialog.hasSuggestions(),
			"an identical candidate must not wipe the list on the user-requested path");
		QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
		TEST_ASSERT(tree != nullptr);
		TEST_ASSERT_M(candidateRowCount(tree) == 1,
			"the identical one is skipped and the different one survives");
		TEST_ASSERT_M(tree->topLevelItem(0)->child(0)->text(0) ==
			QStringLiteral("RC0603FR-0722KL"),
			"the row that survived must be the part holding the *other* footprint");

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// The other half of the same fixture: on the import path an identical candidate still means
	// the store will share the file by itself, so the offer stays out of the way. The two triggers
	// have to disagree here, or one of them is broken.
	TEST_FUNCTION(anImportOfferStaysSilentOnTheSameDatabase)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::filesystem::path folder;
		std::string error;
		int migrating = 0;
		int other = 0;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			sharedPlusOneDatabase("afterimport", folder, error, migrating, other);
		TEST_ASSERT_M(handle != nullptr, "fixture database failed: " + error);
		TEST_ASSERT(migrating != 0 && other != 0);

		const std::string downloaded = chipLand("R_0603_1608Metric", "0.875");
		PartManager::FootprintSuggestionDialog dialog(handle.get(), migrating,
			QByteArray(downloaded.data(), static_cast<int>(downloaded.size())),
			QStringLiteral("R_0603_1608Metric.kicad_mod"));

		TEST_ASSERT_M(!dialog.hasSuggestions(),
			"a download already in the store byte for byte has nothing to ask about");
		QTreeWidget* tree = dialog.findChild<QTreeWidget*>();
		TEST_ASSERT(tree != nullptr && candidateRowCount(tree) == 0);

		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// Silence is the bug the user hit twice, so the button path has to be able to say *why* it
	// found nothing. Each reason is a different sentence in the part editor, so each has to be
	// told apart here — and `offerReplacement()` is the real entry point, which also proves the
	// read-back of the attached footprint. None of these opens a dialog: an empty list returns
	// before exec(), which is the contract this is checking.
	TEST_FUNCTION(aUserRequestedOfferSaysWhyItFoundNothing)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		using Outcome = PartManager::FootprintSuggestionDialog::Outcome;

		std::filesystem::path folder;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			makeDatabase("emptyreason", folder, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		SQLiteWrapper::SQLite& db = handle->connection();

		const int bare = makePart(db, "RC0603FR-0733KL");
		const int alone = makePart(db, "RC0603FR-0710KL");
		TEST_ASSERT_M(controller.attachRoleBytes(alone, PartManager::PartFileRole::KicadFootprint,
			chipLand("R_0603_1608Metric", "0.875"), "R_0603_1608Metric.kicad_mod", &error) != 0,
			"attach failed: " + error);

		TEST_ASSERT_M(PartManager::FootprintSuggestionDialog::offerReplacement(nullptr, controller,
			bare) == Outcome::NoFootprintAttached,
			"with an empty slot there is no reference and the button is disabled anyway");
		TEST_ASSERT_M(PartManager::FootprintSuggestionDialog::offerReplacement(nullptr, controller,
			alone) == Outcome::NoOtherFootprints,
			"the only footprint in the database is this part's own");

		// Now give the bare part the very same file. Nothing changes for the user — the two are
		// already one file — but "nothing to share" would be the wrong sentence for it.
		TEST_ASSERT(controller.attachRoleBytes(bare, PartManager::PartFileRole::KicadFootprint,
			chipLand("R_0603_1608Metric", "0.875"), "R_0603_1608Metric.kicad_mod", &error) != 0);
		TEST_ASSERT_M(PartManager::FootprintSuggestionDialog::offerReplacement(nullptr, controller,
			alone) == Outcome::OnlyIdenticalOnes,
			"a database whose other footprints are all this very file says so");

		// And one with a genuinely different footprint that the pad-count filter drops.
		const int quad = makePart(db, "DFN-4-DUMMY");
		TEST_ASSERT(controller.attachRoleBytes(quad, PartManager::PartFileRole::KicadFootprint,
			quadLand("DFN_4"), "DFN_4.kicad_mod", &error) != 0);
		TEST_ASSERT_M(PartManager::FootprintSuggestionDialog::offerReplacement(nullptr, controller,
			alone) == Outcome::AllFilteredOut,
			"a four-pad footprint is not a two-pad one, and the reason must say that");

		// The poller the other cases rely on is not armed here on purpose: every call above must
		// have returned without showing anything, and a dialog left in exec() would hang the suite
		// rather than fail it. Reaching this line is the assertion.
		handle->close();
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}
};

TEST_INSTANTIATE(TST_FootprintSuggestionArchive);
