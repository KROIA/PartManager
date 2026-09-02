#include "search/PartManager_SearchQuery.h"
#include "units/PartManager_ValueParser.h"

namespace PartManager
{

	namespace
	{
		const char* const TagPrefix = "tag:";

		char lowerAscii(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}

		std::string toLower(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			for (char c : text)
			{
				out.push_back(lowerAscii(c));
			}
			return out;
		}

		bool isSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}

		// Splits on whitespace, except inside double quotes. The quotes themselves are dropped, so
		// `tag:"Do not use"` arrives as the single token `tag:Do not use`.
		// ponytail: an unterminated quote runs to the end of the input instead of being an error —
		// the search box is filtered on every keystroke, and `tag:"Do n` is a half-typed query, not
		// a mistake worth showing a message for.
		std::vector<std::string> tokenize(const std::string& text)
		{
			std::vector<std::string> tokens;
			std::string current;
			bool inQuotes = false;
			bool started = false;
			for (char c : text)
			{
				if (c == '"')
				{
					inQuotes = !inQuotes;
					started = true;
					continue;
				}
				if (!inQuotes && isSpace(c))
				{
					if (started)
					{
						tokens.push_back(current);
					}
					current.clear();
					started = false;
					continue;
				}
				current.push_back(c);
				started = true;
			}
			if (started)
			{
				tokens.push_back(current);
			}
			return tokens;
		}

		bool startsWithTagPrefix(const std::string& token)
		{
			const std::string prefix(TagPrefix);
			return token.size() >= prefix.size() && toLower(token.substr(0, prefix.size())) == prefix;
		}

		// First comparison operator in the token, or npos. Two-character operators win over the
		// single-character one that starts them, so "<=" never reads as "<" followed by "=".
		size_t findOperator(const std::string& token, SearchCompareOp& outOp, size_t& outLength)
		{
			for (size_t i = 0; i < token.size(); ++i)
			{
				const char c = token[i];
				if (c != '<' && c != '>' && c != '=' && c != '!')
				{
					continue;
				}
				const bool twoChar = (i + 1 < token.size()) && token[i + 1] == '=';
				if (c == '!' && !twoChar)
				{
					continue;   // a lone '!' is just text, not an operator
				}
				outLength = twoChar ? 2 : 1;
				if (c == '<')      outOp = twoChar ? SearchCompareOp::LessEqual : SearchCompareOp::Less;
				else if (c == '>') outOp = twoChar ? SearchCompareOp::GreaterEqual : SearchCompareOp::Greater;
				else if (c == '!') outOp = SearchCompareOp::NotEqual;
				else               outOp = SearchCompareOp::Equal;   // '=' and the '==' spelling alike
				return i;
			}
			return std::string::npos;
		}

		// Attribute keys become part of a column name (`attr_<key>`), which SQL cannot parameterise.
		// Restricting them to identifier characters is what keeps that concatenation safe.
		bool isValidAttributeKey(const std::string& key)
		{
			if (key.empty() || (key[0] >= '0' && key[0] <= '9'))
			{
				return false;
			}
			for (char c : key)
			{
				const bool identifierChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_';
				if (!identifierChar)
				{
					return false;
				}
			}
			return true;
		}

		SearchQuery failure(const std::string& message)
		{
			SearchQuery query;
			query.ok = false;
			query.error = message;
			return query;
		}
	}

	bool SearchQuery::isEmpty() const
	{
		return textTerms.empty() && attributeTerms.empty() && tagGroups.empty();
	}

	std::string SearchQuery::withTagGroups(const std::string& text,
		const std::vector<std::vector<std::string>>& groups)
	{
		std::string result;
		for (const std::string& token : tokenize(text))
		{
			if (startsWithTagPrefix(token))
			{
				continue;   // the picker owns these; whatever was there is being replaced
			}
			if (!result.empty())
			{
				result += " ";
			}
			// Re-quote what the tokenizer unquoted, or the term comes back as several.
			result += (token.find(' ') == std::string::npos) ? token : ("\"" + token + "\"");
		}

		for (const std::vector<std::string>& group : groups)
		{
			std::string term;
			for (const std::string& name : group)
			{
				if (name.empty() || name.find(',') != std::string::npos)
				{
					continue;
				}
				if (!term.empty())
				{
					term += ",";
				}
				term += (name.find(' ') == std::string::npos) ? name : ("\"" + name + "\"");
			}
			if (term.empty())
			{
				continue;
			}
			if (!result.empty())
			{
				result += " ";
			}
			result += TagPrefix + term;
		}
		return result;
	}

	SearchQuery SearchQuery::parse(const std::string& text)
	{
		SearchQuery query;
		for (const std::string& token : tokenize(text))
		{
			if (startsWithTagPrefix(token))
			{
				const std::string names = token.substr(std::string(TagPrefix).size());
				if (names.empty())
				{
					return failure("empty tag name in '" + token + "'");
				}
				// Commas OR the names together. A trailing one is a half-typed `tag:I2C,` rather
				// than a mistake — the box filters on every keystroke — so it is simply dropped,
				// but a term made of nothing but separators has no tag in it at all.
				std::vector<std::string> group;
				std::string current;
				for (char c : names + ",")
				{
					if (c != ',')
					{
						current.push_back(c);
						continue;
					}
					if (!current.empty())
					{
						group.push_back(toLower(current));
					}
					current.clear();
				}
				if (group.empty())
				{
					return failure("empty tag name in '" + token + "'");
				}
				query.tagGroups.push_back(group);
				continue;
			}

			SearchCompareOp op = SearchCompareOp::Equal;
			size_t opLength = 0;
			const size_t opPos = findOperator(token, op, opLength);
			if (opPos == std::string::npos)
			{
				query.textTerms.push_back(toLower(token));
				continue;
			}

			SearchAttributeTerm term;
			term.op = op;
			term.key = toLower(token.substr(0, opPos));
			if (!isValidAttributeKey(term.key))
			{
				return failure("'" + token.substr(0, opPos) + "' is not a valid attribute name");
			}
			const std::string valueText = token.substr(opPos + opLength);
			if (valueText.empty())
			{
				return failure("missing value after '" + term.key + "'");
			}
			// One parser for entry and search alike (§2a) — SI prefixes, "4k7", "," as decimal point.
			const ValueParseResult parsed = ValueParser::parse(valueText);
			if (!parsed.ok)
			{
				return failure("cannot read the value '" + valueText + "'");
			}
			term.value = parsed.value;
			term.unit = parsed.hasExplicitUnit ? parsed.unit : std::string();
			query.attributeTerms.push_back(term);
		}
		return query;
	}

}
