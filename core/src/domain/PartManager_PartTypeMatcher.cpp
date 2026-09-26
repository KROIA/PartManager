#include "domain/PartManager_PartTypeMatcher.h"

#include <algorithm>
#include <cctype>

namespace PartManager
{
	namespace
	{
		// How well a candidate matched, lower is better. The order is the documented strategy
		// order: what the vendor itself suggested, then its category, then the free-text
		// description — and everything a synonym found ranks below everything a real name found.
		enum MatchTier
		{
			TierSuggestedExact = 0,
			TierCategoryExact,
			TierCategoryWord,
			TierDescriptionWord,
			TierSynonymSuggested,
			TierSynonymCategory,
			TierSynonymDescription,
			TierNone
		};

		// A type name can only be matched by whole words below this length when it is matched
		// *exactly*. "IC" inside "Magnetics" would otherwise be a match, and a two-letter
		// coincidence is precisely the wrong-template case this file exists to avoid.
		const size_t MinWordMatchLength = 3;

		// The obvious electronics cases, keyed by type name rather than by template id, so a type
		// the user created themselves and called "LED" gets them too. Kept deliberately short:
		// every entry here is a way to attach a template without the category saying its name, so
		// a loose one costs a silently wrong part. Plurals are not listed — folding handles them.
		struct Synonym { const char* typeName; const char* phrase; };
		const Synonym Synonyms[] = {
			{ "led",               "light emitting diode" },
			{ "mosfet",            "fet" },
			{ "mosfet",            "field effect transistor" },
			{ "transistor",        "bjt" },
			{ "transistor",        "bipolar transistor" },
			{ "capacitor",         "mlcc" },
			{ "ceramic capacitor", "mlcc" },
			{ "power regulator",   "voltage regulator" },
			{ "power regulator",   "regulator" },
			{ "power regulator",   "ldo" },
			{ "microcontroller",   "mcu" },
			{ "op-amp",            "operational amplifier" },
			{ "op-amp",            "opamp" },
			{ "crystal / oscillator", "crystal" },
			{ "crystal / oscillator", "oscillator" },
			{ "inductor",          "choke" },
		};

		// Crude but symmetric singular folding: applied to both sides, so it only has to be
		// consistent, not linguistically right. "mosfets" -> "mosfet", "leds" -> "led";
		// "class" keeps its double s and "ics" is too short to touch.
		std::string folded(const std::string& word)
		{
			if (word.size() > 3 && word.back() == 's' && word[word.size() - 2] != 's')
			{
				return word.substr(0, word.size() - 1);
			}
			return word;
		}

		// Lowercase words, every non-alphanumeric character a separator. That is what makes
		// "Crystal/Oscillator", "Crystal - Oscillator" and "crystal oscillator" the same thing,
		// and it is the same spirit as the header matching in PartListMigration.
		std::vector<std::string> wordsOf(const std::string& text)
		{
			std::vector<std::string> words;
			std::string current;
			for (const char c : text)
			{
				const unsigned char uc = static_cast<unsigned char>(c);
				if (std::isalnum(uc))
				{
					current += static_cast<char>(std::tolower(uc));
				}
				else if (!current.empty())
				{
					words.push_back(folded(current));
					current.clear();
				}
			}
			if (!current.empty())
			{
				words.push_back(folded(current));
			}
			return words;
		}

		size_t lettersIn(const std::vector<std::string>& words)
		{
			size_t count = 0;
			for (const std::string& word : words)
			{
				count += word.size();
			}
			return count;
		}

		bool sameWords(const std::vector<std::string>& left, const std::vector<std::string>& right)
		{
			return !left.empty() && left == right;
		}

		// `needle` as a contiguous run of whole words inside `hay`. Whole words rather than a
		// substring, so "Diode" is not found inside "Diodes & Rectifiers - Zener" by accident of
		// spelling but by actually being a word there.
		bool containsWords(const std::vector<std::string>& hay,
			const std::vector<std::string>& needle)
		{
			if (needle.empty() || needle.size() > hay.size())
			{
				return false;
			}
			for (size_t start = 0; start + needle.size() <= hay.size(); ++start)
			{
				if (std::equal(needle.begin(), needle.end(), hay.begin() + start))
				{
					return true;
				}
			}
			return false;
		}

