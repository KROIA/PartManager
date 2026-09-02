// @file PartManager_SqlLiteral.h
// @brief One quoted SQL string literal, because `fetchAll()` takes no bind parameters.
//
// SQLiteWrapper's parameter binding is write-only (`executeWithParams()`); every
// SELECT goes through `fetchAll(const std::string&)`. A query that has to match
// on a user-supplied string therefore has to inline it, and inlining it without
// doubling the quotes is how a part named `O'Brien` turns into a syntax error at
// best and an injection at worst.
//
// Every read path that matches on text uses this. It is header-only and takes no
// database, so it is testable on its own.
// @see PartManager_SellerRepository.h, PartManager_OrderRepository.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Wraps `text` in single quotes with any embedded quote doubled — SQL's own escape.
	// Returns the literal including its quotes, so it drops straight into a query string.
	inline std::string sqlLiteral(const std::string& text)
	{
		std::string quoted;
		quoted.reserve(text.size() + 2);
		quoted += '\'';
		for (char c : text)
		{
			quoted += c;
			if (c == '\'')
			{
				quoted += c;
			}
		}
		quoted += '\'';
		return quoted;
	}

}
