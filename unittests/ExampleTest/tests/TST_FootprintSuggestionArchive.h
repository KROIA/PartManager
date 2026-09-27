#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "controllers/PartManager_PartEditorController.h"
#include "database/PartManager_DatabaseHandle.h"
#include "filestore/PartManager_FileStore.h"
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

	static int makePart(SQLiteWrapper::SQLite& db, const std::string& name)
	{
		PartManager::Part part;
		part.name = name;
		part.package = "0603";
		return PartManager::PartRepository::insertPart(db, part);
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
					candidateRows = tree->topLevelItemCount();
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
};

TEST_INSTANTIATE(TST_FootprintSuggestionArchive);
