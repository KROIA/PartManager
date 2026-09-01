#include "controllers/PartManager_StockController.h"

#include "persistence/PartManager_StockRepository.h"

#include <QObject>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	std::vector<int> runningQuantities(const std::vector<StockTransaction>& history)
	{
		// The log starts from nothing — the opening balance is itself a transaction (§3), so
		// the running sum never needs a seed value from anywhere else.
		std::vector<int> result;
		result.reserve(history.size());
		int running = 0;
		for (const StockTransaction& transaction : history)
		{
			running += transaction.deltaQty;
			result.push_back(running);
		}
		return result;
	}

	QString stockReasonLabel(const std::string& reason)
	{
		if (reason == StockReason::Restock)          { return QObject::tr("Restock"); }
		if (reason == StockReason::CheckoutPartlist) { return QObject::tr("Taken out"); }
		if (reason == StockReason::ManualAdjust)     { return QObject::tr("Manual adjustment"); }
		if (reason == StockReason::Loss)             { return QObject::tr("Lost or broken"); }
		if (reason == StockReason::Initial)          { return QObject::tr("Opening balance"); }
		// `reason` is free-form TEXT, so a row written by an older version still reads.
		return QString::fromStdString(reason);
	}

	StockController::StockController(DatabaseHandle* handle)
		: m_handle(handle)
	{
	}

	int StockController::quantity(int partId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return StockRepository::currentQuantity(m_handle->connection(), partId);
		}
#else
		Q_UNUSED(partId);
#endif
		return 0;
	}

	bool StockController::restock(int partId, int quantity, const QString& note) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return StockRepository::restock(m_handle->connection(), partId, quantity,
				note.toStdString()) != NoStockTransactionId;
		}
#else
		Q_UNUSED(partId); Q_UNUSED(quantity); Q_UNUSED(note);
#endif
		return false;
	}

	bool StockController::takeOut(int partId, int quantity, const QString& note,
		const std::string& reason) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return StockRepository::takeOut(m_handle->connection(), partId, quantity,
				note.toStdString(), reason) != NoStockTransactionId;
		}
#else
		Q_UNUSED(partId); Q_UNUSED(quantity); Q_UNUSED(note); Q_UNUSED(reason);
#endif
		return false;
	}

	bool StockController::setQuantity(int partId, int newQuantity, const QString& note) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			SQLiteWrapper::SQLite& db = m_handle->connection();
			// correct() writes nothing when the count already matches, which is the common case
			// (the field is committed on focus loss whether or not it changed) — not a failure.
			return StockRepository::correct(db, partId, newQuantity, note.toStdString())
					!= NoStockTransactionId
				|| StockRepository::currentQuantity(db, partId) == newQuantity;
		}
#else
		Q_UNUSED(partId); Q_UNUSED(newQuantity); Q_UNUSED(note);
#endif
		return false;
	}

	std::vector<StockTransaction> StockController::history(int partId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return StockRepository::history(m_handle->connection(), partId);
		}
#else
		Q_UNUSED(partId);
#endif
		return std::vector<StockTransaction>();
	}

}
