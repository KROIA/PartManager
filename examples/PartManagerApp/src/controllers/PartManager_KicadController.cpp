#include "controllers/PartManager_KicadController.h"

#include "kicad/PartManager_KicadEditTracker.h"
#include "kicad/PartManager_KicadSymbolWriter.h"
#include "settings/PartManager_Settings.h"

#include <QObject>
#include <filesystem>
#include <fstream>
#include <sstream>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	KicadController::KicadController(DatabaseHandle* handle)
		: m_handle(handle)
	{
	}

	std::string KicadController::libraryPath() const
	{
		const AppPreferences preferences = Settings::getPreferences();
		if (!preferences.kicadLibraryPath.empty())
		{
			return preferences.kicadLibraryPath;
		}
		// The database folder already has a kicad_libs/ (§1 lays it down at creation), so an
		// unconfigured install still generates somewhere sensible.
		return m_handle != nullptr ? m_handle->kicadLibsPath() : std::string();
	}

	QString KicadController::setupInstructions() const
	{
		const QString path = QString::fromStdString(libraryPath());
		// Spelled out because it is genuinely one-time and genuinely manual: KiCad's global
		// library tables are its own config, and nothing here should be editing them.
		return QObject::tr(
			"One-time KiCad setup — after this, every regeneration is invisible to KiCad:\n\n"
			"1. KiCad → Preferences → Configure Paths: add the variable\n"
			"     %1  =  %2\n\n"
			"2. Preferences → Manage Symbol Libraries → Global: add\n"
			"     ${%1}/symbols/<Category>.kicad_sym  for each library below\n\n"
			"3. Preferences → Manage Footprint Libraries → Global: add\n"
			"     ${%1}/footprints/<Category>.pretty\n\n"
			"The generated partmanager-sym-lib-table and partmanager-fp-lib-table in the folder "
			"above list every entry, so they can be pasted in rather than typed.")
			.arg(QString::fromLatin1(KicadLibraryGenerator::PathVariable))
			.arg(path.isEmpty() ? QObject::tr("(no database open)") : path);
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		SQLiteWrapper::SQLite* connectionOf(DatabaseHandle* handle)
		{
			return (handle != nullptr && handle->isOpen()) ? &handle->connection() : nullptr;
		}
	}

	KicadGenerationResult KicadController::generate(const std::vector<std::string>& forcePaths) const
	{
		KicadGenerationResult result;
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db)
		{
			result.errorMessage = QObject::tr("No database is open.").toStdString();
			return result;
		}
		return KicadLibraryGenerator::generate(*db, libraryPath(), m_handle->filestorePath(),
			forcePaths);
	}

	bool KicadController::rebaseline(const std::string& targetPath) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db)
		{
			return false;
		}

		// The on-disk content is read back so the baseline records what is actually there. For a
		// symbol the target is "Library.kicad_sym:Name" and the content is that one block, not
		// the whole file — the tracker works per symbol because they share a file (§5a).
		const size_t colon = targetPath.rfind(':');
		const bool isSymbol = colon != std::string::npos && colon > 2;
		std::string content;
		if (isSymbol)
		{
			const std::filesystem::path library = std::filesystem::path(libraryPath()) / "symbols"
				/ targetPath.substr(0, colon);
			std::ifstream in(library, std::ios::binary);
			std::ostringstream buffer;
			buffer << in.rdbuf();
			const std::string wanted = targetPath.substr(colon + 1);
			for (const std::string& block : KicadSymbolWriter::splitSymbols(buffer.str()))
			{
				if (KicadSymbolWriter::symbolNameOf(block) == wanted)
				{
					content = block;
					break;
				}
			}
		}
		else
		{
			std::ifstream in(targetPath, std::ios::binary);
			std::ostringstream buffer;
			buffer << in.rdbuf();
			content = buffer.str();
		}

		if (content.empty())
		{
			return false;
		}
		return KicadEditTracker::rebaseline(*db, targetPath, content);
	}

#else

	KicadGenerationResult KicadController::generate(const std::vector<std::string>&) const
	{
		return KicadGenerationResult();
	}

	bool KicadController::rebaseline(const std::string&) const
	{
		return false;
	}

#endif

}
