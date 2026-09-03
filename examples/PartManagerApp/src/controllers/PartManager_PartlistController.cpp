#include "controllers/PartManager_PartlistController.h"

#include "filestore/PartManager_FileStore.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_SellerRepository.h"

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

	std::string mergeDesignators(const std::string& first, const std::string& second)
	{
		QStringList merged;
		const QString both = QString::fromStdString(first) + QLatin1Char(',')
			+ QString::fromStdString(second);
		for (const QString& piece : both.split(QLatin1Char(','), QString::SkipEmptyParts))
		{
			// Both separators are in use in the wild — KiCad exports semicolons, a hand-typed
			// list is usually commas, and one list can hold both.
			for (const QString& designator : piece.split(QLatin1Char(';'), QString::SkipEmptyParts))
			{
				const QString trimmed = designator.trimmed();
				// Case-sensitive: `R1` and `r1` are the same placement to a human, but renaming
				// the user's reference is not this function's business.
				if (!trimmed.isEmpty() && !merged.contains(trimmed))
				{
					merged.append(trimmed);
				}
			}
		}
		return merged.join(QStringLiteral(", ")).toStdString();
	}

	bool mergeDuplicateItems(std::vector<PartlistItem>& items)
	{
		bool merged = false;
		for (size_t keep = 0; keep < items.size(); ++keep)
		{
			if (items[keep].partId == NoPartId)
			{
				continue;
			}
			for (size_t other = keep + 1; other < items.size();)
			{
				if (items[other].partId != items[keep].partId)
				{
					++other;
					continue;
				}
				items[keep].quantityPerUnit += items[other].quantityPerUnit;
				items[keep].designators =
					mergeDesignators(items[keep].designators, items[other].designators);
				items.erase(items.begin() + static_cast<long long>(other));
				merged = true;
			}
		}
		return merged;
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

	std::map<int, QString> PartlistController::imagePaths() const
	{
		std::map<int, QString> byPart;
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			FileStore store(m_handle->filestorePath());
			for (const PartFile& file :
				PartRepository::listFilesWithRole(m_handle->connection(), PartFileRole::Image))
			{
				// Newest wins, the single-slot rule FileStore::roleFile() applies.
				byPart[file.partId] = QString::fromStdString(store.absolutePath(file.relativePath));
			}
		}
#endif
		return byPart;
	}

	std::vector<PartSellerLink> PartlistController::allSellerLinks() const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return SellerRepository::allLinks(m_handle->connection());
		}
#endif
		return std::vector<PartSellerLink>();
	}

}
