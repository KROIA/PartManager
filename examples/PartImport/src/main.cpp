// One-shot importer for a `Stockcount;MouserNR;Link` CSV (the shape of .claude/DefaultParts.csv):
// each Mouser part number is looked up on the live Search API, mapped through MouserSearchService,
// and inserted into a .pmdb database. Re-runnable — a part whose MPN is already in the database is
// skipped, not duplicated.
//
// Usage: PartImport <database.pmdb> <parts.csv> [--dry-run]
// A .pmdb path that does not exist yet is created (folder layout + schema + seeded templates).
//
// Needs MOUSER_SEARCH_API in the environment. Console target on purpose: this is a scripted
// migration step, not part of the GUI.
#include "database/PartManager_DatabaseHandle.h"
#include "database/PartManager_DatabaseRegistry.h"
#include "filestore/PartManager_FileStore.h"
#include "mouser/PartManager_MouserClient.h"
#include "mouser/PartManager_MouserSearchService.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_SellerRepository.h"
#include "persistence/PartManager_StockRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_TagRepository.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>

#include "SQLite.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
	struct CsvRow
	{
		int stockCount = 0;
		std::string mouserPartNumber;
	};

	std::string trimQuotes(const std::string& text)
	{
		std::string out = text;
		while (!out.empty() && (out.back() == '\r' || out.back() == '"' || out.back() == ' '))
		{
			out.pop_back();
		}
		size_t start = 0;
		while (start < out.size() && (out[start] == '"' || out[start] == ' '))
		{
			++start;
		}
		return out.substr(start);
	}

	// Only the first two columns matter; Link is the Mouser product page, which the API hands us
	// back anyway. Rows whose Stockcount is not a number are reported and skipped, never guessed at.
	bool parseRows(const std::string& csvPath, std::vector<CsvRow>& outRows, std::string& outError)
	{
		std::ifstream file(csvPath);
		if (!file.is_open())
		{
			outError = "cannot open " + csvPath;
			return false;
		}
		std::string line;
		bool firstLine = true;
		while (std::getline(file, line))
		{
			if (firstLine)
			{
				firstLine = false;   // header
				continue;
			}
			const size_t firstSemicolon = line.find(';');
			if (firstSemicolon == std::string::npos)
			{
				continue;            // blank or trailing line
			}
			const size_t secondSemicolon = line.find(';', firstSemicolon + 1);
			CsvRow row;
			try
			{
				row.stockCount = std::stoi(trimQuotes(line.substr(0, firstSemicolon)));
			}
			catch (const std::exception&)
			{
				std::printf("  skipped (bad Stockcount): %s\n", line.c_str());
				continue;
			}
			row.mouserPartNumber = trimQuotes(secondSemicolon == std::string::npos
				? line.substr(firstSemicolon + 1)
				: line.substr(firstSemicolon + 1, secondSemicolon - firstSemicolon - 1));
			if (!row.mouserPartNumber.empty())
			{
				outRows.push_back(row);
			}
		}
		return true;
	}

	int findTypeIdByName(SQLiteWrapper::SQLite& db, const std::string& name)
	{
		if (name.empty())
		{
			return 0;
		}
		for (const PartManager::PartType& type : PartManager::PartTypeRepository::listTypes(db))
		{
			if (type.name == name)
			{
				return type.id;
			}
		}
		return 0;
	}

	// The CSV's Mouser number is the whole reason a row can be looked up, and it is the only place
	// the Cart API's article number ever comes from (§6: part.mpn is the *manufacturer's* number and
	// the Cart API rejects it). Writing the link is therefore part of importing, not an extra: an
	// imported part without one cannot be ordered and shows an empty Mouser-Nr. in the editor.
	// Upsert semantics in linkPart() make this safe to run again over a part that already has it.
	void linkMouser(SQLiteWrapper::SQLite& db, int partId, const PartManager::MouserPartPrefill& prefill)
	{
		if (partId == 0 || prefill.mouserPartNumber.empty())
		{
			return;
		}
		PartManager::PartSellerLink link;
		link.partId = partId;
		link.sellerId = PartManager::SellerRepository::ensureMouserSeller(db);
		link.sellerPartNumber = prefill.mouserPartNumber;
		link.url = prefill.productDetailUrl;
		link.isPrimary = true;
		const int linkId = PartManager::SellerRepository::linkPart(db, link);
		if (linkId != PartManager::NoPartSellerLinkId && !prefill.priceBreaks.empty())
		{
			PartManager::SellerRepository::recordQuote(db, linkId, prefill.priceBreaks);
		}
	}

	// The product photo and the datasheet are URLs on the prefill, not part columns, so importing
	// without this leaves every part picture-less while the GUI's New Part flow — which downloads
	// exactly these two — fills them in. Same reason the seller link needed writing: the console
	// importer was keeping only the part row out of everything the lookup returned.
	//
	// An occupied slot is left alone, so a re-run backfills what is missing without re-downloading
	// what is there, and a failure is reported rather than retried (Mouser's CDN answers a request
	// it dislikes with a 200 and an HTML page; FileStore::looksLikeBlockPage catches that one).
	void attachRemote(PartManager::FileStore& store, SQLiteWrapper::SQLite& db, int partId,
		PartManager::PartFileRole role, const std::string& url, const char* label)
	{
		if (partId == 0 || url.empty())
		{
			return;
		}
		PartManager::PartFile occupied;
		if (PartManager::FileStore::roleFile(db, partId, role, occupied))
		{
			return;
		}
		const PartManager::FileStoreResult stored = store.downloadFile(url);
		if (!stored.ok)
		{
			std::printf("       %s not downloaded: %s\n", label, stored.errorMessage.c_str());
			return;
		}
		std::string error;
		if (store.adoptStoredFile(db, partId, role, stored, &error) == 0)
		{
			std::printf("       %s not attached: %s\n", label, error.c_str());
		}
	}
}

