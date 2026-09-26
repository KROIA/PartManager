#pragma once

#include "UnitTest.h"
#include "database/PartManager_Transaction.h"
#include <filesystem>
#include <memory>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

class TST_Transaction : public UnitTest::Test
{
	TEST_CLASS(TST_Transaction)
public:
	TST_Transaction()
		: Test("TST_Transaction")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_Transaction::commitPersists);
		ADD_TEST(TST_Transaction::scopeExitWithoutCommitDiscards);
		ADD_TEST(TST_Transaction::aFailureMidWayDiscardsEverythingBeforeIt);
		ADD_TEST(TST_Transaction::explicitRollbackDiscards);
		ADD_TEST(TST_Transaction::anAlreadyOpenTransactionIsLeftAlone);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// A fresh temp database holding one trivial table to write into.
	static std::unique_ptr<SQLiteWrapper::SQLite> freshDb(const std::string& name)
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		auto db = std::make_unique<SQLiteWrapper::SQLite>(path.string());
		db->open();
		db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);");
		return db;
	}

	static int rowCount(SQLiteWrapper::SQLite& db)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll("SELECT COUNT(*) FROM t;");
		if (rows.empty() || rows.front().empty())
		{
			return -1;
		}
		return std::atoi(rows.front().front().c_str());
	}

	// Tests
	TEST_FUNCTION(commitPersists)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_Transaction_commit.db");
		{
			PartManager::Transaction transaction(*db);
			TEST_ASSERT_M(transaction.active(), "BEGIN IMMEDIATE must succeed on an idle connection");
			TEST_ASSERT(db->execute("INSERT INTO t (v) VALUES ('a');"));
			TEST_ASSERT(db->execute("INSERT INTO t (v) VALUES ('b');"));
			TEST_ASSERT_M(transaction.commit(), "commit() must succeed");
			TEST_ASSERT_M(!transaction.active(), "a committed guard owns nothing any more");
		}
		TEST_COMPARE(rowCount(*db), 2);
	}

	TEST_FUNCTION(scopeExitWithoutCommitDiscards)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_Transaction_scope.db");
		{
			PartManager::Transaction transaction(*db);
			TEST_ASSERT(transaction.active());
			TEST_ASSERT(db->execute("INSERT INTO t (v) VALUES ('a');"));
		}
		TEST_COMPARE(rowCount(*db), 0);
	}

	// The case the category merge actually needs: something fails halfway and every write that
	// came before it has to go with it (see PartManager_Transaction.h).
	TEST_FUNCTION(aFailureMidWayDiscardsEverythingBeforeIt)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_Transaction_failure.db");
		bool reportedFailure = false;
		{
			PartManager::Transaction transaction(*db);
			TEST_ASSERT(transaction.active());
			TEST_ASSERT(db->execute("INSERT INTO t (v) VALUES ('first');"));
			if (!db->execute("INSERT INTO no_such_table (v) VALUES ('second');"))
			{
				reportedFailure = true;   // early return without commit(); the guard rolls back
			}
			TEST_ASSERT_M(reportedFailure, "a write against a missing table must report failure");
		}
		TEST_COMPARE(rowCount(*db), 0);
	}

	TEST_FUNCTION(explicitRollbackDiscards)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_Transaction_rollback.db");
		{
			PartManager::Transaction transaction(*db);
			TEST_ASSERT(db->execute("INSERT INTO t (v) VALUES ('a');"));
			TEST_ASSERT_M(transaction.rollback(), "explicit rollback must succeed");
			TEST_ASSERT_M(!transaction.active(), "a rolled-back guard owns nothing any more");
			// The destructor must not roll back a second time, against whatever is open next.
			TEST_ASSERT(db->execute("INSERT INTO t (v) VALUES ('b');"));
		}
		TEST_COMPARE(rowCount(*db), 1);
	}

	// SQLite allows no nested transactions, so BEGIN fails when one is already open. The guard
	// must report that rather than adopting — and above all must not ROLLBACK somebody else's
	// transaction on the way out of scope.
	TEST_FUNCTION(anAlreadyOpenTransactionIsLeftAlone)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_Transaction_nested.db");
		TEST_ASSERT(db->execute("BEGIN IMMEDIATE;"));
		TEST_ASSERT(db->execute("INSERT INTO t (v) VALUES ('outer');"));
		{
			PartManager::Transaction transaction(*db);
			TEST_ASSERT_M(!transaction.active(), "a nested BEGIN must be reported as inactive");
			TEST_ASSERT_M(!transaction.commit(), "an inactive guard commits nothing");
		}
		// Still inside the outer transaction: committing it by hand must keep the row.
		TEST_ASSERT(db->execute("COMMIT;"));
		TEST_COMPARE(rowCount(*db), 1);
	}
#endif

};

TEST_INSTANTIATE(TST_Transaction);
