#pragma once

#include "UnitTest.h"
#include "llm/PartManager_EcadDownloadToolset.h"
#include "llm/PartManager_PartToolset.h"
#include "tests/TST_LlmTestDatabase.h"
#include "tests/TST_PdfText.h"
#include "UnitTest_Gui.h"

// §5c/§14f: the tools that find a vendor ECAD download and attach it to a part — and, mostly,
// the boundary they are allowed to reach the filesystem through. Driven with no model: every
// rule below is a handler's own, and a case that needed a server up could not pin any of them.
//
// The live half was run separately and is not here, because it cannot assert anything this
// cannot: gpt-oss:20b, 2026-09-27, twice — "I already downloaded the ECAD library zip for
// 74HC4051PW-Q100,11" went list_downloaded_libraries(mpn) -> search_parts -> [inspect ->]
// attach_downloaded_library in 4-6 calls and ~135 s, with the archive named out of the listing
// and never a path. What that proved about the *code* is all below.

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "controllers/PartManager_PartEditorController.h"
	#include "llm/PartManager_LlmUiBridge.h"
	#include "ui/PartManager_PartEditorDialog.h"
	#include "domain/PartManager_PartFile.h"
	#include "domain/PartManager_PartFileRole.h"
	#include <QByteArray>
	#include <QDir>
	#include <QLabel>
	#include <QLineEdit>
	#include <QJsonArray>
	#include <QJsonObject>
	#include <QString>
	#include <private/qzipwriter_p.h>
	#include <filesystem>
	#include <fstream>
	#include <string>
#endif

class TST_LlmEcadDownloadToolset : public UnitTest::Test
{
	TEST_CLASS(TST_LlmEcadDownloadToolset)
public:
	TST_LlmEcadDownloadToolset()
		: Test("TST_LlmEcadDownloadToolset")
	{
#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_LlmEcadDownloadToolset::handlersWorkWithoutAModel);
		ADD_TEST(TST_LlmEcadDownloadToolset::theDatasheetToolTakesAPdfAndSavesThePart);
		ADD_TEST(TST_LlmEcadDownloadToolset::anOpenEditorDoesNotUndoTheDatasheet);
#endif
	}

private:
#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	static const char* mpn() { return "74HC4051PW-Q100,11"; }

	// A temp folder holding a plausibly-named vendor download, a decoy, and a non-archive.
	static std::filesystem::path makeDownloadsFolder()
	{
		const std::filesystem::path folder = std::filesystem::path(QDir::tempPath().toStdString())
			/ ("PartManager_TST_EcadDownloads_" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);

		auto writeArchive = [](const std::filesystem::path& path, const QString& prefix)
		{
			QZipWriter writer(QString::fromStdString(path.string()));
			writer.addFile(prefix + QStringLiteral("/CADSTAR/x.lib"), QByteArray("not kicad"));
			writer.addFile(prefix + QStringLiteral("/KiCad/sym.kicad_sym"),
				QByteArray("(kicad_symbol_lib (version 20241209)\n)\n"));
			writer.addFile(prefix + QStringLiteral("/KiCad/fp.kicad_mod"),
				QByteArray("(footprint \"SOP65P640X110-16N\"\n)\n"));
			writer.addFile(prefix + QStringLiteral("/3D/model.stp"),
				QByteArray("ISO-10303-21;\nHEADER;\n"));
			writer.close();
		};
		writeArchive(folder / "LIB_74HC4051PW-Q100,11(5).zip",
			QStringLiteral("74HC4051PW-Q100,11"));
		writeArchive(folder / "LIB_STM32F103C8T6.zip", QStringLiteral("STM32F103C8T6"));
		std::ofstream(folder / "notes.txt") << "not an archive";
		return folder;
	}

