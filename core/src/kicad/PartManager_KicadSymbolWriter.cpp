#include "kicad/PartManager_KicadSymbolWriter.h"
#include "PartManager_global.h"

#include <cstdio>

namespace PartManager
{

	const char* const KicadSymbolWriter::GenericBaseSymbol = "PM_Generic";

	namespace
	{
		// KiCad 9 writes this version; 7 and 8 read it. Bumping it is a deliberate act, not
		// something to derive from whatever KiCad happens to be installed.
		const char* const LibraryVersion = "20241209";

		std::string lower(const std::string& text)
		{
			std::string out = text;
			for (char& c : out)
			{
				if (c >= 'A' && c <= 'Z')
				{
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return out;
		}

		bool contains(const std::string& haystack, const char* needle)
		{
			return haystack.find(needle) != std::string::npos;
		}

		// One hidden property line, at the origin. Position and font are fixed because these are
		// metadata fields nobody looks at on the canvas — only Reference and Value are placed.
		std::string hiddenProperty(const std::string& name, const std::string& value)
		{
			if (value.empty())
			{
				// An empty property is noise in the library and in KiCad's field editor.
				return std::string();
			}
			std::string out;
			out += "\t\t(property \"" + KicadSymbolWriter::escape(name) + "\" \""
				+ KicadSymbolWriter::escape(value) + "\"\n";
			out += "\t\t\t(at 0 0 0)\n";
			out += "\t\t\t(effects\n";
			out += "\t\t\t\t(font\n";
			out += "\t\t\t\t\t(size 1.27 1.27)\n";
			out += "\t\t\t\t)\n";
			out += "\t\t\t\t(hide yes)\n";
			out += "\t\t\t)\n";
			out += "\t\t)\n";
			return out;
		}
	}

	std::string KicadSymbolWriter::escape(const std::string& text)
	{
		std::string out;
		out.reserve(text.size() + 8);
		for (char c : text)
		{
			if (c == '"' || c == '\\')
			{
				out += '\\';
			}
			out += c;
		}
		return out;
	}

	std::string KicadSymbolWriter::sanitizeSymbolName(const std::string& name)
	{
		std::string out;
		for (char c : name)
		{
			// KiCad accepts a lot inside a quoted name, but a colon separates library from symbol
			// everywhere it is referenced, and control characters break the parser outright.
			if (c == ':' || c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20)
			{
				out += '_';
			}
			else
			{
				out += c;
			}
		}
		// A nameless symbol makes the whole library unreadable, so it never gets written.
		return out.empty() ? std::string("Unnamed") : out;
	}

	std::string KicadSymbolWriter::baseSymbolForType(const std::string& typeName)
	{
		const std::string name = lower(typeName);
		// Conservative on purpose, same as the Mouser category mapping: only the unambiguous
		// cases map, everything else gets the generic box rather than a symbol that looks
		// authoritative and has the wrong pinout.
		if (contains(name, "resistor"))    { return "PM_R"; }
		if (contains(name, "capacitor"))   { return "PM_C"; }
		if (contains(name, "inductor"))    { return "PM_L"; }
		if (contains(name, "led"))         { return "PM_LED"; }
		if (contains(name, "diode"))       { return "PM_D"; }
		// "Transistor" and "MOSFET" are both three-terminal and both get the same generic
		// three-pin base; drawing a real MOSFET body would need to know the channel type.
		if (contains(name, "transistor") || contains(name, "mosfet")) { return "PM_Q"; }
		return GenericBaseSymbol;
	}

	std::string KicadSymbolWriter::referenceForBase(const std::string& baseSymbol)
	{
		if (baseSymbol == "PM_R")   { return "R"; }
		if (baseSymbol == "PM_C")   { return "C"; }
		if (baseSymbol == "PM_L")   { return "L"; }
		if (baseSymbol == "PM_D")   { return "D"; }
		if (baseSymbol == "PM_LED") { return "D"; }
		if (baseSymbol == "PM_Q")   { return "Q"; }
		return "U";
	}

	namespace
	{
		// A base symbol: the visible Reference/Value properties plus a body. Kept minimal and
		// generic — these exist to be extended, and every part-specific value is overridden by
		// the deriving symbol.
		std::string baseSymbol(const std::string& name, const std::string& reference,
			const std::string& body, const std::string& footprintFilter)
		{
			std::string out;
			out += "\t(symbol \"" + name + "\"\n";
			out += "\t\t(pin_numbers\n\t\t\t(hide yes)\n\t\t)\n";
			out += "\t\t(pin_names\n\t\t\t(offset 0)\n\t\t)\n";
			out += "\t\t(exclude_from_sim no)\n";
			out += "\t\t(in_bom yes)\n";
			out += "\t\t(on_board yes)\n";
			out += "\t\t(property \"Reference\" \"" + reference + "\"\n";
			out += "\t\t\t(at 2.54 1.27 0)\n";
			out += "\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n";
			out += "\t\t)\n";
			out += "\t\t(property \"Value\" \"" + name + "\"\n";
			out += "\t\t\t(at 2.54 -1.27 0)\n";
			out += "\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n";
			out += "\t\t)\n";
			out += hiddenProperty("Footprint", "-");
			out += hiddenProperty("Datasheet", "~");
			if (!footprintFilter.empty())
			{
				out += hiddenProperty("ki_fp_filters", footprintFilter);
			}
			out += body;
			out += "\t)\n";
			return out;
		}

		// A two-pin passive body: a box with a pin at each end, on the 2.54 mm grid.
		std::string twoPinBody(const std::string& name)
		{
			std::string out;
			out += "\t\t(symbol \"" + name + "_0_1\"\n";
			out += "\t\t\t(rectangle\n";
			out += "\t\t\t\t(start -1.016 -2.54)\n";
			out += "\t\t\t\t(end 1.016 2.54)\n";
			out += "\t\t\t\t(stroke\n\t\t\t\t\t(width 0.254)\n\t\t\t\t\t(type default)\n\t\t\t\t)\n";
			out += "\t\t\t\t(fill\n\t\t\t\t\t(type none)\n\t\t\t\t)\n";
			out += "\t\t\t)\n";
			out += "\t\t)\n";
			out += "\t\t(symbol \"" + name + "_1_1\"\n";
			out += "\t\t\t(pin passive line\n";
			out += "\t\t\t\t(at 0 3.81 270)\n";
			out += "\t\t\t\t(length 1.27)\n";
			out += "\t\t\t\t(name \"~\"\n\t\t\t\t\t(effects\n\t\t\t\t\t\t(font\n\t\t\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t\t\t)\n\t\t\t\t\t)\n\t\t\t\t)\n";
			out += "\t\t\t\t(number \"1\"\n\t\t\t\t\t(effects\n\t\t\t\t\t\t(font\n\t\t\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t\t\t)\n\t\t\t\t\t)\n\t\t\t\t)\n";
			out += "\t\t\t)\n";
			out += "\t\t\t(pin passive line\n";
			out += "\t\t\t\t(at 0 -3.81 90)\n";
			out += "\t\t\t\t(length 1.27)\n";
			out += "\t\t\t\t(name \"~\"\n\t\t\t\t\t(effects\n\t\t\t\t\t\t(font\n\t\t\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t\t\t)\n\t\t\t\t\t)\n\t\t\t\t)\n";
			out += "\t\t\t\t(number \"2\"\n\t\t\t\t\t(effects\n\t\t\t\t\t\t(font\n\t\t\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t\t\t)\n\t\t\t\t\t)\n\t\t\t\t)\n";
			out += "\t\t\t)\n";
			out += "\t\t)\n";
			return out;
		}

		// A three-pin body for transistor-like parts, and the generic box.
		std::string boxBody(const std::string& name, int pinCount)
		{
			std::string out;
			out += "\t\t(symbol \"" + name + "_0_1\"\n";
			out += "\t\t\t(rectangle\n";
			out += "\t\t\t\t(start -5.08 " + std::string(pinCount > 2 ? "5.08" : "2.54") + ")\n";
			out += "\t\t\t\t(end 5.08 " + std::string(pinCount > 2 ? "-5.08" : "-2.54") + ")\n";
			out += "\t\t\t\t(stroke\n\t\t\t\t\t(width 0.254)\n\t\t\t\t\t(type default)\n\t\t\t\t)\n";
			out += "\t\t\t\t(fill\n\t\t\t\t\t(type background)\n\t\t\t\t)\n";
			out += "\t\t\t)\n";
			out += "\t\t)\n";
			out += "\t\t(symbol \"" + name + "_1_1\"\n";
			for (int pin = 1; pin <= pinCount; ++pin)
			{
				// Left column, top down, 2.54 mm apart — a placeholder pinout the user relabels
				// in KiCad, which the edit tracker then stops overwriting.
				const double y = 2.54 * ((pinCount - 1) / 2.0 - (pin - 1));
				char position[32] = { 0 };
				std::snprintf(position, sizeof(position), "%.2f", y);
				out += "\t\t\t(pin passive line\n";
				out += "\t\t\t\t(at -7.62 " + std::string(position) + " 0)\n";
				out += "\t\t\t\t(length 2.54)\n";
				out += "\t\t\t\t(name \"~\"\n\t\t\t\t\t(effects\n\t\t\t\t\t\t(font\n\t\t\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t\t\t)\n\t\t\t\t\t)\n\t\t\t\t)\n";
				out += "\t\t\t\t(number \"" + std::to_string(pin)
					+ "\"\n\t\t\t\t\t(effects\n\t\t\t\t\t\t(font\n\t\t\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t\t\t)\n\t\t\t\t\t)\n\t\t\t\t)\n";
				out += "\t\t\t)\n";
			}
			out += "\t\t)\n";
			return out;
		}
	}

	std::vector<std::string> KicadSymbolWriter::baseSymbolBlocks()
	{
		// Every base a generated symbol can extend has to be in the same file, or KiCad reports
		// the library as broken. This list and baseSymbolForType() move together.
		std::vector<std::string> blocks;
		blocks.push_back(baseSymbol("PM_R", "R", twoPinBody("PM_R"), "R_*"));
		blocks.push_back(baseSymbol("PM_C", "C", twoPinBody("PM_C"), "C_*"));
		blocks.push_back(baseSymbol("PM_L", "L", twoPinBody("PM_L"), "L_*"));
		blocks.push_back(baseSymbol("PM_D", "D", twoPinBody("PM_D"), "D_*"));
		blocks.push_back(baseSymbol("PM_LED", "D", twoPinBody("PM_LED"), "LED_*"));
		blocks.push_back(baseSymbol("PM_Q", "Q", boxBody("PM_Q", 3), std::string()));
		blocks.push_back(baseSymbol(GenericBaseSymbol, "U", boxBody(GenericBaseSymbol, 4),
			std::string()));
		return blocks;
	}

	std::string KicadSymbolWriter::symbolBlock(const KicadSymbolSpec& spec)
	{
		const std::string name = sanitizeSymbolName(spec.name);
		const std::string base = spec.baseSymbol.empty() ? GenericBaseSymbol : spec.baseSymbol;

		std::string out;
		out += "\t(symbol \"" + escape(name) + "\"\n";
		out += "\t\t(extends \"" + escape(base) + "\")\n";
		// Reference and Value are the two the user sees on the canvas, so they are placed rather
		// than hidden. Everything else is metadata.
		out += "\t\t(property \"Reference\" \"" + escape(spec.reference) + "\"\n";
		out += "\t\t\t(at 2.54 1.27 0)\n";
		out += "\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n";
		out += "\t\t)\n";
		out += "\t\t(property \"Value\" \"" + escape(spec.value.empty() ? name : spec.value) + "\"\n";
		out += "\t\t\t(at 2.54 -1.27 0)\n";
		out += "\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n";
		out += "\t\t)\n";
		out += hiddenProperty("Footprint", spec.footprint);
		out += hiddenProperty("Datasheet", spec.datasheet);
		out += hiddenProperty("Description", spec.description);
		out += hiddenProperty("ki_keywords", spec.keywords);
		// The round trip back into this app: a symbol on a board can be traced to the row it
		// came from, which is what §5b's verification plugin reads.
		out += hiddenProperty("PM_PartID", spec.partId > 0 ? std::to_string(spec.partId) : std::string());
		out += hiddenProperty("Mouser P/N", spec.mouserPartNumber);
		out += hiddenProperty("PM_3DModel", spec.model3DPath);
		out += "\t)\n";
		return out;
	}

	std::string KicadSymbolWriter::library(const std::vector<std::string>& symbolBlocks)
	{
		std::string out;
		out += "(kicad_symbol_lib\n";
		out += "\t(version " + std::string(LibraryVersion) + ")\n";
		out += "\t(generator \"PartManager\")\n";
		out += "\t(generator_version \"9.0\")\n";
		for (const std::string& block : baseSymbolBlocks())
		{
			out += block;
		}
		for (const std::string& block : symbolBlocks)
		{
			out += block;
		}
		out += ")\n";
		return out;
	}

	namespace
	{
		// Walks to the end of the string literal that starts at `openQuote` (the index of its
		// opening `"`). Returns the index of the closing quote, or npos when unterminated.
		size_t endOfLiteral(const std::string& text, size_t openQuote)
		{
			for (size_t i = openQuote + 1; i < text.size(); ++i)
			{
				if (text[i] == '\\')
				{
					++i;        // the escaped byte is never the terminator
					continue;
				}
				if (text[i] == '"')
				{
					return i;
				}
			}
			return std::string::npos;
		}

		// The literal's contents with escapes resolved.
		std::string literalAt(const std::string& text, size_t openQuote, size_t closeQuote)
		{
			std::string value;
			for (size_t i = openQuote + 1; i < closeQuote; ++i)
			{
				if (text[i] == '\\' && i + 1 < closeQuote)
				{
					++i;
				}
				value += text[i];
			}
			return value;
		}

		// Index of the `(property "<key>"` whose key matches, or npos. Scans the whole block:
		// properties only ever sit directly under the symbol, and a nested unit body has none.
		size_t findProperty(const std::string& block, const std::string& key, size_t& outKeyOpen,
			size_t& outKeyClose)
		{
			size_t at = 0;
			while ((at = block.find("(property", at)) != std::string::npos)
			{
				size_t i = at + 9;
				while (i < block.size() && (block[i] == ' ' || block[i] == '\t' || block[i] == '\n'
					|| block[i] == '\r'))
				{
					++i;
				}
				if (i >= block.size() || block[i] != '"')
				{
					at += 9;
					continue;
				}
				const size_t close = endOfLiteral(block, i);
				if (close == std::string::npos)
				{
					return std::string::npos;
				}
				if (literalAt(block, i, close) == key)
				{
					outKeyOpen = i;
					outKeyClose = close;
					return at;
				}
				at = close;
			}
			return std::string::npos;
		}
	}

	std::string KicadSymbolWriter::symbolProperty(const std::string& symbolBlock,
		const std::string& key)
	{
		size_t keyOpen = 0;
		size_t keyClose = 0;
		if (findProperty(symbolBlock, key, keyOpen, keyClose) == std::string::npos)
		{
			return std::string();
		}
		// The value is the next literal after the key.
		const size_t valueOpen = symbolBlock.find('"', keyClose + 1);
		if (valueOpen == std::string::npos)
		{
			return std::string();
		}
		const size_t valueClose = endOfLiteral(symbolBlock, valueOpen);
		return valueClose == std::string::npos
			? std::string() : literalAt(symbolBlock, valueOpen, valueClose);
	}

	std::string KicadSymbolWriter::withProperty(const std::string& symbolBlock,
		const std::string& key, const std::string& value)
	{
		size_t keyOpen = 0;
		size_t keyClose = 0;
		if (findProperty(symbolBlock, key, keyOpen, keyClose) != std::string::npos)
		{
			const size_t valueOpen = symbolBlock.find('"', keyClose + 1);
			if (valueOpen != std::string::npos)
			{
				const size_t valueClose = endOfLiteral(symbolBlock, valueOpen);
				if (valueClose != std::string::npos)
				{
					return symbolBlock.substr(0, valueOpen + 1) + escape(value)
						+ symbolBlock.substr(valueClose);
				}
			}
			return symbolBlock;   // malformed; better untouched than truncated
		}

		if (value.empty())
		{
			return symbolBlock;   // nothing to add, and an empty property is noise
		}
		// Appended just before the block's own closing paren, so it lands inside the symbol.
		const size_t lastParen = symbolBlock.rfind(')');
		if (lastParen == std::string::npos)
		{
			return symbolBlock;
		}
		size_t insertAt = lastParen;
		while (insertAt > 0 && (symbolBlock[insertAt - 1] == '\t' || symbolBlock[insertAt - 1] == ' '))
		{
			--insertAt;
		}
		return symbolBlock.substr(0, insertAt) + hiddenProperty(key, value)
			+ symbolBlock.substr(insertAt);
	}

	std::string KicadSymbolWriter::renamedSymbol(const std::string& symbolBlock,
		const std::string& newName)
	{
		const std::string oldName = symbolNameOf(symbolBlock);
		const std::string sanitized = sanitizeSymbolName(newName);
		if (oldName.empty() || oldName == sanitized)
		{
			return symbolBlock;
		}

		std::string out;
		out.reserve(symbolBlock.size() + 32);
		size_t i = 0;
		bool renamedParent = false;
		while (i < symbolBlock.size())
		{
			const size_t at = symbolBlock.find("(symbol \"", i);
			if (at == std::string::npos)
			{
				out += symbolBlock.substr(i);
				break;
			}
			const size_t quote = at + 8;                    // index of the opening `"`
			const size_t close = endOfLiteral(symbolBlock, quote);
			if (close == std::string::npos)
			{
				out += symbolBlock.substr(i);
				break;
			}
			const std::string name = literalAt(symbolBlock, quote, close);

			std::string replacement = name;
			if (!renamedParent)
			{
				replacement = sanitized;
				renamedParent = true;
			}
			else if (name.rfind(oldName + "_", 0) == 0)
			{
				// A unit body: "<Parent>_1_1". Only the parent prefix moves.
				replacement = sanitized + name.substr(oldName.size());
			}

			out += symbolBlock.substr(i, quote + 1 - i);
			out += escape(replacement);
			i = close;                                       // the closing quote is copied next
		}
		return out;
	}

	std::string KicadSymbolWriter::symbolNameOf(const std::string& symbolBlock)
	{
		const size_t open = symbolBlock.find("(symbol \"");
		if (open == std::string::npos)
		{
			return std::string();
		}
		size_t i = open + 9;
		std::string name;
		while (i < symbolBlock.size() && symbolBlock[i] != '"')
		{
			// A quote can be escaped inside the name; skip the backslash and take the next byte
			// literally, or the name is truncated at the first escaped quote.
			if (symbolBlock[i] == '\\' && i + 1 < symbolBlock.size())
			{
				++i;
			}
			name += symbolBlock[i];
			++i;
		}
		return i < symbolBlock.size() ? name : std::string();
	}

	std::vector<std::string> KicadSymbolWriter::splitSymbols(const std::string& libraryText)
	{
		std::vector<std::string> blocks;

		// A depth counter that understands string literals and their escapes. A regex or a naive
		// paren count would break on the first footprint filter containing a bracket, and on
		// every `(name "(")`. Blocks come out byte-for-byte so a hand edit survives the round
		// trip untouched — that is the whole point of preserving them (§5a).
		size_t i = 0;
		while (i < libraryText.size())
		{
			// Find a top-level `(symbol "` — top-level meaning depth 1 inside kicad_symbol_lib.
			if (libraryText.compare(i, 9, "(symbol \"") != 0)
			{
				++i;
				continue;
			}

			// Back up over the line's indentation so the block comes out byte-identical to what
			// symbolBlock() produces — otherwise every symbol read back differs from its own
			// baseline by one leading tab and the whole library reads as hand-edited.
			size_t start = i;
			while (start > 0 && (libraryText[start - 1] == '\t' || libraryText[start - 1] == ' '))
			{
				--start;
			}
			int depth = 0;
			bool inString = false;
			size_t j = i;
			for (; j < libraryText.size(); ++j)
			{
				const char c = libraryText[j];
				if (inString)
				{
					if (c == '\\')
					{
						++j;   // the escaped byte is never a delimiter
					}
					else if (c == '"')
					{
						inString = false;
					}
					continue;
				}
				if (c == '"')      { inString = true; }
				else if (c == '(') { ++depth; }
				else if (c == ')')
				{
					--depth;
					if (depth == 0)
					{
						break;
					}
				}
			}
			if (depth != 0 || j >= libraryText.size())
			{
				// Unbalanced: a truncated file. Stop rather than emit a half block that would be
				// written back out as if it were valid.
				break;
			}

			// Nested `(symbol "X_0_1" ...)` bodies live inside the block just taken, so resuming
			// after it is what keeps them from being reported as separate symbols.
			blocks.push_back(libraryText.substr(start, j - start + 1) + "\n");
			i = j + 1;
		}
		return blocks;
	}

}
