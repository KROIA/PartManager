#include "controllers/PartManager_PartlistController.h"

#include "persistence/PartManager_PartRepository.h"

#include <QObject>
#include <QStringList>
#include <algorithm>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	QString partlistSourceLabel(const std::string& source)
	{
		if (source == PartlistSource::Manual)      { return QObject::tr("Manual"); }
		if (source == PartlistSource::KicadImport) { return QObject::tr("KiCad import"); }
		if (source == PartlistSource::CsvImport)   { return QObject::tr("CSV import"); }
		// `source` is free-form TEXT, so a row written by an older version still reads.
		return QString::fromStdString(source);
	}

	int unresolvedCount(const std::vector<PartlistLine>& lines)
	{
		int count = 0;
		for (const PartlistLine& line : lines)
		{
			if (!line.resolved)
			{
				++count;
			}
		}
		return count;
	}

	QString partlistStatusSummary(const std::vector<PartlistLine>& lines)
	{
		const int unresolved = unresolvedCount(lines);
		int shortLines = 0;
		for (const PartlistLine& line : lines)
		{
			if (line.resolved && line.shortfallQty > 0)
			{
				++shortLines;
			}
		}

		QStringList parts;
		if (unresolved > 0)
		{
			parts.append(QObject::tr("%n line(s) not matched to a part yet", "", unresolved));
		}
		if (shortLines > 0)
		{
			parts.append(QObject::tr("%n line(s) short of the needed quantity", "", shortLines));
		}
		// Nothing to say is the good state; saying "all fine" on every list is noise.
		return parts.join(QObject::tr(", "));
	}

	QString partPickerLabel(const Part& part)
	{
		const QString name = QString::fromStdString(part.name);
		if (part.mpn.empty())
		{
			return name;
		}
		return QStringLiteral("%1 (%2)").arg(name, QString::fromStdString(part.mpn));
	}

	PartlistController::PartlistController(DatabaseHandle* handle)
		: m_handle(handle)
	{
	}

	std::vector<Partlist> PartlistController::partlists() const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return PartlistRepository::listPartlists(m_handle->connection());
		}
#endif
		return {};
	}

	int PartlistController::itemCount(int partlistId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return PartlistRepository::itemCount(m_handle->connection(), partlistId);
		}
#else
		Q_UNUSED(partlistId);
#endif
		return 0;
	}

	bool PartlistController::load(int partlistId, Partlist& outPartlist) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return PartlistRepository::findPartlist(m_handle->connection(), partlistId, outPartlist);
		}
#else
		Q_UNUSED(partlistId);
		Q_UNUSED(outPartlist);
#endif
		return false;
	}

	int PartlistController::create(const Partlist& partlist) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return PartlistRepository::insertPartlist(m_handle->connection(), partlist);
		}
#else
		Q_UNUSED(partlist);
#endif
		return NoPartlistId;
	}

	bool PartlistController::save(const Partlist& partlist) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return PartlistRepository::updatePartlist(m_handle->connection(), partlist);
		}
#else
		Q_UNUSED(partlist);
#endif
		return false;
	}

	bool PartlistController::remove(int partlistId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return PartlistRepository::deletePartlist(m_handle->connection(), partlistId);
		}
#else
		Q_UNUSED(partlistId);
#endif
		return false;
	}

	std::vector<PartlistLine> PartlistController::lines(int partlistId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return PartlistRepository::lines(m_handle->connection(), partlistId);
		}
#else
		Q_UNUSED(partlistId);
#endif
		return {};
	}

	bool PartlistController::saveItems(int partlistId, const std::vector<PartlistItem>& items) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return PartlistRepository::saveItems(m_handle->connection(), partlistId, items);
		}
#else
		Q_UNUSED(partlistId);
		Q_UNUSED(items);
#endif
		return false;
	}

	std::vector<Part> PartlistController::allParts() const
	{
		std::vector<Part> parts;
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			parts = PartRepository::listParts(m_handle->connection());
		}
#endif
		std::sort(parts.begin(), parts.end(),
			[](const Part& a, const Part& b) { return a.name < b.name; });
		return parts;
	}

}