	static PartManager::EcadDownloadContext contextFor(const ScopedDatabase& database,
		const std::filesystem::path& folder, bool allowWrites = true,
		PartManager::LlmUiBridge* ui = nullptr)
	{
		PartManager::EcadDownloadContext context;
		context.database = database.handle();
		context.allowWrites = allowWrites;
		context.allowedRoots = QStringList{ QString::fromStdString(folder.string()) };
		context.ui = ui;
		return context;
	}

	static int makePart(const std::vector<PartManager::LlmTool>& tools)
	{
		QJsonObject args;
		args["categoryId"] = llmCategoryIdNamed(tools, QStringLiteral("Resistor"));
		args["name"] = "8-channel analog multiplexer";
		args["mpn"] = mpn();
		args["manufacturer"] = "Nexperia";
		return callLlmTool(tools, "create_part", args).value("id").toInt();
	}

	// The window, reduced to the one method this toolset uses. Everything else is a pure virtual
	// the interface needs and this case does not, answered with the emptiest true thing — the
	// point of the stub is that a tool holding a bridge reaches exactly one of them.
	class EditorOnlyBridge : public PartManager::LlmUiBridge
	{
	public:
		int reloadCalls = 0;

		int selectedCategoryId() const override { return 0; }
		QString selectedCategoryName() const override { return QString(); }
		int selectedPartId(QString*) const override { return 0; }
		bool selectCategory(int) override { return false; }
		PartManager::SelectPartResult selectPart(int) override
		{ return PartManager::SelectPartResult(); }
		QString treeFilter() const override { return QString(); }
		void setTreeFilter(const QString&) override {}
		QString tableFilter() const override { return QString(); }
		void setTableFilter(const QString&) override {}
		bool allCategoriesSearch() const override { return false; }
		void setAllCategoriesSearch(bool) override {}
		bool hideEmptyCategories() const override { return false; }
		void setHideEmptyCategories(bool) override {}
		bool openPartEditor(int, bool*) override { return false; }

		// The real one, wired to the real registry — this is what is under test.
		bool reloadOpenPartEditor(int partId) override
		{
			++reloadCalls;
			return PartManager::PartEditorDialog::reloadIfOpen(partId);
		}
	};

	static void writeFile(const std::filesystem::path& path, const std::string& bytes)
	{
		std::ofstream out(path, std::ios::binary);
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	}

	static bool hasRoleFile(const ScopedDatabase& database, int partId,
		PartManager::PartFileRole role)
	{
		PartManager::PartEditorController editor(database.handle());
		PartManager::PartFile file;
		return editor.roleFile(partId, role, file);
	}