		// Whether `typeId` inherits, directly or through any number of steps, from `ancestorId`.
		// The step cap is the same defence PartTypeRepository's §2b walk uses: a parent cycle in
		// the database must not hang a form being filled in.
		bool inheritsFrom(const std::vector<PartType>& types, int typeId, int ancestorId)
		{
			if (typeId == NoParentType || ancestorId == NoParentType || typeId == ancestorId)
			{
				return false;
			}
			int current = typeId;
			for (size_t step = 0; step < types.size(); ++step)
			{
				const PartType* type = nullptr;
				for (const PartType& candidate : types)
				{
					if (candidate.id == current) { type = &candidate; break; }
				}
				if (type == nullptr || type->parentTypeId == NoParentType)
				{
					return false;
				}
				if (type->parentTypeId == ancestorId)
				{
					return true;
				}
				current = type->parentTypeId;
			}
			return false;
		}
	}

	TypeMatch matchPartType(const std::vector<PartType>& types, const std::string& mouserCategory,
		const std::string& description, const std::string& suggestedTypeName)
	{
		const std::vector<std::string> suggested = wordsOf(suggestedTypeName);
		const std::vector<std::string> category = wordsOf(mouserCategory);
		const std::vector<std::string> descriptionWords = wordsOf(description);

		TypeMatch best;
		int bestTier = TierNone;
		size_t bestLength = 0;

		// One candidate offered to the running best. Ties are broken the way the header promises:
		// a child template beats the ancestor it inherits from, and otherwise the longer — more
		// specific — matched text wins. `>=` on the tier is deliberate: the first type to reach a
		// tier keeps it unless a later one is genuinely better, so the list order never decides.
		const auto offer = [&](int tier, size_t length, const PartType& type, const char* reason)
		{
			if (tier > bestTier)
			{
				return;
			}
			if (tier == bestTier && best.typeId != 0)
			{
				const bool childWins = inheritsFrom(types, type.id, best.typeId);
				const bool ancestorLoses = inheritsFrom(types, best.typeId, type.id);
				if (!childWins && (ancestorLoses || length <= bestLength))
				{
					return;
				}
			}
			bestTier = tier;
			bestLength = length;
			best.typeId = type.id;
			best.confident = true;
			best.matchedOn = reason;
		};

		for (const PartType& type : types)
		{
			const std::vector<std::string> name = wordsOf(type.name);
			if (name.empty() || type.id == NoParentType)
			{
				continue;
			}
			const size_t nameLength = lettersIn(name);

			if (sameWords(name, suggested))
			{
				offer(TierSuggestedExact, nameLength, type, "suggested-type-name");
			}
			if (sameWords(name, category))
			{
				offer(TierCategoryExact, nameLength, type, "category-exact");
			}
			if (nameLength >= MinWordMatchLength)
			{
				if (containsWords(category, name))
				{
					offer(TierCategoryWord, nameLength, type, "category-word");
				}
				if (containsWords(descriptionWords, name))
				{
					offer(TierDescriptionWord, nameLength, type, "description-word");
				}
			}

			// A synonym is the type's name said another way, so it is scored by the length of the
			// *phrase that was found* — "light emitting diode" is a more specific thing to find in
			// a description than "fet" is, whatever the two type names happen to be called.
			for (const Synonym& synonym : Synonyms)
			{
				if (!sameWords(name, wordsOf(synonym.typeName)))
				{
					continue;
				}
				const std::vector<std::string> phrase = wordsOf(synonym.phrase);
				const size_t phraseLength = lettersIn(phrase);
				if (sameWords(phrase, suggested))
				{
					offer(TierSynonymSuggested, phraseLength, type, "synonym-suggested");
				}
				if (containsWords(category, phrase))
				{
					offer(TierSynonymCategory, phraseLength, type, "synonym-category");
				}
				if (containsWords(descriptionWords, phrase))
				{
					offer(TierSynonymDescription, phraseLength, type, "synonym-description");
				}
			}
		}

		return best;
	}

}
