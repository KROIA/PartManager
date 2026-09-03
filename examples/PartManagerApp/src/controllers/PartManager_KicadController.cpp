#include "controllers/PartManager_KicadController.h"

#include "kicad/PartManager_KicadEditTracker.h"
#include "kicad/PartManager_KicadLibTable.h"
#include "kicad/PartManager_KicadSymbolWriter.h"
#include "settings/PartManager_Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>
#include <algorithm>
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

	namespace
	{
		QString readTextFile(const QString& path)
		{
			QFile file(path);
			return file.open(QIODevice::ReadOnly | QIODevice::Text)
				? QString::fromUtf8(file.readAll()) : QString();
		}

		// Writes `text`, keeping a copy of whatever was there as `<name>.bak` first. The backup is
		// the whole safety net for editing files that are KiCad's, not ours: this runs against a
		// table the user's other libraries live in, and a bad merge has to be undoable by hand.
		bool writeTextFileWithBackup(const QString& path, const QString& text, QString* outError)
		{
			if (QFile::exists(path))
			{
				const QString backup = path + QStringLiteral(".bak");
				QFile::remove(backup);
				if (!QFile::copy(path, backup))
				{
					if (outError != nullptr)
					{
						*outError = QObject::tr("Could not back up %1, so it was left alone.").arg(path);
					}
					return false;
				}
			}
			QFile file(path);
			if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
			{
				if (outError != nullptr)
				{
					*outError = QObject::tr("Could not write %1: %2").arg(path, file.errorString());
				}
				return false;
			}
			return file.write(text.toUtf8()) == text.toUtf8().size();
		}

		// PARTMANAGER_KICAD_LIBS in KiCad's own `kicad_common.json` (`environment.vars`). The
		// footprints' `(model ...)` paths are written against it, so a library installed without
		// it shows symbols and footprints and no 3D models — which reads as a broken export
		// rather than a missing setting.
		bool writePathVariable(const QString& configDir, const QString& value, QString* outError)
		{
			const QString path = QDir(configDir).absoluteFilePath(QStringLiteral("kicad_common.json"));
			QJsonObject root = QJsonDocument::fromJson(readTextFile(path).toUtf8()).object();
			QJsonObject environment = root.value(QStringLiteral("environment")).toObject();
			QJsonObject vars = environment.value(QStringLiteral("vars")).toObject();
			// Forward slashes: KiCad stores paths this way on Windows too, and a backslash would
			// have to be escaped in the JSON for no gain.
			vars.insert(QString::fromLatin1(KicadLibraryGenerator::PathVariable),
				QDir::fromNativeSeparators(value));
			environment.insert(QStringLiteral("vars"), vars);
			root.insert(QStringLiteral("environment"), environment);
			return writeTextFileWithBackup(path,
				QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)), outError);
		}
	}

	QStringList KicadController::kicadConfigDirs()
	{
		// %APPDATA%/kicad/<major.minor>/ since KiCad 6; KiCad 5 kept its config directly in
		// %APPDATA%/kicad. Both shapes are offered, newest first, because a machine that has run
		// two majors has both and the newer one is nearly always the one meant.
		//
		// **Not QStandardPaths::GenericConfigLocation**: on Windows that resolves to
		// AppData/**Local** while KiCad writes to AppData/**Roaming**, so it finds nothing on the
		// exact platform this is being written for. The environment variable is read directly and
		// the Qt locations are kept as the fallback for everywhere else.
		QStringList roots;
		const QString appData = qEnvironmentVariable("APPDATA");
		if (!appData.isEmpty())
		{
			roots.append(QDir(appData).absoluteFilePath(QStringLiteral("kicad")));
		}
		for (QStandardPaths::StandardLocation location :
			{ QStandardPaths::GenericConfigLocation, QStandardPaths::GenericDataLocation })
		{
			const QString base = QStandardPaths::writableLocation(location);
			if (!base.isEmpty())
			{
				roots.append(QDir(base).absoluteFilePath(QStringLiteral("kicad")));
			}
		}

		QStringList found;
		for (const QString& root : roots)
		{
			if (QDir(root).exists())
			{
				appendConfigDirsUnder(root, found);
			}
		}
		return found;
	}

	// Split out so kicadConfigDirs() can try several roots without repeating itself.
	void KicadController::appendConfigDirsUnder(const QString& root, QStringList& found)
	{
		QDir rootDir(root);
		QStringList versions = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
		// Name order puts "10.0" before "9.0", so sort numerically on the major.
		std::sort(versions.begin(), versions.end(), [](const QString& a, const QString& b) {
			return a.section('.', 0, 0).toInt() > b.section('.', 0, 0).toInt();
		});
		for (const QString& version : versions)
		{
			const QString candidate = rootDir.absoluteFilePath(version);
			// A version folder is one that holds KiCad's own settings, not any stray directory.
			if (QFile::exists(QDir(candidate).absoluteFilePath(QStringLiteral("kicad_common.json")))
				|| QFile::exists(QDir(candidate).absoluteFilePath(QStringLiteral("sym-lib-table"))))
			{
				found.append(candidate);
			}
		}
		if (QFile::exists(rootDir.absoluteFilePath(QStringLiteral("kicad_common"))))
		{
			found.append(root);   // KiCad 5
		}
	}

	QStringList KicadController::libraryNamesOnDisk() const
	{
		QStringList names;
		const std::string root = libraryPath();
		if (root.empty())
		{
			return names;
		}
		QDir symbols(QDir(QString::fromStdString(root)).absoluteFilePath(QStringLiteral("symbols")));
		for (const QFileInfo& file : symbols.entryInfoList(QStringList{ QStringLiteral("*.kicad_sym") },
			QDir::Files, QDir::Name))
		{
			names.append(file.completeBaseName());
		}
		return names;
	}

	QStringList KicadController::lastLibraryNames() const
	{
		return libraryNamesOnDisk();
	}

	bool KicadController::install(const QString& tableFolder, bool setPathVariable,
		QString* outError) const
	{
		const auto fail = [outError](const QString& message) {
			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		};

		const QString root = QString::fromStdString(libraryPath());
		if (root.isEmpty())
		{
			return fail(QObject::tr("No database is open, so there are no libraries to install."));
		}
		const QStringList names = libraryNamesOnDisk();
		if (names.isEmpty())
		{
			return fail(QObject::tr("No generated libraries were found in %1. Press Generate first.")
				.arg(root));
		}
		QDir folder(tableFolder);
		if (!folder.exists())
		{
			return fail(QObject::tr("%1 does not exist.").arg(tableFolder));
		}

		std::vector<std::string> libraryNames;
		for (const QString& name : names)
		{
			libraryNames.push_back(name.toStdString());
		}
		const std::string variable = KicadLibraryGenerator::PathVariable;

		// Both tables or neither: a symbol library whose footprints are missing is a worse state
		// to leave someone in than not having installed at all.
		const QString symPath = folder.absoluteFilePath(QStringLiteral("sym-lib-table"));
		const QString fpPath = folder.absoluteFilePath(QStringLiteral("fp-lib-table"));
		const QString symText = QString::fromStdString(KicadLibTable::merge(
			readTextFile(symPath).toStdString(), "sym_lib_table",
			KicadLibTable::symbolEntries(libraryNames, variable)));
		const QString fpText = QString::fromStdString(KicadLibTable::merge(
			readTextFile(fpPath).toStdString(), "fp_lib_table",
			KicadLibTable::footprintEntries(libraryNames, variable)));
		if (!writeTextFileWithBackup(symPath, symText, outError)
			|| !writeTextFileWithBackup(fpPath, fpText, outError))
		{
			return false;
		}

		if (!setPathVariable)
		{
			return true;
		}
		// The variable is global whatever the tables were: KiCad has no project-scoped path
		// variables. If the folder we just wrote into is itself a config folder, that is the one
		// to teach; otherwise every KiCad install on the machine gets it, since we cannot know
		// which one will open the project.
		QStringList configDirs;
		if (QFile::exists(folder.absoluteFilePath(QStringLiteral("kicad_common.json"))))
		{
			configDirs.append(folder.absolutePath());
		}
		else
		{
			configDirs = kicadConfigDirs();
		}
		if (configDirs.isEmpty())
		{
			return fail(QObject::tr("The libraries were installed, but KiCad's settings folder was "
				"not found, so %1 has to be added by hand under Preferences → Configure Paths.")
				.arg(QString::fromLatin1(KicadLibraryGenerator::PathVariable)));
		}
		for (const QString& configDir : configDirs)
		{
			if (!writePathVariable(configDir, root, outError))
			{
				return false;
			}
		}
		return true;
	}

	QString KicadController::setupInstructions() const
	{
		const QString path = QString::fromStdString(libraryPath());
		// Kept for the user who would rather wire this up themselves — "Install in KiCad" does
		// exactly these three steps, and someone whose KiCad lives somewhere unusual, or who
		// keeps their tables under version control, wants to see them rather than have them done.
		return QObject::tr(
			"Install in KiCad does this for you. By hand it is:\n\n"
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
