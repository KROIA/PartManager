#include "llm/PartManager_LlmTool.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"
#include <Client.h>

namespace PartManager
{

	bool LlmToolContext::isUsable() const
	{
		return database != nullptr && database->isOpen();
	}

	QJsonObject llmOk(const QJsonObject& payload)
	{
		QJsonObject result = payload;
		result.insert(QStringLiteral("status"), QStringLiteral("ok"));
		return result;
	}

	QJsonObject llmError(const QString& message, const QJsonObject& extra)
	{
		QJsonObject result = extra;
		result.insert(QStringLiteral("status"), QStringLiteral("error"));
		result.insert(QStringLiteral("message"), message);
		return result;
	}

	const LlmTool* findLlmTool(const std::vector<LlmTool>& tools, const QString& name)
	{
		for (const LlmTool& tool : tools)
		{
			if (tool.schema.name() == name)
				return &tool;
		}
		return nullptr;
	}

	void registerLlmTools(QtLLM::Client& client, const std::vector<LlmTool>& tools)
	{
		for (const LlmTool& tool : tools)
			client.registerTool(tool.schema, tool.handler);
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
