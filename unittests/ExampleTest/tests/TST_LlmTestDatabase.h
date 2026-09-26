#pragma once

#include "llm/PartManager_DatasheetToolset.h"
#include "llm/PartManager_KicadToolset.h"
#include "llm/PartManager_MouserToolset.h"
#include "llm/PartManager_PartToolset.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "database/PartManager_DatabaseHandle.h"
	#include <QDateTime>
	#include <QDir>
	#include <QJsonArray>
	#include <QJsonObject>
	#include <QString>
	#include <filesystem>
	#include <memory>
	#include <string>
	#include <vector>
#endif

// The throwaway database every §14 test case runs against, and the three one-liners that drive a
// tool the way a model does. Shared rather than nested in one suite: a second copy of a
// temp-database helper is a second place to remember that Windows will not delete a file SQLite
// still has open, and the copies drift the moment one of them learns something.
// @see PartManager_LlmTool.h, TST_LlmPartToolset.h, TST_LlmMigration.h

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

// One throwaway database per case, under %TEMP% and never the user's own (ORIENTATION §6). The
// folder name carries the case name and a timestamp so two runs, or two cases inside one run, can
// never land on the same folder. It is removed in the destructor *after* closing the handle:
// Windows will not delete a file SQLite still has open, and a leaked folder per case turns into a
// full temp drive over a few hundred runs.
class ScopedDatabase
{
public:
	explicit ScopedDatabase(const std::string& caseName)
	{
		m_folder = std::filesystem::path(QDir::tempPath().toStdString())
			/ ("PartManager_TST_Llm_" + caseName + "_"
				+ std::to_string(QDateTime::currentMSecsSinceEpoch()));
		std::error_code ec;
		std::filesystem::remove_all(m_folder, ec);
		std::filesystem::create_directories(m_folder, ec);
		m_handle = PartManager::DatabaseHandle::createNew(m_folder.string(), "LlmTools", m_error);
	}

	~ScopedDatabase()
	{
		if (m_handle)
		{
			m_handle->close();
			m_handle.reset();
		}
		std::error_code ec;
		std::filesystem::remove_all(m_folder, ec);
	}

	ScopedDatabase(const ScopedDatabase&) = delete;
	ScopedDatabase& operator=(const ScopedDatabase&) = delete;

	PartManager::DatabaseHandle* handle() const { return m_handle.get(); }
	const std::string& error() const { return m_error; }

	// The context every toolset is built from. Public because MigrationAgent takes one directly.
	PartManager::LlmToolContext context(bool allowWrites = true) const
	{
		PartManager::LlmToolContext built;
		built.database = m_handle.get();
		built.allowWrites = allowWrites;
		return built;
	}

	// The toolset a writing assistant runs with.
	std::vector<PartManager::LlmTool> tools() const
	{
		return PartManager::PartToolset::tools(context(true));
	}

	// The toolset a "just answer questions about my library" assistant runs with (§14).
	std::vector<PartManager::LlmTool> readOnlyTools() const
	{
		return PartManager::PartToolset::tools(context(false));
	}

	// The §6 tools on their own — what the offline Mouser cases drive.
	std::vector<PartManager::LlmTool> mouserTools() const
	{
		return PartManager::MouserToolset::tools(context(true));
	}

	std::vector<PartManager::LlmTool> readOnlyMouserTools() const
	{
		return PartManager::MouserToolset::tools(context(false));
	}

	// The §14 KiCad tools — what TST_LlmKicadToolset drives.
	std::vector<PartManager::LlmTool> kicadTools() const
	{
		return PartManager::KicadToolset::tools(context(true));
	}

	std::vector<PartManager::LlmTool> readOnlyKicadTools() const
	{
		return PartManager::KicadToolset::tools(context(false));
	}

	// The §14g datasheet tools — what TST_LlmDatasheetToolset drives. All three are reads, so a
	// read-only context is the interesting one and `allowWrites` changes nothing about them.
	std::vector<PartManager::LlmTool> datasheetTools(bool allowWrites = true) const
	{
		return PartManager::DatasheetToolset::tools(context(allowWrites));
	}

private:
	std::filesystem::path m_folder;
	std::string m_error;
	std::unique_ptr<PartManager::DatabaseHandle> m_handle;
};

// Exactly what a model does: name a tool, hand it arguments, read the object back. A name no tool
// answers to comes back as an error result rather than a crash, so a typo in a case fails that
// case instead of the whole run.
inline QJsonObject callLlmTool(const std::vector<PartManager::LlmTool>& tools, const char* name,
	const QJsonObject& args = QJsonObject())
{
	const PartManager::LlmTool* tool =
		PartManager::findLlmTool(tools, QString::fromLatin1(name));
	if (tool == nullptr)
	{
		QJsonObject missing;
		missing["status"] = "error";
		missing["message"] = QStringLiteral("no tool named %1").arg(QString::fromLatin1(name));
		return missing;
	}
	return tool->handler(args);
}

inline bool llmResultOk(const QJsonObject& result)
{
	return result.value("status").toString() == QStringLiteral("ok");
}

inline std::string llmResultMessage(const QJsonObject& result)
{
	return result.value("message").toString().toStdString();
}

// The id of a seeded category by name, or 0. Goes through list_categories rather than the
// repository on purpose — a case that needs an id gets it the way the model does.
inline int llmCategoryIdNamed(const std::vector<PartManager::LlmTool>& tools, const QString& name)
{
	const QJsonArray categories =
		callLlmTool(tools, "list_categories").value("categories").toArray();
	for (const QJsonValue& entry : categories)
	{
		if (entry.toObject().value("name").toString() == name)
		{
			return entry.toObject().value("id").toInt();
		}
	}
	return 0;
}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE && SQLITEWRAPPER_LIBRARY_AVAILABLE