	TEST_FUNCTION(handlersWorkWithoutAModel)
	{
		TEST_START;

		ScopedDatabase database("zz_downloads");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::filesystem::path folder = makeDownloadsFolder();
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const int partId = makePart(partTools);
		TEST_ASSERT(partId != 0);

		const std::vector<PartManager::LlmTool> tools =
			PartManager::EcadDownloadToolset::tools(contextFor(database, folder));
		TEST_COMPARE(tools.size(), static_cast<size_t>(4));

		// Unfiltered: both archives, never notes.txt.
		const QJsonObject all = callLlmTool(tools, "list_downloaded_libraries");
		TEST_ASSERT_M(llmResultOk(all), llmResultMessage(all));
		TEST_COMPARE(all.value("files").toArray().size(), 2);

		// By MPN: the vendor's mangled name still matches.
		QJsonObject byMpn;
		byMpn["mpn"] = mpn();
		const QJsonObject filtered = callLlmTool(tools, "list_downloaded_libraries", byMpn);
		TEST_COMPARE(filtered.value("files").toArray().size(), 1);
		const QJsonObject entry = filtered.value("files").toArray().at(0).toObject();
		TEST_COMPARE(entry.value("file").toString().toStdString(),
			std::string("LIB_74HC4051PW-Q100,11(5).zip"));
		TEST_ASSERT_M(!entry.value("modified").toString().isEmpty(), "no modification time");
		TEST_MESSAGE("listed: " + QJsonDocument(entry).toJson(QJsonDocument::Compact).toStdString());

		// The boundary. Each of these is an error naming the rule, never a silent empty answer.
		const char* refused[] = { "../secrets.zip", "C:/Windows/x.zip", "sub/LIB.zip", "notes.txt" };
		for (const char* bad : refused)
		{
			QJsonObject args;
			args["file"] = bad;
			const QJsonObject result = callLlmTool(tools, "inspect_downloaded_library", args);
			TEST_ASSERT_M(!llmResultOk(result), std::string("must refuse ") + bad);
			TEST_MESSAGE(std::string(bad) + " -> " + llmResultMessage(result));
		}

		QJsonObject good;
		good["file"] = "LIB_74HC4051PW-Q100,11(5).zip";
		const QJsonObject inspected = callLlmTool(tools, "inspect_downloaded_library", good);
		TEST_ASSERT_M(llmResultOk(inspected), llmResultMessage(inspected));
		TEST_ASSERT(inspected.value("symbol").toObject().value("present").toBool());
		TEST_ASSERT(inspected.value("footprint").toObject().value("present").toBool());
		TEST_ASSERT(inspected.value("model3d").toObject().value("present").toBool());
		// Read-only: inspecting attached nothing.
		TEST_ASSERT_M(!hasRoleFile(database, partId, PartManager::PartFileRole::KicadSymbol),
			"inspect must not attach anything");

		// Read-only context refuses the write and says why.
		QJsonObject attachArgs;
		attachArgs["partId"] = partId;
		attachArgs["file"] = "LIB_74HC4051PW-Q100,11(5).zip";
		const std::vector<PartManager::LlmTool> readOnly =
			PartManager::EcadDownloadToolset::tools(contextFor(database, folder, false));
		const QJsonObject refusedWrite =
			callLlmTool(readOnly, "attach_downloaded_library", attachArgs);
		TEST_ASSERT_M(!llmResultOk(refusedWrite), "a read-only assistant must not attach");

		const QJsonObject attached = callLlmTool(tools, "attach_downloaded_library", attachArgs);
		TEST_ASSERT_M(llmResultOk(attached), llmResultMessage(attached));
		TEST_ASSERT(attached.value("symbolAttached").toBool());
		TEST_ASSERT(attached.value("footprintAttached").toBool());
		TEST_ASSERT(attached.value("modelAttached").toBool());
		TEST_ASSERT(hasRoleFile(database, partId, PartManager::PartFileRole::KicadSymbol));
		TEST_ASSERT(hasRoleFile(database, partId, PartManager::PartFileRole::KicadFootprint));
		TEST_ASSERT(hasRoleFile(database, partId, PartManager::PartFileRole::Kicad3DModel));

		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

	// §3's two datasheet paths behind one tool, and the part row that has to be saved for either
	// of them to mean anything.
	TEST_FUNCTION(theDatasheetToolTakesAPdfAndSavesThePart)
	{
		TEST_START;

		ScopedDatabase database("ecad_datasheet");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::filesystem::path folder = makeDownloadsFolder();
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const int partId = makePart(partTools);
		TEST_ASSERT(partId != 0);

		// A readable PDF in the allowed folder, a scan somewhere else entirely.
		const std::filesystem::path inFolder = folder / "74HC4051 datasheet.pdf";
		writeFile(inFolder, PdfFixtures::flatePdf());
		const std::filesystem::path elsewhere =
			std::filesystem::path(QDir::tempPath().toStdString())
			/ ("PartManager_TST_scan_" + std::to_string(QDateTime::currentMSecsSinceEpoch()) + ".pdf");
		writeFile(elsewhere, PdfFixtures::scannedPdf());

		const std::vector<PartManager::LlmTool> tools =
			PartManager::EcadDownloadToolset::tools(contextFor(database, folder));

		// Both and neither are the two shapes a confused model produces, and both are refused.
		QJsonObject neither;
		neither["partId"] = partId;
		TEST_ASSERT(!llmResultOk(callLlmTool(tools, "set_part_datasheet", neither)));
		QJsonObject both = neither;
		both["url"] = "https://example.com/x.pdf";
		both["file"] = "74HC4051 datasheet.pdf";
		TEST_ASSERT(!llmResultOk(callLlmTool(tools, "set_part_datasheet", both)));

		// A bare name resolves in the allowed folder, exactly like the archive tools.
		QJsonObject byName;
		byName["partId"] = partId;
		byName["file"] = "74HC4051 datasheet.pdf";
		const QJsonObject attached = callLlmTool(tools, "set_part_datasheet", byName);
		TEST_ASSERT_M(llmResultOk(attached), llmResultMessage(attached));
		TEST_ASSERT_M(attached.value("readable").toBool(), "the fixture PDF carries text");
		TEST_ASSERT(!attached.value("looksScanned").toBool());
		TEST_COMPARE(attached.value("source").toString().toStdString(), std::string("file"));
		// No window in this host, so nothing could be stale and nothing was reloaded.
		TEST_ASSERT(!attached.value("editorReloaded").toBool());

		// **The part row, not just the part_file row.** Both controller calls only touch the
		// in-memory copy, so a handler that forgot to save would still report a file id.
		PartManager::Part saved;
		TEST_ASSERT(PartManager::PartEditorController(database.handle()).loadPart(partId, saved));
		TEST_ASSERT_M(saved.datasheetFileId != 0, "the part row must point at the datasheet");

		// An absolute path is the one extra thing this tool accepts, and a scan is reported as
		// attached-but-unreadable rather than as a failure.
		QJsonObject byPath;
		byPath["partId"] = partId;
		byPath["file"] = QString::fromStdString(elsewhere.string());
		const QJsonObject scan = callLlmTool(tools, "set_part_datasheet", byPath);
		TEST_ASSERT_M(llmResultOk(scan), llmResultMessage(scan));
		TEST_ASSERT_M(scan.value("looksScanned").toBool(), "the scan fixture has no text layer");
		TEST_ASSERT_M(!scan.value("readable").toBool(), "a scan must not be reported as readable");
		TEST_ASSERT_M(!scan.value("note").toString().isEmpty(), "a scan has to say so in words");
		TEST_MESSAGE("scan: " + llmResultMessage(scan)
			+ QJsonDocument(scan).toJson(QJsonDocument::Compact).toStdString());

		// What it still refuses: a path that is not a PDF, a PDF that is not there, and a URL
		// that is not one.
		const char* badFiles[] = { "C:/Windows/system32/notepad.exe", "C:/nowhere/at/all.pdf" };
		for (const char* bad : badFiles)
		{
			QJsonObject args;
			args["partId"] = partId;
			args["file"] = bad;
			const QJsonObject refused = callLlmTool(tools, "set_part_datasheet", args);
			TEST_ASSERT_M(!llmResultOk(refused), std::string("must refuse ") + bad);
			TEST_MESSAGE(std::string(bad) + " -> " + llmResultMessage(refused));
		}
		QJsonObject notAUrl;
		notAUrl["partId"] = partId;
		notAUrl["url"] = "74HC4051PW-Q100,11";
		TEST_ASSERT(!llmResultOk(callLlmTool(tools, "set_part_datasheet", notAUrl)));

		// A read-only assistant may not attach one at all.
		const std::vector<PartManager::LlmTool> readOnly =
			PartManager::EcadDownloadToolset::tools(contextFor(database, folder, false));
		TEST_ASSERT(!llmResultOk(callLlmTool(readOnly, "set_part_datasheet", byName)));

		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
		std::filesystem::remove(elsewhere, ec);
	}

	// The regression the modeless editor created (§10, §14f).
	//
	// A part editor outlives the call that opened it and holds its own copy of the row, and §10
	// autosave writes *every* column back — `datasheet_file_id` included. So a tool that attaches
	// a datasheet behind an open editor has its write undone by the user's next keystroke, and
	// nothing anywhere says so: the file is still in the store, the part simply stops pointing at
	// it. This case is that exact sequence.
	TEST_FUNCTION(anOpenEditorDoesNotUndoTheDatasheet)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		ScopedDatabase database("ecad_open_editor");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::filesystem::path folder = makeDownloadsFolder();
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const int partId = makePart(partTools);
		TEST_ASSERT(partId != 0);
		writeFile(folder / "74HC4051 datasheet.pdf", PdfFixtures::flatePdf());

		// The user has the part open, as they would while asking the assistant about it.
		PartManager::PartEditorDialog* dialog =
			PartManager::PartEditorDialog::open(database.handle(), partId, nullptr);
		TEST_ASSERT(dialog != nullptr);
		TEST_ASSERT(UnitTest::Gui::showAndWait(dialog));

		QLabel* datasheetLabel = UnitTest::Gui::find<QLabel>("datasheetLabel", dialog);
		TEST_ASSERT_M(datasheetLabel != nullptr, "no datasheetLabel in the editor:\n"
			+ UnitTest::Gui::dumpWidgetTree(dialog).toStdString());

		EditorOnlyBridge bridge;
		const std::vector<PartManager::LlmTool> tools =
			PartManager::EcadDownloadToolset::tools(contextFor(database, folder, true, &bridge));

		QJsonObject args;
		args["partId"] = partId;
		args["file"] = "74HC4051 datasheet.pdf";
		const QJsonObject attached = callLlmTool(tools, "set_part_datasheet", args);
		TEST_ASSERT_M(llmResultOk(attached), llmResultMessage(attached));
		TEST_COMPARE(bridge.reloadCalls, 1);
		TEST_ASSERT_M(attached.value("editorReloaded").toBool(),
			"the tool has to report that it refreshed the user's open editor");

		// The keystroke that used to undo it. nameEdit is wired to textChanged, so this is
		// exactly what typing does: schedule the debounced §10 write.
		QLineEdit* nameEdit = UnitTest::Gui::find<QLineEdit>("nameEdit", dialog);
		TEST_ASSERT(nameEdit != nullptr);
		const std::string typed = "8-channel analog multiplexer (checked)";
		nameEdit->setText(QString::fromStdString(typed));

		// Pumped, never slept: the 400 ms debounce needs the event loop this would otherwise
		// block. Waiting for the *name* is what makes the assertion below meaningful — it says
		// the editor's own write really happened, so a surviving datasheet id survived it.
		const PartManager::PartEditorController check(database.handle());
		const bool wrote = UnitTest::Gui::waitFor([&check, partId, &typed]()
			{
				PartManager::Part row;
				return check.loadPart(partId, row) && row.name == typed;
			}, 3000);
		TEST_ASSERT_M(wrote, "the editor's debounced autosave never ran");

		PartManager::Part saved;
		TEST_ASSERT(check.loadPart(partId, saved));
		TEST_ASSERT_M(saved.datasheetFileId != 0,
			"the open editor's autosave put its stale copy back and undid the datasheet");

		// ...and the user is looking at the new file, not at "no datasheet attached yet". Checked
		// after the row, because the row is the damage and the label is only how it shows.
		TEST_ASSERT_M(datasheetLabel->text().contains(QStringLiteral("74HC4051")),
			"the open editor still shows the old state: \""
				+ datasheetLabel->text().toStdString() + "\"");

		UnitTest::Gui::closeWindow(dialog);
		PartManager::PartEditorDialog::closeAll();

		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
	}

#endif
};

TEST_INSTANTIATE(TST_LlmEcadDownloadToolset);
