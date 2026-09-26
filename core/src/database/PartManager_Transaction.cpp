#include "database/PartManager_Transaction.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	Transaction::Transaction(SQLiteWrapper::SQLite& db)
		: m_db(db)
	{
		// A BEGIN against a connection that already has a transaction open fails with
		// "cannot start a transaction within a transaction". That is not an error worth
		// aborting on here — it simply means somebody outside owns the transaction — so it
		// is recorded as "not active" and this guard stays out of the way entirely.
		m_active = m_db.execute("BEGIN IMMEDIATE;");
	}

	Transaction::~Transaction()
	{
		// Runs on the error path by definition, so nothing may escape it (see header).
		try
		{
			rollback();
		}
		catch (...)
		{
		}
	}

	bool Transaction::active() const
	{
		return m_active;
	}

	bool Transaction::commit()
	{
		if (!m_active)
		{
			return false;
		}
		if (!m_db.execute("COMMIT;"))
		{
			// The transaction is still open — leave the guard active so scope exit rolls back
			// rather than silently leaving a half-written merge behind an uncommitted lock.
			return false;
		}
		m_active = false;
		return true;
	}

	bool Transaction::rollback()
	{
		if (!m_active)
		{
			return false;
		}
		m_active = false;
		return m_db.execute("ROLLBACK;");
	}

#endif

}