int main(int argc, char* argv[])
{
	// Unbuffered: redirected to a file or a pipe the CRT block-buffers stdout, and progress written
	// during a run this long has to be visible while it runs, not after it ends.
	setvbuf(stdout, nullptr, _IONBF, 0);

	QCoreApplication app(argc, argv);   // QNetworkAccessManager needs one
	QCoreApplication::setOrganizationName("KROIA");
	QCoreApplication::setApplicationName("PartManager");

	std::vector<std::string> positional;
	bool dryRun = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];
		if (argument == "--dry-run")
		{
			dryRun = true;
		}
		else
		{
			positional.push_back(argument);
		}
	}
	if (positional.size() != 2)
	{
		std::printf("Usage: PartImport <database.pmdb> <parts.csv> [--dry-run]\n");
		return 2;
	}
	const std::string pmdbPath = positional[0];
	const std::string csvPath = positional[1];

	if (!PartManager::MouserClient::hasApiKey())
	{
		std::printf("MOUSER_SEARCH_API is not set in this process's environment - nothing imported.\n");
		return 1;
	}

	std::vector<CsvRow> rows;
	std::string error;
	if (!parseRows(csvPath, rows, error))
	{
		std::printf("CSV error: %s\n", error.c_str());
		return 1;
	}
	std::printf("%zu rows in %s\n", rows.size(), csvPath.c_str());

	std::unique_ptr<PartManager::DatabaseHandle> handle;
	const QFileInfo pmdbInfo(QString::fromStdString(pmdbPath));
	if (pmdbInfo.exists())
	{
		handle.reset(new PartManager::DatabaseHandle(pmdbPath));
		if (!handle->open())
		{
			std::printf("Cannot open database: %s\n", handle->errorMessage().c_str());
			return 1;
		}
	}
	else
	{
		// The .pmdb lives inside the folder it names, so create <parent-of-folder>/<folder-name>.
		const QDir databaseFolder = pmdbInfo.absoluteDir();
		handle = PartManager::DatabaseHandle::createNew(
			databaseFolder.absolutePath().section('/', 0, -2).toStdString(),
			databaseFolder.dirName().toStdString(), error);
		if (!handle)
		{
			std::printf("Cannot create database: %s\n", error.c_str());
			return 1;
		}
		std::printf("Created database %s\n", handle->pmdbPath().c_str());
	}

	// §1b: creating and remembering are separate concerns, so DatabaseHandle does not do this itself.
	// Without it an imported-into database never shows up in the selector's known-databases list.
	PartManager::DatabaseRegistry::add(handle->pmdbPath());

	SQLiteWrapper::SQLite& db = handle->connection();
	PartManager::FileStore store(handle->filestorePath());

	// Both seeds only add what is missing, so this backfills built-in categories and tags that were
	// added after an older database was created. Nothing existing is overwritten.
	PartManager::PartTypeRepository::seedDefaultTypes(db);
	PartManager::TagRepository::seedDefaultTags(db);
	const std::vector<PartManager::Part> existing = PartManager::PartRepository::listParts(db);

	PartManager::MouserClient client;
	int imported = 0;
	int skipped = 0;
	int failed = 0;
	bool firstRequest = true;
	for (const CsvRow& row : rows)
	{
		// Mouser answers HTTP 403 once a key passes ~30 requests in a minute, which on a list this
		// long lands mid-run and looks like a dead key. Measured 2026-09-03: 29 rows through, then
		// 403 for the rest. One request every 2.1 s stays under it.
		if (!firstRequest)
		{
			QThread::msleep(2100);
		}
		firstRequest = false;
		PartManager::MouserSearchResult result = client.searchByPartNumber(row.mouserPartNumber);
		if (!result.ok)
		{
			std::printf("FAIL %-22s %s\n", row.mouserPartNumber.c_str(), result.errorMessage.c_str());
			++failed;
			continue;
		}
		if (result.parts.empty())
		{
			std::printf("FAIL %-22s no results\n", row.mouserPartNumber.c_str());
			++failed;
			continue;
		}
		PartManager::MouserSearchService::rankByMatch(result.parts, row.mouserPartNumber);
		const PartManager::MouserPartPrefill prefill =
			PartManager::MouserSearchService::toPrefill(result.parts.front());

		int existingId = 0;
		PartManager::Part existingPart;
		for (const PartManager::Part& part : existing)
		{
			if (!prefill.part.mpn.empty() && part.mpn == prefill.part.mpn)
			{
				existingId = part.id;
				existingPart = part;
				break;
			}
		}
		if (existingId != 0)
		{
			// An empty field is filled, a non-empty one is never touched: the parametrics come out
			// of Mouser's free-text description, and anything already there was either typed by the
			// user or read from a better source than prose.
			const bool noAttributes = existingPart.attributes.empty()
				|| existingPart.attributes == "{}";
			if (!dryRun && (noAttributes || existingPart.package.empty()))
			{
				if (noAttributes && prefill.part.attributes != "{}")
				{
					existingPart.attributes = prefill.part.attributes;
				}
				if (existingPart.package.empty())
				{
					existingPart.package = prefill.part.package;
				}
				PartManager::PartRepository::updatePart(db, existingPart);
			}
			// Still linked: a database imported before the link was written would otherwise keep an
			// empty Mouser-Nr. forever, and re-running is the only way to backfill it.
			if (!dryRun)
			{
				linkMouser(db, existingId, prefill);
				attachRemote(store, db, existingId, PartManager::PartFileRole::Image,
					prefill.imageUrl, "image");
				attachRemote(store, db, existingId, PartManager::PartFileRole::Datasheet,
					prefill.datasheetUrl, "datasheet");
			}
			std::printf("SKIP %-22s already in database (Mouser-Nr. linked)%s\n",
				row.mouserPartNumber.c_str(),
				noAttributes && prefill.part.attributes != "{}" ? ", attributes filled" : "");
			++skipped;
			continue;
		}

		PartManager::Part part = prefill.part;
		part.partTypeId = findTypeIdByName(db, prefill.suggestedTypeName);
		// stock_qty is written directly here and turned into an 'initial' stock_transaction by the
		// backfill run after the loop - one opening balance per imported part, no per-row bookkeeping.
		part.stockQty = row.stockCount;

		std::printf("%s %-22s type=%-14s qty=%-5d %s\n",
			dryRun ? "DRY " : "OK  ",
			row.mouserPartNumber.c_str(),
			prefill.suggestedTypeName.empty() ? "(none)" : prefill.suggestedTypeName.c_str(),
			part.stockQty,
			part.mpn.c_str());
		if (!prefill.unmappedAttributes.empty())
		{
			std::printf("       unmapped attributes: %zu (left for manual entry)\n",
				prefill.unmappedAttributes.size());
		}
		if (dryRun)
		{
			continue;
		}
		const int partId = PartManager::PartRepository::insertPart(db, part);
		if (partId == 0)
		{
			std::printf("FAIL %-22s insert failed\n", row.mouserPartNumber.c_str());
			++failed;
			continue;
		}
		linkMouser(db, partId, prefill);
		attachRemote(store, db, partId, PartManager::PartFileRole::Image, prefill.imageUrl, "image");
		attachRemote(store, db, partId, PartManager::PartFileRole::Datasheet, prefill.datasheetUrl,
			"datasheet");
		++imported;
	}

	// The schema migration backfills opening balances for databases that predate stock_transaction,
	// but a part imported *after* that migration would otherwise carry a stock_qty with no log behind
	// it forever. Same idempotent call, so re-running the importer never double-counts.
	if (!dryRun)
	{
		int backfilled = PartManager::StockRepository::backfillOpeningBalances(db);
		if (backfilled > 0)
		{
			std::printf("\nopening balances logged for %d part(s)\n", backfilled);
		}
	}

	std::printf("\nimported=%d skipped=%d failed=%d%s\n", imported, skipped, failed,
		dryRun ? " (dry run, nothing written)" : "");
	return failed == 0 ? 0 : 1;
}
