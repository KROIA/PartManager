#include "kicad/PartManager_KicadLibraryGenerator.h"
#include "kicad/PartManager_KicadGeometry.h"
#include "kicad/PartManager_KicadLibTable.h"
#include "kicad/PartManager_KicadSymbolWriter.h"
#include "filestore/PartManager_FileStore.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_SellerRepository.h"
#include "PartManager_global.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	const char* const KicadLibraryGenerator::PathVariable = "PARTMANAGER_KICAD_LIBS";

	namespace
	{
		std::string readFile(const std::filesystem::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
			{
				return std::string();
			}
			std::ostringstream buffer;
			buffer << in.rdbuf();
			return buffer.str();
		}

		bool writeFile(const std::filesystem::path& path, const std::string& contents)
		{
			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				return false;
			}
			out << contents;
			return out.good();
		}
	}

	std::string KicadGenerationResult::summary() const
	{
		if (!ok)
		{
			return errorMessage;
		}
		std::string text = std::to_string(symbolsGenerated) + " symbol(s) in "
			+ std::to_string(librariesWritten) + " library file(s)";
		if (symbolsFromAttachment > 0)
		{
			text += " (" + std::to_string(symbolsFromAttachment) + " from the part's own KiCad file)";
		}
		if (symbolsDerivedFromFootprint > 0)
		{
			text += " (" + std::to_string(symbolsDerivedFromFootprint)
				+ " derived from the footprint's pads)";
		}
		if (symbolsPreserved > 0)
		{
			// Named first among the caveats because it is the one the user has to act on.
			text += ", " + std::to_string(symbolsPreserved) + " left alone because they were "
				"edited in KiCad";
		}
		if (symbolsSyncedBack + footprintsSyncedBack > 0)
		{
			text += ", " + std::to_string(symbolsSyncedBack + footprintsSyncedBack)
				+ " edit(s) written back into the parts";
		}
		if (footprintsCopied > 0)
		{
			text += ", " + std::to_string(footprintsCopied) + " footprint(s)";
		}
		if (modelsCopied > 0)
		{
			text += ", " + std::to_string(modelsCopied) + " 3D model(s)";
		}
		if (!skippedForNoCategory.empty())
		{
			text += ", " + std::to_string(skippedForNoCategory.size())
				+ " part(s) skipped for having no KiCad category";
		}
		if (!skippedForNoKicadFiles.empty())
		{
			text += ", " + std::to_string(skippedForNoKicadFiles.size())
				+ " part(s) skipped for having no KiCad symbol or footprint";
		}
		if (!skippedForNoSymbol.empty())
		{
			text += ", " + std::to_string(skippedForNoSymbol.size())
				+ " part(s) with a footprint but no symbol";
		}
		if (staleItemsRemoved > 0)
		{
			text += ", " + std::to_string(staleItemsRemoved) + " no longer generated and removed";
		}
		return text;
	}

	std::string KicadLibraryGenerator::libraryNameFor(const std::string& category)
	{
		std::string name;
		for (char c : category)
		{
			// KiCad's library nickname is used as `Nickname:Symbol`, so a colon would split it in
			// two, and a space makes the lib-table entry ambiguous.
			name += (c == ' ' || c == ':' || c == '/' || c == '\\') ? '_' : c;
		}
		return name.empty() ? std::string("PartManager") : name;
	}

	// Both tables are built by the same code that installs into KiCad's own, so the file a user
	// copies from and the rows "Install in KiCad" writes can never drift apart — including the
	// nickname prefix, which is the piece that would be easy to add in only one of the two.
	std::string KicadLibraryGenerator::symLibTable(const std::vector<std::string>& libraryNames,
		const std::string& pathVariable)
	{
		return KicadLibTable::merge(std::string(), "sym_lib_table",
			KicadLibTable::symbolEntries(libraryNames, pathVariable));
	}

	std::string KicadLibraryGenerator::fpLibTable(const std::vector<std::string>& libraryNames,
		const std::string& pathVariable)
	{
		return KicadLibTable::merge(std::string(), "fp_lib_table",
			KicadLibTable::footprintEntries(libraryNames, pathVariable));
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		// One part, resolved into everything the symbol needs. Built here so the writer stays
		// database-free.
		KicadSymbolSpec specFor(SQLiteWrapper::SQLite& db, const Part& part,
			const std::string& typeName, const std::string& filestorePath,
			const std::filesystem::path& modelsDir, int& modelsCopied)
		{
			KicadSymbolSpec spec;
			spec.name = part.name;
			spec.baseSymbol = KicadSymbolWriter::baseSymbolForType(typeName);
			spec.reference = KicadSymbolWriter::referenceForBase(spec.baseSymbol);
			// The MPN is what belongs on a schematic; the part's own name is a fallback for a
			// hand-made part that has none.
			spec.value = part.mpn.empty() ? part.name : part.mpn;
			spec.description = part.description;
			spec.keywords = typeName;
			spec.partId = part.id;
			spec.mouserPartNumber = SellerRepository::mouserPartNumber(db, part.id);

			for (const PartFile& file : PartRepository::listFiles(db, part.id))
			{
				const PartFileRole role = partFileRoleFromString(file.role);
				const std::filesystem::path stored =
					std::filesystem::path(filestorePath) / file.relativePath;
				if (role == PartFileRole::Datasheet && spec.datasheet.empty())
				{
					// A path rather than a URL: the file is already local, and the URL it came
					// from is not stored on the file row.
					spec.datasheet = stored.string();
				}
				else if (role == PartFileRole::Kicad3DModel)
				{
					// Copied into kicad_libs/3dmodels/ under its original name so the path in the
					// symbol is readable, and referenced through the same env var the lib tables
					// use — an absolute filestore path would not survive moving the database.
					const std::string name = file.originalFilename.empty()
						? std::filesystem::path(file.relativePath).filename().string()
						: file.originalFilename;
					std::error_code error;
					std::filesystem::create_directories(modelsDir, error);
					std::filesystem::copy_file(stored, modelsDir / name,
						std::filesystem::copy_options::overwrite_existing, error);
					if (!error)
					{
						++modelsCopied;
					}
					spec.model3DPath = "${" + std::string(KicadLibraryGenerator::PathVariable)
						+ "}/3dmodels/" + name;
				}
			}
			return spec;
		}

		// Which of a library file's symbols is *the part's*.
		//
		// **Not simply the first.** A `.kicad_sym` PartManager wrote starts with the embedded
		// base symbols every generated symbol `(extends ...)`, so the first block is `PM_R` and
		// taking it would silently replace the part with a bare resistor outline. A vendor's
		// file, by contrast, holds exactly one symbol under its own name.
		//
		// So: the block named after the part if it is there, otherwise the first block that is
		// not one of the bases.
		std::string pickPartSymbol(const std::vector<std::string>& blocks,
			const std::string& symbolName)
		{
			std::vector<std::string> baseNames;
			for (const std::string& base : KicadSymbolWriter::baseSymbolBlocks())
			{
				baseNames.push_back(KicadSymbolWriter::symbolNameOf(base));
			}

			std::string firstNonBase;
			for (const std::string& block : blocks)
			{
				const std::string name = KicadSymbolWriter::symbolNameOf(block);
				if (name == symbolName)
				{
					return block;
				}
				if (firstNonBase.empty()
					&& std::find(baseNames.begin(), baseNames.end(), name) == baseNames.end())
				{
					firstNonBase = block;
				}
			}
			return firstNonBase;
		}

		// §5c: the symbol block for a part, taken from its attached `.kicad_sym` when it has one.
		// The attachment is a whole library file holding a single symbol, so it is split and the
		// first block taken; the block is then renamed to the part's symbol name and given
		// PartManager's own fields, so a vendor symbol keeps its real pins and graphics while
		// still resolving its footprint and pointing back at the part row.
		//
		// Empty when the part has no attachment or the file will not parse — the caller falls
		// back to the generated `(extends ...)` symbol rather than writing nothing.
		std::string symbolFromAttachment(SQLiteWrapper::SQLite& db, const Part& part,
			const std::string& filestorePath, const std::string& symbolName,
			const std::string& footprintRef, const KicadSymbolSpec& spec)
		{
			PartFile file;
			if (!FileStore::roleFile(db, part.id, PartFileRole::KicadSymbol, file))
			{
				return std::string();
			}
			const std::string text =
				readFile(std::filesystem::path(filestorePath) / file.relativePath);
			const std::string picked = pickPartSymbol(
				KicadSymbolWriter::splitSymbols(text), symbolName);
			if (picked.empty())
			{
				return std::string();
			}

			std::string block = KicadSymbolWriter::renamedSymbol(picked, symbolName);
			// The vendor writes a bare footprint name ("SOP65P640X110-16N"), which KiCad cannot
			// resolve without a library nickname in front of it.
			if (!footprintRef.empty())
			{
				block = KicadSymbolWriter::withProperty(block, "Footprint", footprintRef);
			}
			// The round trip back into this app, and the two fields the user actually looks for.
			block = KicadSymbolWriter::withProperty(block, "PM_PartID",
				spec.partId > 0 ? std::to_string(spec.partId) : std::string());
			if (!spec.mouserPartNumber.empty())
			{
				block = KicadSymbolWriter::withProperty(block, "Mouser P/N", spec.mouserPartNumber);
			}
			if (!spec.datasheet.empty())
			{
				block = KicadSymbolWriter::withProperty(block, "Datasheet", spec.datasheet);
			}
			if (!spec.model3DPath.empty())
			{
				block = KicadSymbolWriter::withProperty(block, "PM_3DModel", spec.model3DPath);
			}
			return block;
		}

		// §5c: writes `content` into the part's slot, but only when it differs from what is
		// already there — an unconditional write would make a new filestore row on every
		// regeneration and the "2 copies" would drift apart in metadata if not in bytes.
		// Returns true when something was actually written.
		bool syncAttachment(SQLiteWrapper::SQLite& db, FileStore& store, int partId,
			PartFileRole role, const std::string& content, const std::string& fileName)
		{
			if (content.empty())
			{
				return false;
			}
			PartFile existing;
			if (FileStore::roleFile(db, partId, role, existing)
				&& existing.contentHash == FileStore::hashBytes(content))
			{
				return false;   // already identical, which is the steady state
			}
			return store.replaceRoleFileBytes(db, partId, role, content, fileName) != 0;
		}
	}

	KicadGenerationResult KicadLibraryGenerator::generate(SQLiteWrapper::SQLite& db,
		const std::string& kicadLibsPath, const std::string& filestorePath,
		const std::vector<std::string>& forcePaths)
	{
		KicadGenerationResult result;
		if (kicadLibsPath.empty())
		{
			result.errorMessage = "No KiCad library folder is configured.";
			return result;
		}

		KicadEditTracker::createSchema(db);

		// §5c writes back into the part's attachments, so the generator needs the store the
		// editor uses — the same single-slot rule, from the same place.
		FileStore store(filestorePath);

		const std::filesystem::path root(kicadLibsPath);
		const std::filesystem::path symbolsDir = root / "symbols";
		const std::filesystem::path footprintsDir = root / "footprints";
		const std::filesystem::path modelsDir = root / "3dmodels";

		// Category -> the symbol blocks that library will hold, in part order.
		std::map<std::string, std::vector<std::string>> symbolsByCategory;
		// Category -> the target paths generated into it, so a preserved symbol can be matched.
		std::map<std::string, std::vector<std::pair<std::string, int>>> targetsByCategory;
		// Every artifact of a part that qualified this run, whether it ended up regenerated or
		// preserved. Anything tracked and *not* in here belongs to a part that stopped qualifying.
		std::set<std::string> currentTargets;
		// Libraries with at least one qualifying part, so a library that lost its last one can be
		// told apart from one that only holds footprints.
		std::set<std::string> usedLibraries;

		for (const PartType& type : PartTypeRepository::listTypes(db))
		{
			// A child category (e.g. "Neopixel 5050 WS2812B" under "LED") inherits relevance from
			// its nearest kicad_relevant ancestor — the tree is flattened per root category so every
			// exportable part underneath it is generated, not just parts on the flagged type itself.
			if (!PartTypeRepository::effectiveKicadRelevant(db, type.id))
			{
				continue;
			}
			// §2b: a type with no category of its own inherits its nearest ancestor's.
			const std::string category = PartTypeRepository::effectiveKicadCategory(db, type.id);

			for (const Part& part : PartRepository::listParts(db, type.id))
			{
				if (category.empty())
				{
					// Counted, not dropped: otherwise the part simply never appears in KiCad and
					// there is nothing to explain why.
					result.skippedForNoCategory.push_back(part.name);
					continue;
				}
				const std::string libraryName = libraryNameFor(category);
				const std::string symbolName = KicadSymbolWriter::sanitizeSymbolName(part.name);
				const std::string targetPath = libraryName + ".kicad_sym:" + symbolName;

				// **Nothing is invented.** A part is generated from the KiCad files it actually
				// has: neither attached means it is not generated at all, symbol only means symbol
				// only, footprint only means the footprint and no symbol. A placeholder symbol is
				// worse than an absent part — it places silently in a schematic and is wrong on
				// the board. Whatever an earlier run left for a part that no longer qualifies is
				// removed by the stale pass below.
				PartFile footprintRow;
				const bool hasFootprint =
					FileStore::roleFile(db, part.id, PartFileRole::KicadFootprint, footprintRow);
				PartFile symbolRow;
				const bool hasSymbol =
					FileStore::roleFile(db, part.id, PartFileRole::KicadSymbol, symbolRow);
				if (!hasSymbol && !hasFootprint)
				{
					result.skippedForNoKicadFiles.push_back(part.name);
					continue;
				}
				usedLibraries.insert(libraryName);
				// The library exists even when this category only ever produces footprints, so the
				// fp-lib-table entry KiCad needs is written and its sym-lib-table twin resolves.
				symbolsByCategory.emplace(libraryName, std::vector<std::string>());

				KicadSymbolSpec spec = specFor(db, part, type.name, filestorePath, modelsDir,
					result.modelsCopied);
				// The footprint the symbol should reference — the .pretty entry is written under
				// the symbol's name.
				if (hasFootprint)
				{
					// **The nickname, not the file name.** KiCad resolves a `Footprint` property
					// through the fp-lib-table, where the library is called
					// `PartManager_ICs` — the `ICs.pretty` folder name never appears to it. Using
					// the file name produces a reference that looks right in the symbol's
					// properties and cannot be resolved.
					spec.footprint = KicadLibTable::nicknameFor(libraryName) + ":" + symbolName;
				}

				// Read once: the footprint is both where the symbol's pins may come from and what
				// gets copied into the `.pretty` below.
				const std::string footprintText = hasFootprint
					? readFile(std::filesystem::path(filestorePath) / footprintRow.relativePath)
					: std::string();

				// §5c: the symbol is the part's own `.kicad_sym`, or one derived from the pads of
				// its footprint, or there is no symbol.
				std::string block = symbolFromAttachment(db, part, filestorePath, symbolName,
					spec.footprint, spec);
				bool derived = false;
				if (block.empty() && hasFootprint)
				{
					// **Still nothing invented.** A pad carries a pin number, so a symbol built
					// from the pads connects to the copper the part actually has — which is
					// exactly what the placeholder this replaced never did. A footprint whose
					// pads carry no numbers derives nothing.
					block = KicadSymbolWriter::derivedSymbolBlock(spec,
						KicadSymbolWriter::pinsFromFootprint(footprintText));
					derived = !block.empty();
				}
				if (block.empty())
				{
					// No symbol attached and no pads to read: an attachment that will not parse is
					// a problem to report, and a footprint of nothing but mounting holes has no
					// pins to offer. Neither is a reason to invent any.
					result.skippedForNoSymbol.push_back(part.name);
				}
				else
				{
					if (derived) { ++result.symbolsDerivedFromFootprint; }
					else         { ++result.symbolsFromAttachment; }
					symbolsByCategory[libraryName].push_back(block);
					targetsByCategory[libraryName].push_back({ targetPath, part.id });
					currentTargets.insert(targetPath);
				}

				// Footprints are per-file, so they are a straight hash check against the file.
				if (hasFootprint)
				{
					const std::filesystem::path target = footprintsDir
						/ (libraryName + ".pretty")
						/ (symbolName + ".kicad_mod");
					currentTargets.insert(target.string());
					const std::string onDisk = readFile(target);
					const KicadItemState state = KicadEditTracker::stateOf(db, target.string(), onDisk);
					const bool forced = std::find(forcePaths.begin(), forcePaths.end(),
						target.string()) != forcePaths.end();
					if (state == KicadItemState::EditedExternally && !forced)
					{
						// §5c: the edit in KiCad wins and goes back into the part's attachment, so
						// the two copies agree again and the edit outlives `kicad_libs/`.
						if (syncAttachment(db, store, part.id, PartFileRole::KicadFootprint, onDisk,
							footprintRow.originalFilename.empty()
								? symbolName + ".kicad_mod" : footprintRow.originalFilename))
						{
							++result.footprintsSyncedBack;
							KicadEditTracker::rebaseline(db, target.string(), onDisk);
						}
						KicadSkippedItem skipped;
						skipped.partId = part.id;
						skipped.partName = part.name;
						skipped.targetPath = target.string();
						result.preserved.push_back(skipped);
						continue;
					}
					// The vendor's footprint names the vendor's own 3D-model path, which resolves
					// to nothing on this machine. The model file itself has already been copied
					// into kicad_libs/3dmodels/ by specFor(), so the copy is pointed at that —
					// otherwise KiCad opens the footprint and shows no model at all.
					const std::string contents =
						KicadGeometry::withModelPath(footprintText, spec.model3DPath);
					if (!contents.empty() && writeFile(target, contents))
					{
						KicadEditTracker::record(db, part.id, KicadItemType::Footprint,
							target.string(), contents);
						++result.footprintsCopied;
					}
				}
			}
		}

		// What an earlier run generated for a part that no longer qualifies — its KiCad files were
		// detached, its category went away, it was renamed, or the part is gone. Left alone the row
		// would report a phantom "modified externally" on every future run, because that part will
		// never produce a fresh baseline for it again, and the library would keep an entry nothing
		// points at. So the artifact goes and the row is forgotten — **unless its hash says a human
		// edited it**, which is never ours to delete: that one stays, stays tracked, and is
		// surfaced as `stale` for the user to decide on (force-regenerate removes it).
		std::map<std::string, std::vector<KicadGeneratedItem>> staleSymbolsByCategory;
		for (const KicadGeneratedItem& item : KicadEditTracker::allItems(db))
		{
			if (currentTargets.count(item.targetPath) != 0)
			{
				continue;
			}
			if (item.itemType == KicadItemType::Symbol)
			{
				// Deferred to the write loop below: a symbol is removed by leaving it out of the
				// file that is rebuilt there, not by touching anything here.
				const size_t marker = item.targetPath.find(".kicad_sym:");
				if (marker == std::string::npos)
				{
					continue;
				}
				const std::string library = item.targetPath.substr(0, marker);
				staleSymbolsByCategory[library].push_back(item);
				symbolsByCategory.emplace(library, std::vector<std::string>());
				continue;
			}

			// A footprint is its own file, so dropping it means deleting it.
			const std::filesystem::path target(item.targetPath);
			const std::string onDisk = readFile(target);
			const bool forced = std::find(forcePaths.begin(), forcePaths.end(), item.targetPath)
				!= forcePaths.end();
			if (KicadEditTracker::stateOf(item.lastGeneratedHash, onDisk, true)
				== KicadItemState::EditedExternally && !forced)
			{
				KicadSkippedItem skipped;
				skipped.partId = item.partId;
				skipped.partName = target.filename().string();
				skipped.targetPath = item.targetPath;
				skipped.stale = true;
				result.preserved.push_back(skipped);
				continue;
			}
			std::error_code error;
			std::filesystem::remove(target, error);
			KicadEditTracker::forget(db, item.targetPath);
			++result.staleItemsRemoved;
		}

		std::vector<std::string> libraryNames;
		for (const std::pair<const std::string, std::vector<std::string>>& entry : symbolsByCategory)
		{
			const std::string& libraryName = entry.first;
			const std::filesystem::path libraryPath = symbolsDir / (libraryName + ".kicad_sym");

			// The existing file's symbols, byte for byte, so a hand-edited one can be carried
			// across without being re-serialised (which would itself count as an edit next run).
			std::map<std::string, std::string> onDiskSymbols;
			for (const std::string& block : KicadSymbolWriter::splitSymbols(readFile(libraryPath)))
			{
				onDiskSymbols[KicadSymbolWriter::symbolNameOf(block)] = block;
			}

			std::vector<std::string> blocks;
			const std::vector<std::pair<std::string, int>>& targets = targetsByCategory[libraryName];
			for (size_t i = 0; i < entry.second.size(); ++i)
			{
				const std::string& targetPath = targets[i].first;
				const int partId = targets[i].second;
				const std::string symbolName = targetPath.substr(targetPath.find(':') + 1);
				const std::string existing = onDiskSymbols.count(symbolName) != 0
					? onDiskSymbols[symbolName] : std::string();

				const KicadItemState state = KicadEditTracker::stateOf(db, targetPath, existing);
				const bool forced = std::find(forcePaths.begin(), forcePaths.end(), targetPath)
					!= forcePaths.end();
				if (state == KicadItemState::EditedExternally && !forced)
				{
					// The user's version wins and goes back into the file unchanged...
					blocks.push_back(existing);
					// ...and into the part's own attachment, so the edit survives `kicad_libs/`
					// being deleted and the next run splices the edited symbol straight back in
					// (§5c). The attachment is stored as a whole one-symbol library, which is
					// exactly what symbolFromAttachment() reads and what KiCad opens directly.
					if (syncAttachment(db, store, partId, PartFileRole::KicadSymbol,
						KicadSymbolWriter::library({ existing }), symbolName + ".kicad_sym"))
					{
						++result.symbolsSyncedBack;
						// Re-baselined because the edit is now *ours* — it is what the attachment
						// holds, so the next run regenerates the same bytes and stops reporting it.
						KicadEditTracker::rebaseline(db, targetPath, existing);
					}
					KicadSkippedItem skipped;
					skipped.partId = partId;
					skipped.partName = symbolName;
					skipped.targetPath = targetPath;
					result.preserved.push_back(skipped);
					++result.symbolsPreserved;
					continue;
				}

				blocks.push_back(entry.second[i]);
				KicadEditTracker::record(db, partId, KicadItemType::Symbol, targetPath,
					entry.second[i]);
				++result.symbolsGenerated;
			}

			// Symbols of parts that stopped qualifying. Leaving one out of `blocks` is what removes
			// it, since the file is rebuilt from `blocks` every run.
			for (const KicadGeneratedItem& item : staleSymbolsByCategory[libraryName])
			{
				const std::string symbolName = item.targetPath.substr(item.targetPath.find(':') + 1);
				const std::string existing = onDiskSymbols.count(symbolName) != 0
					? onDiskSymbols[symbolName] : std::string();
				const bool forced = std::find(forcePaths.begin(), forcePaths.end(), item.targetPath)
					!= forcePaths.end();
				if (KicadEditTracker::stateOf(item.lastGeneratedHash, existing, true)
					== KicadItemState::EditedExternally && !forced)
				{
					// Someone's own work, and no part behind it any more. Not deleted, not synced
					// back into a part that no longer wants it — carried across and reported.
					blocks.push_back(existing);
					KicadSkippedItem skipped;
					skipped.partId = item.partId;
					skipped.partName = symbolName;
					skipped.targetPath = item.targetPath;
					skipped.stale = true;
					result.preserved.push_back(skipped);
					++result.symbolsPreserved;
					continue;
				}
				KicadEditTracker::forget(db, item.targetPath);
				++result.staleItemsRemoved;
			}

			if (blocks.empty() && usedLibraries.count(libraryName) == 0)
			{
				// The library lost its last part: the file goes rather than sitting there empty and
				// listed in a table for a category that no longer exists.
				std::error_code error;
				std::filesystem::remove(libraryPath, error);
				continue;
			}
			if (!writeFile(libraryPath, KicadSymbolWriter::library(blocks)))
			{
				result.errorMessage = "Could not write " + libraryPath.string();
				return result;
			}
			libraryNames.push_back(libraryName);
			++result.librariesWritten;
		}

		// Written every run, including when nothing generated: a table listing a library that no
		// longer exists makes KiCad complain on every launch.
		writeFile(root / "partmanager-sym-lib-table", symLibTable(libraryNames, PathVariable));
		writeFile(root / "partmanager-fp-lib-table", fpLibTable(libraryNames, PathVariable));
		result.libraryNames = libraryNames;

		result.ok = true;
		return result;
	}

#endif

}
