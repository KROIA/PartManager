#include "pdf/PartManager_PdfText.h"

#ifdef QT_ENABLED
	#include <QByteArray>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace PartManager
{
	namespace
	{
		// ---- limits ------------------------------------------------------------------------
		// Every one of these exists because a malformed PDF must come back as an answer and not
		// as a hang. A datasheet is a few hundred pages at the very worst.

		// How far into the file `%PDF-` may sit. Real files start with it; a few carry a junk
		// preamble from a web server, and Acrobat itself accepts a leading offset.
		constexpr size_t PdfHeaderSearchWindow = 1024;
		// Recursion caps for the two structures that can be made cyclic by a corrupt file.
		constexpr int MaxPageTreeDepth = 32;
		constexpr int MaxFormXObjectDepth = 8;
		// A `TJ` kerning number is in thousandths of the text space unit, negative for a gap to
		// the right. Intra-word kerning is single or low double digits; a word space is 200-350.
		// 120 sits in the empty middle, so it separates words without splitting them.
		constexpr double SpaceKerningThousandths = 120.0;
		// Two text positions this far apart vertically are two lines. In unscaled text space, so
		// it is a fraction of a point — anything above it is a real line move and not a
		// sub/superscript shift.
		constexpr double LineBreakEpsilon = 0.55;
		// A file carrying image data and fewer non-blank characters per page than this is a
		// picture of text with a few stray labels on it, not a datasheet that was read.
		//
		// **Measured over the user's 27 real datasheets (2026-09-26.)** The genuinely readable
		// ones run from 692 to 2878 characters a page. Five are single-page scans with exactly
		// zero. One — a six-page part drawing with three images, one `DCTDecode` and *no
		// embedded fonts at all* — yields 186 characters in total, 31 a page, and they are the
		// `Ω @ V` out of a table header. At 8 it was reported as readable text, which is the
		// worst possible answer: a model handed 31 characters a page and no match concludes the
		// datasheet does not say, and fills the rest in from memory. 120 sits in the empty gap
		// between 31 and 692 with room on both sides.
		constexpr int ScannedTextCharsPerPage = 120;

		// ---- byte classification ------------------------------------------------------------

		bool isPdfBlank(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
		}

		bool isPdfDelimiter(char c)
		{
			return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']'
				|| c == '{' || c == '}' || c == '/' || c == '%';
		}

		bool isPdfRegular(char c)
		{
			return !isPdfBlank(c) && !isPdfDelimiter(c);
		}

		bool isDigit(char c)
		{
			return c >= '0' && c <= '9';
		}

		int hexValue(char c)
		{
			if (c >= '0' && c <= '9') { return c - '0'; }
			if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
			if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
			return -1;
		}

		// Past whitespace *and* `%` comments, which are legal between any two tokens.
		size_t skipBlanks(const std::string& text, size_t at)
		{
			while (at < text.size())
			{
				if (isPdfBlank(text[at]))
				{
					++at;
					continue;
				}
				if (text[at] == '%')
				{
					while (at < text.size() && text[at] != '\n' && text[at] != '\r')
					{
						++at;
					}
					continue;
				}
				return at;
			}
			return at;
		}

		// ---- text encoding ------------------------------------------------------------------

		void appendUtf8(std::string& out, unsigned int codePoint)
		{
			if (codePoint < 0x80u)
			{
				out += static_cast<char>(codePoint);
			}
			else if (codePoint < 0x800u)
			{
				out += static_cast<char>(0xC0u | (codePoint >> 6));
				out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
			}
			else if (codePoint < 0x10000u)
			{
				out += static_cast<char>(0xE0u | (codePoint >> 12));
				out += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
				out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
			}
			else if (codePoint <= 0x10FFFFu)
			{
				out += static_cast<char>(0xF0u | (codePoint >> 18));
				out += static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu));
				out += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
				out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
			}
		}

		// A `/ToUnicode` destination is UTF-16BE and may be several code units long — an `ffi`
		// ligature in a subset font maps one glyph to three characters, and dropping the tail
		// would silently misspell every word containing one.
		std::string utf16BeToUtf8(const std::string& bytes)
		{
			std::string out;
			size_t i = 0;
			while (i + 1 < bytes.size())
			{
				unsigned int unit = static_cast<unsigned int>(
					(static_cast<unsigned char>(bytes[i]) << 8)
					| static_cast<unsigned char>(bytes[i + 1]));
				i += 2;
				if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1 < bytes.size())
				{
					const unsigned int low = static_cast<unsigned int>(
						(static_cast<unsigned char>(bytes[i]) << 8)
						| static_cast<unsigned char>(bytes[i + 1]));
					if (low >= 0xDC00u && low <= 0xDFFFu)
					{
						i += 2;
						unit = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
					}
				}
				// U+0000 is what a subset font writes for a glyph it has no character for.
				// Emitting it would put a NUL in the middle of a std::string that a JSON
				// writer then truncates at.
				if (unit != 0)
				{
					appendUtf8(out, unit);
				}
			}
			return out;
		}

		// WinAnsiEncoding, the default a PDF producer reaches for on Windows and the fallback
		// this extractor uses when a font declares no encoding at all. 0x00-0x7F is ASCII;
		// 0x80-0x9F is the CP1252 block that is *not* Latin-1 and is where a naive reader turns
		// a curly quote into a control character; 0xA0 and 0xAD are the two the PDF spec
		// redefines as space and hyphen.
		const unsigned short* winAnsiTable()
		{
			static unsigned short table[256];
			static bool built = false;
			if (!built)
			{
				for (int i = 0; i < 256; ++i)
				{
					table[i] = static_cast<unsigned short>(i);
				}
				static const unsigned short high[32] = {
					0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
					0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
					0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
					0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178 };
				for (int i = 0; i < 32; ++i)
				{
					table[0x80 + i] = high[i];
				}
				table[0xA0] = 0x0020;
				table[0xAD] = 0x002D;
				built = true;
			}
			return table;
		}

		// MacRomanEncoding 0x80-0xFF. Rare in vendor datasheets and cheap to carry, and the
		// failure it prevents is silent: without it a Mac-produced PDF reads as Latin-1 and
		// every degree sign, micro and ohm comes out as a different letter.
		const unsigned short* macRomanTable()
		{
			static unsigned short table[256];
			static bool built = false;
			if (!built)
			{
				for (int i = 0; i < 128; ++i)
				{
					table[i] = static_cast<unsigned short>(i);
				}
				static const unsigned short high[128] = {
					0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
					0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
					0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
					0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
					0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
					0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
					0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
					0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
					0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
					0x00BB, 0x2026, 0x0020, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
					0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
					0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
					0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
					0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
					0xFFFD, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
					0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7 };
				for (int i = 0; i < 128; ++i)
				{
					table[128 + i] = high[i];
				}
				built = true;
			}
			return table;
		}

		// Adobe glyph names, for `/Encoding << /Differences [...] >>`. An older Type 1 font —
		// which is what a 1990s op-amp datasheet is set in — carries no `/ToUnicode` at all, so
		// this table is the only thing standing between it and mojibake.
		//
		// Letters and digits are generated rather than listed; what is listed is everything
		// whose name is not its own character.
		const std::map<std::string, unsigned short>& glyphNameTable()
		{
			static std::map<std::string, unsigned short> names;
			if (!names.empty())
			{
				return names;
			}
			for (char c = 'A'; c <= 'Z'; ++c)
			{
				names[std::string(1, c)] = static_cast<unsigned short>(c);
			}
			for (char c = 'a'; c <= 'z'; ++c)
			{
				names[std::string(1, c)] = static_cast<unsigned short>(c);
			}
			static const char* const digitNames[10] = { "zero", "one", "two", "three", "four",
				"five", "six", "seven", "eight", "nine" };
			for (int i = 0; i < 10; ++i)
			{
				names[digitNames[i]] = static_cast<unsigned short>('0' + i);
			}
			struct Entry { const char* name; unsigned short code; };
			static const Entry entries[] = {
				{ "space", 0x0020 }, { "exclam", 0x0021 }, { "quotedbl", 0x0022 },
				{ "numbersign", 0x0023 }, { "dollar", 0x0024 }, { "percent", 0x0025 },
				{ "ampersand", 0x0026 }, { "quotesingle", 0x0027 }, { "quoteright", 0x2019 },
				{ "parenleft", 0x0028 }, { "parenright", 0x0029 }, { "asterisk", 0x002A },
				{ "plus", 0x002B }, { "comma", 0x002C }, { "hyphen", 0x002D },
				{ "period", 0x002E }, { "slash", 0x002F }, { "colon", 0x003A },
				{ "semicolon", 0x003B }, { "less", 0x003C }, { "equal", 0x003D },
				{ "greater", 0x003E }, { "question", 0x003F }, { "at", 0x0040 },
				{ "bracketleft", 0x005B }, { "backslash", 0x005C }, { "bracketright", 0x005D },
				{ "asciicircum", 0x005E }, { "underscore", 0x005F }, { "grave", 0x0060 },
				{ "quoteleft", 0x2018 }, { "braceleft", 0x007B }, { "bar", 0x007C },
				{ "braceright", 0x007D }, { "asciitilde", 0x007E },
				{ "exclamdown", 0x00A1 }, { "cent", 0x00A2 }, { "sterling", 0x00A3 },
				{ "currency", 0x00A4 }, { "yen", 0x00A5 }, { "brokenbar", 0x00A6 },
				{ "section", 0x00A7 }, { "dieresis", 0x00A8 }, { "copyright", 0x00A9 },
				{ "ordfeminine", 0x00AA }, { "guillemotleft", 0x00AB }, { "logicalnot", 0x00AC },
				{ "registered", 0x00AE }, { "macron", 0x00AF }, { "degree", 0x00B0 },
				{ "plusminus", 0x00B1 }, { "twosuperior", 0x00B2 }, { "threesuperior", 0x00B3 },
				{ "acute", 0x00B4 }, { "mu", 0x00B5 }, { "paragraph", 0x00B6 },
				{ "periodcentered", 0x00B7 }, { "cedilla", 0x00B8 }, { "onesuperior", 0x00B9 },
				{ "ordmasculine", 0x00BA }, { "guillemotright", 0x00BB }, { "onequarter", 0x00BC },
				{ "onehalf", 0x00BD }, { "threequarters", 0x00BE }, { "questiondown", 0x00BF },
				{ "multiply", 0x00D7 }, { "divide", 0x00F7 }, { "germandbls", 0x00DF },
				{ "AE", 0x00C6 }, { "ae", 0x00E6 }, { "Oslash", 0x00D8 }, { "oslash", 0x00F8 },
				{ "OE", 0x0152 }, { "oe", 0x0153 }, { "Lslash", 0x0141 }, { "lslash", 0x0142 },
				{ "Eth", 0x00D0 }, { "eth", 0x00F0 }, { "Thorn", 0x00DE }, { "thorn", 0x00FE },
				{ "Scaron", 0x0160 }, { "scaron", 0x0161 }, { "Zcaron", 0x017D },
				{ "zcaron", 0x017E }, { "Ydieresis", 0x0178 }, { "dotlessi", 0x0131 },
				{ "florin", 0x0192 }, { "circumflex", 0x02C6 }, { "caron", 0x02C7 },
				{ "breve", 0x02D8 }, { "dotaccent", 0x02D9 }, { "ring", 0x02DA },
				{ "ogonek", 0x02DB }, { "tilde", 0x02DC }, { "hungarumlaut", 0x02DD },
				{ "endash", 0x2013 }, { "emdash", 0x2014 }, { "quotedblleft", 0x201C },
				{ "quotedblright", 0x201D }, { "quotesinglbase", 0x201A },
				{ "quotedblbase", 0x201E }, { "dagger", 0x2020 }, { "daggerdbl", 0x2021 },
				{ "bullet", 0x2022 }, { "ellipsis", 0x2026 }, { "perthousand", 0x2030 },
				{ "guilsinglleft", 0x2039 }, { "guilsinglright", 0x203A }, { "fraction", 0x2044 },
				{ "Euro", 0x20AC }, { "trademark", 0x2122 }, { "Omega", 0x03A9 },
				{ "Delta", 0x0394 }, { "pi", 0x03C0 }, { "minus", 0x2212 },
				{ "partialdiff", 0x2202 }, { "product", 0x220F }, { "summation", 0x2211 },
				{ "radical", 0x221A }, { "infinity", 0x221E }, { "integral", 0x222B },
				{ "approxequal", 0x2248 }, { "notequal", 0x2260 }, { "lessequal", 0x2264 },
				{ "greaterequal", 0x2265 }, { "lozenge", 0x25CA }, { "fi", 0xFB01 },
				{ "fl", 0xFB02 },
				{ "Agrave", 0x00C0 }, { "Aacute", 0x00C1 }, { "Acircumflex", 0x00C2 },
				{ "Atilde", 0x00C3 }, { "Adieresis", 0x00C4 }, { "Aring", 0x00C5 },
				{ "Ccedilla", 0x00C7 }, { "Egrave", 0x00C8 }, { "Eacute", 0x00C9 },
				{ "Ecircumflex", 0x00CA }, { "Edieresis", 0x00CB }, { "Igrave", 0x00CC },
				{ "Iacute", 0x00CD }, { "Icircumflex", 0x00CE }, { "Idieresis", 0x00CF },
				{ "Ntilde", 0x00D1 }, { "Ograve", 0x00D2 }, { "Oacute", 0x00D3 },
				{ "Ocircumflex", 0x00D4 }, { "Otilde", 0x00D5 }, { "Odieresis", 0x00D6 },
				{ "Ugrave", 0x00D9 }, { "Uacute", 0x00DA }, { "Ucircumflex", 0x00DB },
				{ "Udieresis", 0x00DC }, { "Yacute", 0x00DD },
				{ "agrave", 0x00E0 }, { "aacute", 0x00E1 }, { "acircumflex", 0x00E2 },
				{ "atilde", 0x00E3 }, { "adieresis", 0x00E4 }, { "aring", 0x00E5 },
				{ "ccedilla", 0x00E7 }, { "egrave", 0x00E8 }, { "eacute", 0x00E9 },
				{ "ecircumflex", 0x00EA }, { "edieresis", 0x00EB }, { "igrave", 0x00EC },
				{ "iacute", 0x00ED }, { "icircumflex", 0x00EE }, { "idieresis", 0x00EF },
				{ "ntilde", 0x00F1 }, { "ograve", 0x00F2 }, { "oacute", 0x00F3 },
				{ "ocircumflex", 0x00F4 }, { "otilde", 0x00F5 }, { "odieresis", 0x00F6 },
				{ "ugrave", 0x00F9 }, { "uacute", 0x00FA }, { "ucircumflex", 0x00FB },
				{ "udieresis", 0x00FC }, { "yacute", 0x00FD }, { "ydieresis", 0x00FF } };
			for (const Entry& entry : entries)
			{
				names[entry.name] = entry.code;
			}
			return names;
		}

		// A glyph name to its character, or 0. Handles the two generated forms a subset font
		// emits (`/uni2126`, `/u1F600`) before falling back to the table; `/g17` and `/cid42`
		// name a glyph index and carry no character at all, which is exactly why a font that
		// uses them needs its `/ToUnicode`.
		unsigned int glyphNameToChar(const std::string& name)
		{
			if (name.size() >= 7 && name.compare(0, 3, "uni") == 0)
			{
				unsigned int value = 0;
				for (size_t i = 3; i < 7; ++i)
				{
					const int digit = hexValue(name[i]);
					if (digit < 0) { return 0; }
					value = (value << 4) | static_cast<unsigned int>(digit);
				}
				return value;
			}
			if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u')
			{
				unsigned int value = 0;
				for (size_t i = 1; i < name.size(); ++i)
				{
					const int digit = hexValue(name[i]);
					if (digit < 0) { return 0; }
					value = (value << 4) | static_cast<unsigned int>(digit);
				}
				return value;
			}
			const std::map<std::string, unsigned short>& table = glyphNameTable();
			const std::map<std::string, unsigned short>::const_iterator found = table.find(name);
			return found == table.end() ? 0u : static_cast<unsigned int>(found->second);
		}

		// ---- stream filters -------------------------------------------------------------------

		// `/FlateDecode`, through the zlib Qt already carries.
		//
		// **Measured 2026-09-26, Qt 5.15.2.** `qUncompress` wants a 4-byte big-endian
		// *uncompressed size* in front of the raw zlib stream, which a PDF does not have — but
		// that header is only a starting guess: on `Z_BUF_ERROR` Qt doubles its buffer and
		// retries, so a claim of **zero** inflates a 1.84 MB stream whole in 2 ms. Nothing is
		// truncated and there is no size to get wrong. Lying the *other* way is the trap: Qt
		// allocates the claimed size up front, so a guessed 200 MB really does allocate 200 MB.
		// A truncated stream and a non-deflate one both come back empty in single-digit
		// milliseconds rather than climbing the doubling loop, which is what makes this safe to
		// point at an arbitrary file.
		std::string flateDecode(const std::string& raw)
		{
#ifdef QT_ENABLED
			if (raw.size() < 2)
			{
				return std::string();
			}
			const auto inflateFrom = [&raw](size_t start) -> std::string
			{
				QByteArray framed;
				framed.reserve(static_cast<int>(raw.size() - start) + 4);
				framed.append(4, '\0');
				framed.append(raw.data() + start, static_cast<int>(raw.size() - start));
				const QByteArray out = qUncompress(framed);
				return std::string(out.constData(), static_cast<size_t>(out.size()));
			};
			std::string out = inflateFrom(0);
			if (!out.empty())
			{
				return out;
			}
			// Some producers leave the EOL that followed `stream` inside the stream data. The
			// zlib header is two bytes with a known shape, so finding where it really starts is
			// cheap and rescues a file that would otherwise read as empty.
			for (size_t start = 1; start < raw.size() - 1 && start < 8; ++start)
			{
				const unsigned int first = static_cast<unsigned char>(raw[start]);
				const unsigned int second = static_cast<unsigned char>(raw[start + 1]);
				if ((first & 0x0Fu) != 8u || ((first << 8) | second) % 31u != 0u)
				{
					continue;
				}
				out = inflateFrom(start);
				if (!out.empty())
				{
					return out;
				}
			}
			return std::string();
#else
			// No Qt means no zlib, and there is no second copy of one in this project. Callers
			// see this as "the stream decoded to nothing", which extract() reports as a file it
			// could not read rather than as an empty datasheet.
			PM_UNUSED(raw);
			return std::string();
#endif
		}

		std::string asciiHexDecode(const std::string& raw)
		{
			std::string out;
			int high = -1;
			for (const char c : raw)
			{
				if (c == '>')
				{
					break;
				}
				const int digit = hexValue(c);
				if (digit < 0)
				{
					continue;
				}
				if (high < 0)
				{
					high = digit;
				}
				else
				{
					out += static_cast<char>((high << 4) | digit);
					high = -1;
				}
			}
			if (high >= 0)
			{
				// An odd final digit is padded with zero, per the filter's own rule.
				out += static_cast<char>(high << 4);
			}
			return out;
		}

		std::string ascii85Decode(const std::string& raw)
		{
			std::string out;
			unsigned int group = 0;
			int count = 0;
			size_t i = 0;
			if (raw.size() >= 2 && raw[0] == '<' && raw[1] == '~')
			{
				i = 2;
			}
			for (; i < raw.size(); ++i)
			{
				const char c = raw[i];
				if (c == '~')
				{
					break;
				}
				if (isPdfBlank(c))
				{
					continue;
				}
				if (c == 'z' && count == 0)
				{
					out.append(4, '\0');
					continue;
				}
				if (c < '!' || c > 'u')
				{
					continue;
				}
				group = group * 85u + static_cast<unsigned int>(c - '!');
				if (++count == 5)
				{
					for (int shift = 24; shift >= 0; shift -= 8)
					{
						out += static_cast<char>((group >> shift) & 0xFFu);
					}
					group = 0;
					count = 0;
				}
			}
			if (count > 1)
			{
				for (int pad = count; pad < 5; ++pad)
				{
					group = group * 85u + 84u;
				}
				for (int written = 0; written < count - 1; ++written)
				{
					out += static_cast<char>((group >> (24 - 8 * written)) & 0xFFu);
				}
			}
			return out;
		}

		std::string runLengthDecode(const std::string& raw)
		{
			std::string out;
			size_t i = 0;
			while (i < raw.size())
			{
				const int length = static_cast<unsigned char>(raw[i++]);
				if (length == 128)
				{
					break;
				}
				if (length < 128)
				{
					const size_t count = std::min(static_cast<size_t>(length) + 1, raw.size() - i);
					out.append(raw, i, count);
					i += count;
				}
				else if (i < raw.size())
				{
					out.append(static_cast<size_t>(257 - length), raw[i]);
					++i;
				}
			}
			return out;
		}

		// `/LZWDecode`, which is what a PDF written before Acrobat 3 uses in place of Flate. Kept
		// because "this datasheet is from 1998" is not a reason to answer from memory.
		std::string lzwDecode(const std::string& raw, int earlyChange)
		{
			std::vector<std::string> table;
			table.reserve(4096);
			const auto resetTable = [&table]()
			{
				table.clear();
				for (int i = 0; i < 256; ++i)
				{
					table.push_back(std::string(1, static_cast<char>(i)));
				}
				table.push_back(std::string());   // 256: clear
				table.push_back(std::string());   // 257: end of data
			};
			resetTable();

			std::string out;
			unsigned int buffer = 0;
			int bitsHeld = 0;
			int codeBits = 9;
			int previous = -1;
			for (size_t i = 0; i <= raw.size(); ++i)
			{
				if (i < raw.size())
				{
					buffer = (buffer << 8) | static_cast<unsigned char>(raw[i]);
					bitsHeld += 8;
				}
				while (bitsHeld >= codeBits)
				{
					const unsigned int mask = (1u << codeBits) - 1u;
					const int code =
						static_cast<int>((buffer >> (bitsHeld - codeBits)) & mask);
					bitsHeld -= codeBits;
					if (code == 256)
					{
						resetTable();
						codeBits = 9;
						previous = -1;
						continue;
					}
					if (code == 257)
					{
						return out;
					}
					std::string entry;
					if (code >= 0 && code < static_cast<int>(table.size()))
					{
						entry = table[static_cast<size_t>(code)];
					}
					else if (previous >= 0 && previous < static_cast<int>(table.size()))
					{
						const std::string& before = table[static_cast<size_t>(previous)];
						if (before.empty())
						{
							return out;
						}
						entry = before + before.substr(0, 1);
					}
					else
					{
						return out;
					}
					out += entry;
					if (previous >= 0 && previous < static_cast<int>(table.size())
						&& !entry.empty() && table.size() < 4096)
					{
						table.push_back(table[static_cast<size_t>(previous)] + entry.substr(0, 1));
					}
					previous = code;
					const size_t next = table.size() + static_cast<size_t>(earlyChange);
					if (next >= 2048) { codeBits = 12; }
					else if (next >= 1024) { codeBits = 11; }
					else if (next >= 512) { codeBits = 10; }
					else { codeBits = 9; }
				}
				if (i >= raw.size())
				{
					break;
				}
			}
			return out;
		}

		// `/DecodeParms /Predictor`. An `/ObjStm` compressed with a PNG predictor is not common
		// but it is legal, and undoing the wrong one produces a body that parses as nothing — a
		// failure that looks exactly like a file with no text in it.
		std::string applyPredictor(const std::string& data, int predictor, int colors,
			int bitsPerComponent, int columns)
		{
			if (predictor < 2 || data.empty())
			{
				return data;
			}
			const int sampleBits = std::max(1, colors) * std::max(1, bitsPerComponent);
			const size_t bytesPerPixel =
				static_cast<size_t>(std::max(1, sampleBits / 8));
			const size_t rowBytes = static_cast<size_t>(
				(static_cast<long long>(std::max(1, columns)) * sampleBits + 7) / 8);
			if (rowBytes == 0)
			{
				return data;
			}
			if (predictor == 2)
			{
				if (bitsPerComponent != 8)
				{
					return data;
				}
				std::string out = data;
				for (size_t row = 0; row + rowBytes <= out.size(); row += rowBytes)
				{
					for (size_t i = bytesPerPixel; i < rowBytes; ++i)
					{
						out[row + i] = static_cast<char>(
							static_cast<unsigned char>(out[row + i])
							+ static_cast<unsigned char>(out[row + i - bytesPerPixel]));
					}
				}
				return out;
			}

			// PNG predictors: every row is prefixed with the filter type it was written with.
			std::string out;
			std::vector<unsigned char> previous(rowBytes, 0);
			std::vector<unsigned char> current(rowBytes, 0);
			size_t at = 0;
			while (at + 1 <= data.size())
			{
				const int filter = static_cast<unsigned char>(data[at++]);
				const size_t available = std::min(rowBytes, data.size() - at);
				if (available == 0)
				{
					break;
				}
				std::fill(current.begin(), current.end(), static_cast<unsigned char>(0));
				for (size_t i = 0; i < available; ++i)
				{
					current[i] = static_cast<unsigned char>(data[at + i]);
				}
				at += available;
				for (size_t i = 0; i < rowBytes; ++i)
				{
					const int left = i >= bytesPerPixel
						? static_cast<int>(current[i - bytesPerPixel]) : 0;
					const int up = static_cast<int>(previous[i]);
					const int upLeft = i >= bytesPerPixel
						? static_cast<int>(previous[i - bytesPerPixel]) : 0;
					int value = static_cast<int>(current[i]);
					switch (filter)
					{
					case 1: value += left; break;
					case 2: value += up; break;
					case 3: value += (left + up) / 2; break;
					case 4:
					{
						const int estimate = left + up - upLeft;
						const int distLeft = std::abs(estimate - left);
						const int distUp = std::abs(estimate - up);
						const int distUpLeft = std::abs(estimate - upLeft);
						if (distLeft <= distUp && distLeft <= distUpLeft) { value += left; }
						else if (distUp <= distUpLeft) { value += up; }
						else { value += upLeft; }
						break;
					}
					default: break;
					}
					current[i] = static_cast<unsigned char>(value & 0xFF);
				}
				out.append(reinterpret_cast<const char*>(current.data()), rowBytes);
				previous = current;
			}
			return out;
		}

		// ---- object syntax ------------------------------------------------------------------

		// A literal `(...)` string, escapes resolved. `at` points at the opening parenthesis;
		// the return value is the index just past the closing one.
		size_t readLiteralString(const std::string& text, size_t at, std::string& out)
		{
			out.clear();
			if (at >= text.size() || text[at] != '(')
			{
				return at;
			}
			++at;
			int depth = 1;
			while (at < text.size())
			{
				const char c = text[at];
				if (c == '\\')
				{
					++at;
					if (at >= text.size())
					{
						break;
					}
					const char escaped = text[at];
					switch (escaped)
					{
					case 'n': out += '\n'; ++at; break;
					case 'r': out += '\r'; ++at; break;
					case 't': out += '\t'; ++at; break;
					case 'b': out += '\b'; ++at; break;
					case 'f': out += '\f'; ++at; break;
					case '(': out += '('; ++at; break;
					case ')': out += ')'; ++at; break;
					case '\\': out += '\\'; ++at; break;
					case '\r':
						// A backslash before an end-of-line splices two source lines and
						// contributes nothing to the string.
						++at;
						if (at < text.size() && text[at] == '\n') { ++at; }
						break;
					case '\n': ++at; break;
					default:
						if (escaped >= '0' && escaped <= '7')
						{
							int value = 0;
							int digits = 0;
							while (at < text.size() && digits < 3
								&& text[at] >= '0' && text[at] <= '7')
							{
								value = value * 8 + (text[at] - '0');
								++at;
								++digits;
							}
							out += static_cast<char>(value & 0xFF);
						}
						else
						{
							out += escaped;
							++at;
						}
						break;
					}
					continue;
				}
				if (c == '(')
				{
					++depth;
					out += c;
					++at;
					continue;
				}
				if (c == ')')
				{
					--depth;
					++at;
					if (depth == 0)
					{
						return at;
					}
					out += c;
					continue;
				}
				out += c;
				++at;
			}
			return at;
		}

		// A `<...>` hex string, decoded. `at` points at `<`.
		size_t readHexString(const std::string& text, size_t at, std::string& out)
		{
			out.clear();
			if (at >= text.size() || text[at] != '<')
			{
				return at;
			}
			++at;
			int high = -1;
			while (at < text.size() && text[at] != '>')
			{
				const int digit = hexValue(text[at]);
				++at;
				if (digit < 0)
				{
					continue;
				}
				if (high < 0)
				{
					high = digit;
				}
				else
				{
					out += static_cast<char>((high << 4) | digit);
					high = -1;
				}
			}
			if (high >= 0)
			{
				out += static_cast<char>(high << 4);
			}
			return at < text.size() ? at + 1 : at;
		}

		// One object-level token, returned as its own source text (delimiters included) so that
		// a dictionary value can be handed straight back to the parser. Balanced for `<<>>`,
		// `[]` and `()`, which is what keeps a nested `/Length` from being read as the outer
		// stream's.
		size_t readRawToken(const std::string& text, size_t at, std::string& out)
		{
			out.clear();
			at = skipBlanks(text, at);
			if (at >= text.size())
			{
				return at;
			}
			const size_t start = at;
			const char c = text[at];
			if (c == '<' && at + 1 < text.size() && text[at + 1] == '<')
			{
				size_t j = at + 2;
				int depth = 1;
				while (j < text.size() && depth > 0)
				{
					if (text[j] == '(')
					{
						std::string ignored;
						j = readLiteralString(text, j, ignored);
						continue;
					}
					if (text[j] == '%')
					{
						while (j < text.size() && text[j] != '\n' && text[j] != '\r') { ++j; }
						continue;
					}
					if (text[j] == '<' && j + 1 < text.size() && text[j + 1] == '<')
					{
						++depth;
						j += 2;
						continue;
					}
					if (text[j] == '<')
					{
						std::string ignored;
						j = readHexString(text, j, ignored);
						continue;
					}
					if (text[j] == '>' && j + 1 < text.size() && text[j + 1] == '>')
					{
						--depth;
						j += 2;
						continue;
					}
					++j;
				}
				out = text.substr(start, j - start);
				return j;
			}
			if (c == '[')
			{
				size_t j = at + 1;
				int depth = 1;
				while (j < text.size() && depth > 0)
				{
					if (text[j] == '(')
					{
						std::string ignored;
						j = readLiteralString(text, j, ignored);
						continue;
					}
					if (text[j] == '<' && (j + 1 >= text.size() || text[j + 1] != '<'))
					{
						std::string ignored;
						j = readHexString(text, j, ignored);
						continue;
					}
					if (text[j] == '[') { ++depth; }
					else if (text[j] == ']') { --depth; }
					++j;
				}
				out = text.substr(start, j - start);
				return j;
			}
			if (c == '(')
			{
				std::string ignored;
				const size_t end = readLiteralString(text, at, ignored);
				out = text.substr(start, end - start);
				return end;
			}
			if (c == '<')
			{
				std::string ignored;
				const size_t end = readHexString(text, at, ignored);
				out = text.substr(start, end - start);
				return end;
			}
			if (c == '/')
			{
				size_t j = at + 1;
				while (j < text.size() && isPdfRegular(text[j])) { ++j; }
				out = text.substr(start, j - start);
				return j;
			}
			if (isPdfDelimiter(c))
			{
				out = text.substr(start, 1);
				return at + 1;
			}
			size_t j = at;
			while (j < text.size() && isPdfRegular(text[j])) { ++j; }
			if (j == at) { ++j; }
			out = text.substr(start, j - start);
			return j;
		}

		bool looksLikeInteger(const std::string& token)
		{
			if (token.empty())
			{
				return false;
			}
			for (const char c : token)
			{
				if (!isDigit(c))
				{
					return false;
				}
			}
			return true;
		}

		// A dictionary as its entries, in file order. A vector because a PDF dictionary holds a
		// handful of keys and a linear scan over those beats the allocation a map costs per
		// object — and every page in the document builds one.
		using PdfDict = std::vector<std::pair<std::string, std::string>>;

		std::string dictGet(const PdfDict& dict, const char* key)
		{
			for (const std::pair<std::string, std::string>& entry : dict)
			{
				if (entry.first == key)
				{
					return entry.second;
				}
			}
			return std::string();
		}

		bool dictHas(const PdfDict& dict, const char* key)
		{
			for (const std::pair<std::string, std::string>& entry : dict)
			{
				if (entry.first == key)
				{
					return true;
				}
			}
			return false;
		}

		// `<< /Key value ... >>` into pairs. A value that is an indirect reference is kept in
		// its `N G R` form so a caller can decide whether to follow it.
		PdfDict parseDict(const std::string& text)
		{
			PdfDict dict;
			const size_t open = text.find("<<");
			if (open == std::string::npos)
			{
				return dict;
			}
			size_t at = open + 2;
			while (at < text.size())
			{
				at = skipBlanks(text, at);
				if (at + 1 < text.size() && text[at] == '>' && text[at + 1] == '>')
				{
					break;
				}
				if (at >= text.size() || text[at] != '/')
				{
					// Not a key where one belongs: step over the stray token rather than
					// giving up on the whole dictionary, which is usually still readable.
					std::string skipped;
					const size_t next = readRawToken(text, at, skipped);
					if (next <= at)
					{
						break;
					}
					at = next;
					continue;
				}
				std::string key;
				at = readRawToken(text, at, key);
				std::string value;
				at = readRawToken(text, at, value);
				if (looksLikeInteger(value))
				{
					// `12 0 R` arrives as three tokens. Peeking for the pair is what turns it
					// back into one value; anything else leaves `at` where it was.
					const size_t save = at;
					std::string generation;
					const size_t afterGeneration = readRawToken(text, at, generation);
					std::string marker;
					const size_t afterMarker = readRawToken(text, afterGeneration, marker);
					if (looksLikeInteger(generation) && marker == "R")
					{
						value += " " + generation + " R";
						at = afterMarker;
					}
					else
					{
						at = save;
					}
				}
				dict.push_back(std::make_pair(key, value));
			}
			return dict;
		}

		// The object number a `N G R` token names, or 0.
		int referenceTarget(const std::string& token)
		{
			size_t at = 0;
			std::string number;
			at = readRawToken(token, at, number);
			std::string generation;
			at = readRawToken(token, at, generation);
			std::string marker;
			readRawToken(token, at, marker);
			if (marker != "R" || !looksLikeInteger(number))
			{
				return 0;
			}
			return std::atoi(number.c_str());
		}

		// The elements of a `[ ... ]` token, each as its own source text.
		std::vector<std::string> parseArray(const std::string& token)
		{
			std::vector<std::string> items;
			const size_t open = token.find('[');
			if (open == std::string::npos)
			{
				if (!token.empty())
				{
					items.push_back(token);
				}
				return items;
			}
			size_t at = open + 1;
			while (at < token.size())
			{
				at = skipBlanks(token, at);
				if (at >= token.size() || token[at] == ']')
				{
					break;
				}
				std::string value;
				const size_t next = readRawToken(token, at, value);
				if (next <= at || value.empty())
				{
					break;
				}
				at = next;
				if (looksLikeInteger(value))
				{
					const size_t save = at;
					std::string generation;
					const size_t afterGeneration = readRawToken(token, at, generation);
					std::string marker;
					const size_t afterMarker = readRawToken(token, afterGeneration, marker);
					if (looksLikeInteger(generation) && marker == "R")
					{
						value += " " + generation + " R";
						at = afterMarker;
					}
					else
					{
						at = save;
					}
				}
				items.push_back(value);
			}
			return items;
		}

		double toNumber(const std::string& token)
		{
			return std::atof(token.c_str());
		}

		// ---- the document ---------------------------------------------------------------------

		struct RawObject
		{
			std::string body;       // everything between `obj` and `stream`/`endobj`
			std::string stream;     // still filtered; empty when the object carries none
			bool hasStream = false;
		};

		struct PdfDocument
		{
			std::map<int, RawObject> objects;
			bool encrypted = false;
			bool hasImageData = false;
			int rootObject = 0;

			const RawObject* object(int number) const
			{
				const std::map<int, RawObject>::const_iterator found = objects.find(number);
				return found == objects.end() ? nullptr : &found->second;
			}

			// A value that may be `N G R`, followed to the object it names. Returns the token
			// unchanged when it is already direct.
			std::string resolve(const std::string& token) const
			{
				const int target = referenceTarget(token);
				if (target == 0)
				{
					return token;
				}
				const RawObject* found = object(target);
				return found == nullptr ? std::string() : found->body;
			}

			bool resolveInt(const std::string& token, long long& outValue) const
			{
				const std::string direct = resolve(token);
				if (direct.empty())
				{
					return false;
				}
				const std::string trimmed = direct.substr(skipBlanks(direct, 0));
				if (trimmed.empty() || (!isDigit(trimmed[0]) && trimmed[0] != '-'
					&& trimmed[0] != '+'))
				{
					return false;
				}
				outValue = std::atoll(trimmed.c_str());
				return true;
			}

			// The decoded bytes of an object's stream, every filter in the chain applied in
			// order. Empty when the object has no stream, or when a filter in the chain is one
			// this does not implement — an image filter, which is deliberate: nothing here wants
			// the pixels.
			std::string decodedStream(int number) const;
		};

		bool filterIsImage(const std::string& name)
		{
			return name == "/DCTDecode" || name == "/CCITTFaxDecode" || name == "/JPXDecode"
				|| name == "/JBIG2Decode";
		}

		std::string PdfDocument::decodedStream(int number) const
		{
			const RawObject* raw = object(number);
			if (raw == nullptr || !raw->hasStream)
			{
				return std::string();
			}
			const PdfDict dict = parseDict(raw->body);
			std::string filterToken = dictGet(dict, "/Filter");
			if (filterToken.empty())
			{
				filterToken = dictGet(dict, "/F");
			}
			std::vector<std::string> filters;
			if (!filterToken.empty())
			{
				const std::string direct = resolve(filterToken);
				filters = direct.find('[') != std::string::npos
					? parseArray(direct)
					: std::vector<std::string>(1, direct);
			}

			std::string parmsToken = dictGet(dict, "/DecodeParms");
			if (parmsToken.empty())
			{
				parmsToken = dictGet(dict, "/DP");
			}
			std::vector<std::string> parms;
			if (!parmsToken.empty())
			{
				const std::string direct = resolve(parmsToken);
				parms = direct.find('[') == 0
					? parseArray(direct)
					: std::vector<std::string>(1, direct);
			}

			std::string data = raw->stream;
			for (size_t i = 0; i < filters.size(); ++i)
			{
				const std::string& filter = filters[i];
				if (filterIsImage(filter))
				{
					return std::string();
				}
				if (filter == "/FlateDecode" || filter == "/Fl")
				{
					data = flateDecode(data);
				}
				else if (filter == "/LZWDecode" || filter == "/LZW")
				{
					long long early = 1;
					if (i < parms.size())
					{
						const PdfDict parm = parseDict(resolve(parms[i]));
						const std::string value = dictGet(parm, "/EarlyChange");
						if (!value.empty())
						{
							resolveInt(value, early);
						}
					}
					data = lzwDecode(data, static_cast<int>(early));
				}
				else if (filter == "/ASCIIHexDecode" || filter == "/AHx")
				{
					data = asciiHexDecode(data);
					continue;
				}
				else if (filter == "/ASCII85Decode" || filter == "/A85")
				{
					data = ascii85Decode(data);
					continue;
				}
				else if (filter == "/RunLengthDecode" || filter == "/RL")
				{
					data = runLengthDecode(data);
					continue;
				}
				else if (filter == "/Crypt")
				{
					continue;
				}
				else
				{
					return std::string();
				}

				if (i < parms.size())
				{
					const PdfDict parm = parseDict(resolve(parms[i]));
					long long predictor = 1;
					long long colors = 1;
					long long bits = 8;
					long long columns = 1;
					const std::string predictorToken = dictGet(parm, "/Predictor");
					if (!predictorToken.empty())
					{
						resolveInt(predictorToken, predictor);
					}
					if (predictor >= 2)
					{
						const std::string colorsToken = dictGet(parm, "/Colors");
						if (!colorsToken.empty()) { resolveInt(colorsToken, colors); }
						const std::string bitsToken = dictGet(parm, "/BitsPerComponent");
						if (!bitsToken.empty()) { resolveInt(bitsToken, bits); }
						const std::string columnsToken = dictGet(parm, "/Columns");
						if (!columnsToken.empty()) { resolveInt(columnsToken, columns); }
						data = applyPredictor(data, static_cast<int>(predictor),
							static_cast<int>(colors), static_cast<int>(bits),
							static_cast<int>(columns));
					}
				}
			}
			return data;
		}

		// Walks the file for `N G obj … endobj`, skipping *over* stream bodies rather than
		// through them. That is the whole reason this is a hand-written scan and not a search
		// for the literal `obj`: a compressed stream contains those three bytes often enough
		// that a naive scan invents objects out of image data.
		void scanObjects(const std::string& bytes, PdfDocument& document)
		{
			size_t at = 0;
			while (at + 3 < bytes.size())
			{
				const size_t found = bytes.find("obj", at);
				if (found == std::string::npos)
				{
					break;
				}
				at = found + 3;
				if (found + 3 < bytes.size() && isPdfRegular(bytes[found + 3]))
				{
					continue;
				}
				// Back over `<number> <generation> ` to prove this is an object header.
				size_t cursor = found;
				while (cursor > 0 && isPdfBlank(bytes[cursor - 1])) { --cursor; }
				const size_t generationEnd = cursor;
				while (cursor > 0 && isDigit(bytes[cursor - 1])) { --cursor; }
				if (cursor == generationEnd) { continue; }
				if (cursor == 0 || !isPdfBlank(bytes[cursor - 1])) { continue; }
				while (cursor > 0 && isPdfBlank(bytes[cursor - 1])) { --cursor; }
				const size_t numberEnd = cursor;
				while (cursor > 0 && isDigit(bytes[cursor - 1])) { --cursor; }
				if (cursor == numberEnd) { continue; }
				const size_t numberStart = cursor;
				if (cursor > 0 && isPdfRegular(bytes[cursor - 1])) { continue; }

				const int number = std::atoi(bytes.substr(numberStart,
					numberEnd - numberStart).c_str());
				if (number <= 0)
				{
					continue;
				}

				RawObject entry;
				size_t cursorAt = skipBlanks(bytes, found + 3);
				std::string body;
				const size_t afterBody = readRawToken(bytes, cursorAt, body);
				entry.body = body;
				size_t after = skipBlanks(bytes, afterBody);
				if (bytes.compare(after, 6, "stream") == 0)
				{
					size_t dataStart = after + 6;
					// The spec says CRLF or LF. Producers write a bare CR too, and the crude
					// corpus scan that preceded this slice missed those files entirely.
					if (dataStart < bytes.size() && bytes[dataStart] == '\r') { ++dataStart; }
					if (dataStart < bytes.size() && bytes[dataStart] == '\n') { ++dataStart; }

					size_t dataEnd = std::string::npos;
					const PdfDict dict = parseDict(entry.body);
					const std::string lengthToken = dictGet(dict, "/Length");
					if (looksLikeInteger(lengthToken))
					{
						const size_t declared = static_cast<size_t>(std::atoll(lengthToken.c_str()));
						const size_t candidate = dataStart + declared;
						if (candidate <= bytes.size())
						{
							const size_t probe = skipBlanks(bytes, candidate);
							if (bytes.compare(probe, 9, "endstream") == 0)
							{
								dataEnd = candidate;
							}
						}
					}
					if (dataEnd == std::string::npos)
					{
						// An indirect `/Length`, or one that lies. Both happen; `endstream` is
						// the only marker that is always there.
						const size_t marker = bytes.find("endstream", dataStart);
						dataEnd = marker == std::string::npos ? bytes.size() : marker;
						while (dataEnd > dataStart
							&& (bytes[dataEnd - 1] == '\n' || bytes[dataEnd - 1] == '\r'))
						{
							--dataEnd;
						}
					}
					entry.stream = bytes.substr(dataStart, dataEnd - dataStart);
					entry.hasStream = true;

					const size_t marker = bytes.find("endstream", dataEnd);
					at = marker == std::string::npos ? bytes.size() : marker + 9;

					if (dictGet(dict, "/Subtype") == "/Image")
					{
						document.hasImageData = true;
					}
					const std::string filterToken = dictGet(dict, "/Filter");
					if (!filterToken.empty())
					{
						const std::vector<std::string> filters =
							filterToken.find('[') != std::string::npos
								? parseArray(filterToken)
								: std::vector<std::string>(1, filterToken);
						for (const std::string& filter : filters)
						{
							if (filterIsImage(filter))
							{
								document.hasImageData = true;
							}
						}
					}
				}
				else
				{
					const size_t marker = bytes.find("endobj", afterBody);
					at = marker == std::string::npos ? bytes.size() : marker + 6;
				}
				// Later definitions win, which is what an incrementally updated PDF means by
				// appending a new version of an object at the end of the file.
				document.objects[number] = entry;
			}
		}

		// `/Type /ObjStm`: a stream holding whole objects, which is how every PDF written this
		// decade stores its catalogue and page tree. Missing this is why a crude scan finds no
		// pages in a file that is perfectly readable.
		void expandObjectStreams(PdfDocument& document)
		{
			std::vector<int> containers;
			for (const std::pair<const int, RawObject>& entry : document.objects)
			{
				if (entry.second.hasStream
					&& dictGet(parseDict(entry.second.body), "/Type") == "/ObjStm")
				{
					containers.push_back(entry.first);
				}
			}
			for (const int number : containers)
			{
				const RawObject* raw = document.object(number);
				if (raw == nullptr)
				{
					continue;
				}
				const PdfDict dict = parseDict(raw->body);
				long long count = 0;
				long long first = 0;
				if (!document.resolveInt(dictGet(dict, "/N"), count)
					|| !document.resolveInt(dictGet(dict, "/First"), first))
				{
					continue;
				}
				const std::string data = document.decodedStream(number);
				if (data.empty() || first < 0 || static_cast<size_t>(first) > data.size())
				{
					continue;
				}
				// The header is N pairs of `objectNumber offset`, the offsets relative to
				// /First.
				std::vector<std::pair<int, size_t>> entries;
				size_t at = 0;
				for (long long i = 0; i < count; ++i)
				{
					std::string numberToken;
					at = readRawToken(data, at, numberToken);
					std::string offsetToken;
					at = readRawToken(data, at, offsetToken);
					if (!looksLikeInteger(numberToken) || !looksLikeInteger(offsetToken))
					{
						break;
					}
					entries.push_back(std::make_pair(std::atoi(numberToken.c_str()),
						static_cast<size_t>(std::atoll(offsetToken.c_str()))));
				}
				for (size_t i = 0; i < entries.size(); ++i)
				{
					const size_t start = static_cast<size_t>(first) + entries[i].second;
					if (start >= data.size())
					{
						continue;
					}
					RawObject embedded;
					std::string body;
					readRawToken(data, start, body);
					embedded.body = body;
					// A file-level definition of the same object number is a later revision and
					// must not be overwritten by the one still inside the stream.
					if (document.objects.find(entries[i].first) == document.objects.end())
					{
						document.objects[entries[i].first] = embedded;
					}
				}
			}
		}

		// `/Encrypt` and `/Root`, from wherever this file keeps its trailer: the classic
		// `trailer << … >>` keyword, or the `/Type /XRef` stream that replaced it.
		void readTrailer(const std::string& bytes, PdfDocument& document)
		{
			size_t at = 0;
			while (true)
			{
				const size_t found = bytes.find("trailer", at);
				if (found == std::string::npos)
				{
					break;
				}
				at = found + 7;
				const PdfDict dict = parseDict(bytes.substr(found,
					std::min<size_t>(4096, bytes.size() - found)));
				if (dictHas(dict, "/Encrypt"))
				{
					document.encrypted = true;
				}
				const int root = referenceTarget(dictGet(dict, "/Root"));
				if (root != 0)
				{
					document.rootObject = root;
				}
			}
			for (const std::pair<const int, RawObject>& entry : document.objects)
			{
				const PdfDict dict = parseDict(entry.second.body);
				if (dictGet(dict, "/Type") != "/XRef")
				{
					continue;
				}
				if (dictHas(dict, "/Encrypt"))
				{
					document.encrypted = true;
				}
				const int root = referenceTarget(dictGet(dict, "/Root"));
				if (root != 0 && document.rootObject == 0)
				{
					document.rootObject = root;
				}
			}
			if (document.rootObject == 0)
			{
				for (const std::pair<const int, RawObject>& entry : document.objects)
				{
					if (dictGet(parseDict(entry.second.body), "/Type") == "/Catalog")
					{
						document.rootObject = entry.first;
						break;
					}
				}
			}
		}

		// ---- fonts --------------------------------------------------------------------------

		struct FontInfo
		{
			// Identity-H and friends address glyphs with two bytes. Getting this wrong halves or
			// doubles every string, which reads as noise rather than as an error.
			bool twoByte = false;
			std::map<unsigned int, std::string> toUnicode;
			std::map<unsigned int, std::string> differences;
			const unsigned short* baseEncoding = nullptr;
		};

		unsigned int hexToCode(const std::string& bytes)
		{
			unsigned int value = 0;
			for (const char c : bytes)
			{
				value = (value << 8) | static_cast<unsigned char>(c);
			}
			return value;
		}

		// `beginbfchar`/`beginbfrange` out of a `/ToUnicode` CMap. This is the part that turns a
		// subset-embedded font — where code 3 is whatever glyph the subsetter put third — back
		// into readable text, and it is the difference between a datasheet and mojibake.
		void parseToUnicodeCMap(const std::string& cmap, FontInfo& font)
		{
			size_t at = 0;
			while (at < cmap.size())
			{
				const size_t rangeStart = cmap.find("begincodespacerange", at);
				if (rangeStart == std::string::npos)
				{
					break;
				}
				size_t cursor = rangeStart + 19;
				std::string low;
				const size_t afterLow = readRawToken(cmap, cursor, low);
				if (low.size() >= 2 && low[0] == '<')
				{
					std::string decoded;
					readHexString(low, 0, decoded);
					if (decoded.size() >= 2)
					{
						font.twoByte = true;
					}
				}
				at = afterLow;
			}

			at = 0;
			while (true)
			{
				const size_t start = cmap.find("beginbfchar", at);
				if (start == std::string::npos)
				{
					break;
				}
				const size_t end = cmap.find("endbfchar", start);
				const size_t stop = end == std::string::npos ? cmap.size() : end;
				size_t cursor = start + 11;
				while (cursor < stop)
				{
					std::string source;
					const size_t afterSource = readRawToken(cmap, cursor, source);
					if (afterSource <= cursor || afterSource > stop || source.empty())
					{
						break;
					}
					std::string destination;
					const size_t afterDestination =
						readRawToken(cmap, afterSource, destination);
					if (afterDestination <= afterSource || destination.empty())
					{
						break;
					}
					cursor = afterDestination;
					if (source[0] != '<')
					{
						continue;
					}
					std::string sourceBytes;
					readHexString(source, 0, sourceBytes);
					if (sourceBytes.size() >= 2)
					{
						font.twoByte = true;
					}
					std::string text;
					if (destination[0] == '<')
					{
						std::string destinationBytes;
						readHexString(destination, 0, destinationBytes);
						text = utf16BeToUtf8(destinationBytes);
					}
					else if (destination[0] == '/')
					{
						const unsigned int code = glyphNameToChar(destination.substr(1));
						if (code != 0)
						{
							appendUtf8(text, code);
						}
					}
					if (!text.empty())
					{
						font.toUnicode[hexToCode(sourceBytes)] = text;
					}
				}
				at = stop + 9;
			}

			at = 0;
			while (true)
			{
				const size_t start = cmap.find("beginbfrange", at);
				if (start == std::string::npos)
				{
					break;
				}
				const size_t end = cmap.find("endbfrange", start);
				const size_t stop = end == std::string::npos ? cmap.size() : end;
				size_t cursor = start + 12;
				while (cursor < stop)
				{
					std::string lowToken;
					const size_t afterLow = readRawToken(cmap, cursor, lowToken);
					if (afterLow <= cursor || afterLow > stop || lowToken.empty()
						|| lowToken[0] != '<')
					{
						break;
					}
					std::string highToken;
					const size_t afterHigh = readRawToken(cmap, afterLow, highToken);
					if (afterHigh <= afterLow || highToken.empty())
					{
						break;
					}
					std::string destination;
					const size_t afterDestination =
						readRawToken(cmap, afterHigh, destination);
					if (afterDestination <= afterHigh || destination.empty())
					{
						break;
					}
					cursor = afterDestination;

					std::string lowBytes;
					readHexString(lowToken, 0, lowBytes);
					std::string highBytes;
					readHexString(highToken, 0, highBytes);
					if (lowBytes.size() >= 2)
					{
						font.twoByte = true;
					}
					const unsigned int low = hexToCode(lowBytes);
					const unsigned int high = hexToCode(highBytes);
					if (high < low || high - low > 0xFFFFu)
					{
						continue;
					}
					if (destination[0] == '[')
					{
						const std::vector<std::string> items = parseArray(destination);
						for (size_t i = 0; i < items.size() && low + i <= high; ++i)
						{
							if (items[i].empty() || items[i][0] != '<')
							{
								continue;
							}
							std::string bytes;
							readHexString(items[i], 0, bytes);
							const std::string text = utf16BeToUtf8(bytes);
							if (!text.empty())
							{
								font.toUnicode[low + static_cast<unsigned int>(i)] = text;
							}
						}
						continue;
					}
					if (destination[0] != '<')
					{
						continue;
					}
					std::string destinationBytes;
					readHexString(destination, 0, destinationBytes);
					if (destinationBytes.size() < 2)
					{
						continue;
					}
					// The last UTF-16 unit counts up across the range; anything before it is a
					// fixed prefix, which is how a ligature range is written.
					for (unsigned int code = low; code <= high; ++code)
					{
						std::string bytes = destinationBytes;
						const unsigned int step = code - low;
						unsigned int last = static_cast<unsigned int>(
							(static_cast<unsigned char>(bytes[bytes.size() - 2]) << 8)
							| static_cast<unsigned char>(bytes[bytes.size() - 1]));
						last += step;
						bytes[bytes.size() - 2] = static_cast<char>((last >> 8) & 0xFFu);
						bytes[bytes.size() - 1] = static_cast<char>(last & 0xFFu);
						const std::string text = utf16BeToUtf8(bytes);
						if (!text.empty())
						{
							font.toUnicode[code] = text;
						}
					}
				}
				at = stop + 10;
			}
		}

		FontInfo readFont(const PdfDocument& document, const std::string& fontToken)
		{
			FontInfo font;
			font.baseEncoding = winAnsiTable();
			const std::string body = document.resolve(fontToken);
			if (body.empty())
			{
				return font;
			}
			const PdfDict dict = parseDict(body);
			if (dictGet(dict, "/Subtype") == "/Type0")
			{
				font.twoByte = true;
			}
			const std::string encodingToken = dictGet(dict, "/Encoding");
			if (encodingToken == "/Identity-H" || encodingToken == "/Identity-V")
			{
				font.twoByte = true;
			}
			else if (encodingToken == "/MacRomanEncoding")
			{
				font.baseEncoding = macRomanTable();
			}
			else if (!encodingToken.empty() && encodingToken[0] != '/')
			{
				const std::string encodingBody = document.resolve(encodingToken);
				const PdfDict encoding = parseDict(encodingBody);
				if (dictGet(encoding, "/BaseEncoding") == "/MacRomanEncoding")
				{
					font.baseEncoding = macRomanTable();
				}
				const std::string differencesToken = dictGet(encoding, "/Differences");
				if (!differencesToken.empty())
				{
					const std::vector<std::string> items =
						parseArray(document.resolve(differencesToken));
					unsigned int next = 0;
					for (const std::string& item : items)
					{
						if (item.empty())
						{
							continue;
						}
						if (item[0] == '/')
						{
							const unsigned int code = glyphNameToChar(item.substr(1));
							if (code != 0)
							{
								std::string text;
								appendUtf8(text, code);
								font.differences[next] = text;
							}
							++next;
						}
						else if (isDigit(item[0]))
						{
							next = static_cast<unsigned int>(std::atoll(item.c_str()));
						}
					}
				}
			}

			const std::string toUnicodeToken = dictGet(dict, "/ToUnicode");
			const int toUnicodeObject = referenceTarget(toUnicodeToken);
			if (toUnicodeObject != 0)
			{
				const std::string cmap = document.decodedStream(toUnicodeObject);
				if (!cmap.empty())
				{
					parseToUnicodeCMap(cmap, font);
				}
			}
			return font;
		}

		// ---- content streams ------------------------------------------------------------------

		// One operand on the content stream's stack. Arrays are kept whole because `TJ` — the
		// operator that carries almost all the text in a modern PDF — is an array of alternating
		// strings and kerning numbers, and the numbers are where the spaces come from.
		struct Operand
		{
			bool isNumber = false;
			double number = 0.0;
			bool isString = false;
			std::string bytes;
			std::string name;
			bool isArray = false;
			std::vector<Operand> items;
		};

		// What one page's text is built in. Position tracking is deliberately translation-only:
		// this is a text extractor, not a layout engine (see the header), and `e`/`f` out of the
		// text matrix is all that is needed to tell one line from the next.
		struct TextState
		{
			std::string out;
			const FontInfo* font = nullptr;
			double lineX = 0.0;
			double lineY = 0.0;
			double leading = 0.0;
			bool positioned = false;
			bool sawTextOperator = false;

			void appendSpace()
			{
				if (!out.empty() && out[out.size() - 1] != ' ' && out[out.size() - 1] != '\n')
				{
					out += ' ';
				}
			}

			void appendNewline()
			{
				if (!out.empty() && out[out.size() - 1] != '\n')
				{
					out += '\n';
				}
			}
		};

		void showText(TextState& state, const std::string& bytes)
		{
			state.sawTextOperator = true;
			const FontInfo* font = state.font;
			const unsigned short* base = font != nullptr && font->baseEncoding != nullptr
				? font->baseEncoding : winAnsiTable();
			const bool twoByte = font != nullptr && font->twoByte;
			const size_t step = twoByte ? 2u : 1u;
			for (size_t i = 0; i + step <= bytes.size(); i += step)
			{
				unsigned int code = static_cast<unsigned char>(bytes[i]);
				if (twoByte)
				{
					code = (code << 8) | static_cast<unsigned char>(bytes[i + 1]);
				}
				if (font != nullptr)
				{
					const std::map<unsigned int, std::string>::const_iterator mapped =
						font->toUnicode.find(code);
					if (mapped != font->toUnicode.end())
					{
						state.out += mapped->second;
						continue;
					}
					const std::map<unsigned int, std::string>::const_iterator different =
						font->differences.find(code);
					if (different != font->differences.end())
					{
						state.out += different->second;
						continue;
					}
				}
				if (twoByte)
				{
					// A two-byte font with no usable `/ToUnicode` is a subset whose codes are
					// glyph indices. Writing the index out as a character is exactly the
					// invented-specification failure this feature must not have, so it
					// contributes nothing except, where the gap is real, a space.
					if (code == 0 || code == 32)
					{
						state.appendSpace();
					}
					continue;
				}
				const unsigned int mapped = base[code & 0xFFu];
				if (mapped != 0)
				{
					appendUtf8(state.out, mapped);
				}
			}
		}

		void applyPositionChange(TextState& state, double newX, double newY)
		{
			if (!state.positioned)
			{
				state.positioned = true;
				state.lineX = newX;
				state.lineY = newY;
				return;
			}
			if (std::abs(newY - state.lineY) > LineBreakEpsilon)
			{
				state.appendNewline();
			}
			else if (std::abs(newX - state.lineX) > 0.01)
			{
				state.appendSpace();
			}
			state.lineX = newX;
			state.lineY = newY;
		}

		void extractContent(const PdfDocument& document, const std::string& content,
			const std::map<std::string, FontInfo>& fonts, const PdfDict& resources,
			TextState& state, int depth);

		// A `/Subtype /Form` XObject is a content stream of its own, and on plenty of
		// vendor-generated datasheets it is where all the body text lives — a page whose own
		// stream is three operators long and one `Do`.
		void runFormXObject(const PdfDocument& document, const std::string& name,
			const PdfDict& resources, TextState& state, int depth)
		{
			if (depth >= MaxFormXObjectDepth)
			{
				return;
			}
			const std::string xobjectToken = dictGet(resources, "/XObject");
			if (xobjectToken.empty())
			{
				return;
			}
			const PdfDict xobjects = parseDict(document.resolve(xobjectToken));
			const std::string entry = dictGet(xobjects, name.c_str());
			const int target = referenceTarget(entry);
			if (target == 0)
			{
				return;
			}
			const RawObject* raw = document.object(target);
			if (raw == nullptr || !raw->hasStream)
			{
				return;
			}
			const PdfDict dict = parseDict(raw->body);
			if (dictGet(dict, "/Subtype") != "/Form")
			{
				return;
			}
			const std::string inner = document.decodedStream(target);
			if (inner.empty())
			{
				return;
			}
			PdfDict innerResources = resources;
			const std::string ownResources = dictGet(dict, "/Resources");
			if (!ownResources.empty())
			{
				innerResources = parseDict(document.resolve(ownResources));
			}
			std::map<std::string, FontInfo> innerFonts;
			const std::string fontToken = dictGet(innerResources, "/Font");
			if (!fontToken.empty())
			{
				const PdfDict fontDict = parseDict(document.resolve(fontToken));
				for (const std::pair<std::string, std::string>& font : fontDict)
				{
					innerFonts[font.first] = readFont(document, font.second);
				}
			}
			const FontInfo* outerFont = state.font;
			state.font = nullptr;
			extractContent(document, inner, innerFonts, innerResources, state, depth + 1);
			state.font = outerFont;
		}

		// `BI … ID <binary> EI`. The binary payload is not content-stream syntax, so tokenizing
		// it produces garbage operators; stepping over it is the only correct thing to do.
		size_t skipInlineImage(const std::string& content, size_t at)
		{
			const size_t dataStart = content.find("ID", at);
			if (dataStart == std::string::npos)
			{
				return content.size();
			}
			size_t cursor = dataStart + 2;
			if (cursor < content.size() && isPdfBlank(content[cursor]))
			{
				++cursor;
			}
			while (cursor + 1 < content.size())
			{
				if (content[cursor] == 'E' && content[cursor + 1] == 'I'
					&& (cursor == 0 || isPdfBlank(content[cursor - 1]))
					&& (cursor + 2 >= content.size() || !isPdfRegular(content[cursor + 2])))
				{
					return cursor + 2;
				}
				++cursor;
			}
			return content.size();
		}

		size_t readOperand(const std::string& content, size_t at, Operand& out, int depth);

		size_t readArrayOperand(const std::string& content, size_t at, Operand& out, int depth)
		{
			out = Operand();
			out.isArray = true;
			++at;
			while (at < content.size())
			{
				at = skipBlanks(content, at);
				if (at >= content.size() || content[at] == ']')
				{
					return at < content.size() ? at + 1 : at;
				}
				Operand item;
				const size_t next = readOperand(content, at, item, depth + 1);
				if (next <= at)
				{
					break;
				}
				at = next;
				out.items.push_back(item);
			}
			return at;
		}

		size_t readOperand(const std::string& content, size_t at, Operand& out, int depth)
		{
			out = Operand();
			at = skipBlanks(content, at);
			if (at >= content.size())
			{
				return at;
			}
			const char c = content[at];
			if (c == '(')
			{
				out.isString = true;
				return readLiteralString(content, at, out.bytes);
			}
			if (c == '<' && (at + 1 >= content.size() || content[at + 1] != '<'))
			{
				out.isString = true;
				return readHexString(content, at, out.bytes);
			}
			if (c == '[' && depth < 4)
			{
				return readArrayOperand(content, at, out, depth);
			}
			if (c == '/')
			{
				std::string token;
				const size_t next = readRawToken(content, at, token);
				out.name = token.size() > 1 ? token.substr(1) : std::string();
				return next;
			}
			std::string token;
			const size_t next = readRawToken(content, at, token);
			if (!token.empty() && (isDigit(token[0]) || token[0] == '-' || token[0] == '+'
				|| token[0] == '.'))
			{
				out.isNumber = true;
				out.number = toNumber(token);
			}
			else
			{
				out.name = token;
			}
			return next;
		}

		void extractContent(const PdfDocument& document, const std::string& content,
			const std::map<std::string, FontInfo>& fonts, const PdfDict& resources,
			TextState& state, int depth)
		{
			std::vector<Operand> stack;
			size_t at = 0;
			while (at < content.size())
			{
				at = skipBlanks(content, at);
				if (at >= content.size())
				{
					break;
				}
				const char c = content[at];
				if (c == '(' || c == '[' || c == '/'
					|| (c == '<' && (at + 1 >= content.size() || content[at + 1] != '<')))
				{
					Operand operand;
					const size_t next = readOperand(content, at, operand, 0);
					if (next <= at)
					{
						++at;
						continue;
					}
					at = next;
					stack.push_back(operand);
					continue;
				}
				if (c == '<' || c == ']' || c == '>' || c == '{' || c == '}')
				{
					// A `<< … >>` operand belongs to BDC/DP and carries no text; the rest are
					// strays. Either way, skip the token and keep the stream readable.
					std::string ignored;
					const size_t next = readRawToken(content, at, ignored);
					at = next <= at ? at + 1 : next;
					continue;
				}
				if (isDigit(c) || c == '-' || c == '+' || c == '.')
				{
					Operand operand;
					const size_t next = readOperand(content, at, operand, 0);
					at = next <= at ? at + 1 : next;
					stack.push_back(operand);
					continue;
				}

				std::string op;
				const size_t next = readRawToken(content, at, op);
				at = next <= at ? at + 1 : next;
				if (op.empty())
				{
					continue;
				}

				if (op == "BT")
				{
					state.positioned = false;
					state.lineX = 0.0;
					state.lineY = 0.0;
					state.appendNewline();
				}
				else if (op == "ET")
				{
					state.positioned = false;
				}
				else if (op == "Tf")
				{
					if (stack.size() >= 2 && !stack[stack.size() - 2].name.empty())
					{
						const std::string key = "/" + stack[stack.size() - 2].name;
						const std::map<std::string, FontInfo>::const_iterator found =
							fonts.find(key);
						state.font = found == fonts.end() ? nullptr : &found->second;
					}
				}
				else if (op == "TL")
				{
					if (!stack.empty() && stack.back().isNumber)
					{
						state.leading = stack.back().number;
					}
				}
				else if (op == "Td" || op == "TD")
				{
					if (stack.size() >= 2)
					{
						const double tx = stack[stack.size() - 2].number;
						const double ty = stack[stack.size() - 1].number;
						if (op == "TD")
						{
							state.leading = -ty;
						}
						applyPositionChange(state, state.lineX + tx, state.lineY + ty);
					}
				}
				else if (op == "Tm")
				{
					if (stack.size() >= 6)
					{
						applyPositionChange(state, stack[stack.size() - 2].number,
							stack[stack.size() - 1].number);
					}
				}
				else if (op == "T*")
				{
					applyPositionChange(state, state.lineX, state.lineY - state.leading);
				}
				else if (op == "Tj")
				{
					if (!stack.empty() && stack.back().isString)
					{
						showText(state, stack.back().bytes);
					}
				}
				else if (op == "'")
				{
					applyPositionChange(state, state.lineX, state.lineY - state.leading);
					if (!stack.empty() && stack.back().isString)
					{
						showText(state, stack.back().bytes);
					}
				}
				else if (op == "\"")
				{
					applyPositionChange(state, state.lineX, state.lineY - state.leading);
					if (!stack.empty() && stack.back().isString)
					{
						showText(state, stack.back().bytes);
					}
				}
				else if (op == "TJ")
				{
					if (!stack.empty() && stack.back().isArray)
					{
						state.sawTextOperator = true;
						for (const Operand& item : stack.back().items)
						{
							if (item.isString)
							{
								showText(state, item.bytes);
							}
							else if (item.isNumber
								&& item.number <= -SpaceKerningThousandths)
							{
								state.appendSpace();
							}
						}
					}
				}
				else if (op == "Do")
				{
					if (!stack.empty() && !stack.back().name.empty())
					{
						runFormXObject(document, "/" + stack.back().name, resources, state,
							depth);
					}
				}
				else if (op == "BI")
				{
					at = skipInlineImage(content, at);
				}
				stack.clear();
			}
		}

		// ---- page tree --------------------------------------------------------------------

		struct PageEntry
		{
			int object = 0;
			PdfDict resources;
		};

		void collectPages(const PdfDocument& document, int number, const PdfDict& inherited,
			int depth, std::set<int>& visited, std::vector<PageEntry>& out)
		{
			if (depth > MaxPageTreeDepth || number == 0 || visited.count(number) != 0)
			{
				return;
			}
			visited.insert(number);
			const RawObject* raw = document.object(number);
			if (raw == nullptr)
			{
				return;
			}
			const PdfDict dict = parseDict(raw->body);
			PdfDict resources = inherited;
			const std::string resourcesToken = dictGet(dict, "/Resources");
			if (!resourcesToken.empty())
			{
				resources = parseDict(document.resolve(resourcesToken));
			}
			const std::string type = dictGet(dict, "/Type");
			const std::string kidsToken = dictGet(dict, "/Kids");
			if (type == "/Pages" || (type.empty() && !kidsToken.empty()))
			{
				const std::vector<std::string> kids =
					parseArray(document.resolve(kidsToken));
				for (const std::string& kid : kids)
				{
					collectPages(document, referenceTarget(kid), resources, depth + 1, visited,
						out);
				}
				return;
			}
			if (type == "/Page" || dictHas(dict, "/Contents"))
			{
				PageEntry entry;
				entry.object = number;
				entry.resources = resources;
				out.push_back(entry);
			}
		}

		std::vector<PageEntry> findPages(const PdfDocument& document)
		{
			std::vector<PageEntry> pages;
			std::set<int> visited;
			if (document.rootObject != 0)
			{
				const RawObject* root = document.object(document.rootObject);
				if (root != nullptr)
				{
					const PdfDict catalog = parseDict(root->body);
					const int pagesObject = referenceTarget(dictGet(catalog, "/Pages"));
					if (pagesObject != 0)
					{
						collectPages(document, pagesObject, PdfDict(), 0, visited, pages);
					}
				}
			}
			if (!pages.empty())
			{
				return pages;
			}
			// No usable catalogue — a damaged or hand-assembled file. Every `/Type /Page`
			// object in object-number order is not guaranteed to be the printed order, but it
			// is the whole text, and the page numbers it reports are the only ones available.
			for (const std::pair<const int, RawObject>& entry : document.objects)
			{
				const PdfDict dict = parseDict(entry.second.body);
				if (dictGet(dict, "/Type") != "/Page")
				{
					continue;
				}
				PageEntry page;
				page.object = entry.first;
				const std::string resourcesToken = dictGet(dict, "/Resources");
				if (!resourcesToken.empty())
				{
					page.resources = parseDict(document.resolve(resourcesToken));
				}
				pages.push_back(page);
			}
			return pages;
		}

		std::string pageContent(const PdfDocument& document, int pageObject)
		{
			const RawObject* raw = document.object(pageObject);
			if (raw == nullptr)
			{
				return std::string();
			}
			const PdfDict dict = parseDict(raw->body);
			const std::string contentsToken = dictGet(dict, "/Contents");
			if (contentsToken.empty())
			{
				return std::string();
			}
			std::vector<std::string> parts;
			if (contentsToken[0] == '[')
			{
				parts = parseArray(contentsToken);
			}
			else
			{
				const int target = referenceTarget(contentsToken);
				if (target != 0)
				{
					const RawObject* stream = document.object(target);
					if (stream != nullptr && !stream->hasStream)
					{
						// An indirect `/Contents` that points at an array rather than a stream.
						parts = parseArray(stream->body);
					}
					else
					{
						parts.push_back(contentsToken);
					}
				}
			}
			std::string content;
			for (const std::string& part : parts)
			{
				const int target = referenceTarget(part);
				if (target == 0)
				{
					continue;
				}
				const std::string decoded = document.decodedStream(target);
				if (decoded.empty())
				{
					continue;
				}
				// A newline between the pieces: the spec lets a single operator be split
				// across two streams, but joining them without a separator can also fuse two
				// operators into one nonsense token.
				content += decoded;
				content += '\n';
			}
			return content;
		}

		// Runs of blanks squeezed out, so what reaches the model is text and not the layout
		// engine's leftovers. Newlines survive as newlines, everything else becomes one space.
		std::string tidy(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			bool pendingSpace = false;
			bool pendingNewline = false;
			for (const char c : text)
			{
				if (c == '\n' || c == '\r')
				{
					pendingNewline = true;
					continue;
				}
				if (c == ' ' || c == '\t' || c == '\f' || c == '\v')
				{
					pendingSpace = true;
					continue;
				}
				if (pendingNewline)
				{
					if (!out.empty())
					{
						out += '\n';
					}
					pendingNewline = false;
					pendingSpace = false;
				}
				else if (pendingSpace)
				{
					if (!out.empty())
					{
						out += ' ';
					}
					pendingSpace = false;
				}
				out += c;
			}
			return out;
		}

		int countNonBlank(const std::string& text)
		{
			int count = 0;
			for (const char c : text)
			{
				if (!isPdfBlank(c))
				{
					++count;
				}
			}
			return count;
		}

		char asciiLower(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}

		// Lowercased, and every blank turned into a plain space — **without changing the length**,
		// so an offset into the folded copy is still an offset into the original.
		//
		// The line breaks matter: `tidy()` has already collapsed runs, so the only difference
		// between the two forms is which single separator sits between two words. Measured on the
		// user's own PCA9745B datasheet, searching "operating temperature" found nothing because
		// the phrase is set across a line break — which is not a property of the datasheet, only
		// of where the typesetter ran out of column.
		std::string searchFolded(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			for (const char c : text)
			{
				out += isPdfBlank(c) ? ' ' : asciiLower(c);
			}
			return out;
		}

		// The needle by the same rule, but runs *are* collapsed here: a model that types two
		// spaces means one.
		std::string foldedNeedle(const std::string& text)
		{
			std::string out;
			bool pendingSpace = false;
			for (const char c : text)
			{
				if (isPdfBlank(c))
				{
					pendingSpace = !out.empty();
					continue;
				}
				if (pendingSpace)
				{
					out += ' ';
					pendingSpace = false;
				}
				out += asciiLower(c);
			}
			return out;
		}
	}

	std::string PdfTextResult::allText() const
	{
		std::string joined;
		for (size_t i = 0; i < pages.size(); ++i)
		{
			if (i != 0)
			{
				joined += '\f';
			}
			joined += pages[i].text;
		}
		return joined;
	}

	PdfTextResult PdfText::extract(const std::string& filePath, int maxPages)
	{
		PdfTextResult result;
		if (filePath.empty())
		{
			result.error = "no file was named";
			return result;
		}
		std::ifstream stream(filePath, std::ios::binary);
		if (!stream)
		{
			result.error = "the file could not be opened: " + filePath;
			return result;
		}
		std::ostringstream buffer;
		buffer << stream.rdbuf();
		return extractBytes(buffer.str(), maxPages);
	}

	PdfTextResult PdfText::extractBytes(const std::string& bytes, int maxPages)
	{
		PdfTextResult result;
		if (bytes.empty())
		{
			result.error = "the file is empty";
			return result;
		}
		const size_t headerAt = bytes.find("%PDF-");
		if (headerAt == std::string::npos || headerAt > PdfHeaderSearchWindow)
		{
			result.error = "not a PDF: the file does not begin with %PDF";
			return result;
		}

		PdfDocument document;
		scanObjects(bytes, document);
		readTrailer(bytes, document);
		if (document.encrypted)
		{
			// Named rather than reported as an empty datasheet. Most encrypted datasheets carry
			// an empty user password and only restrict printing, but reading them still means
			// implementing the standard security handler — which this does not. Saying so is
			// what keeps a caller from answering the question from memory.
			result.error = "the PDF is encrypted (its trailer carries /Encrypt), so its text "
				"cannot be read without implementing the PDF security handler";
			return result;
		}
		expandObjectStreams(document);
		// `/Encrypt` can live inside an object stream's trailer on a cross-reference-stream
		// file, so the check is worth repeating once the containers are open.
		readTrailer(bytes, document);
		if (document.encrypted)
		{
			result.error = "the PDF is encrypted (its trailer carries /Encrypt), so its text "
				"cannot be read without implementing the PDF security handler";
			return result;
		}
		if (document.objects.empty())
		{
			result.error = "no PDF objects could be read from the file; it is damaged or "
				"truncated";
			return result;
		}

		const std::vector<PageEntry> pages = findPages(document);
		if (pages.empty())
		{
			result.error = "the PDF carries no page objects this reader could find";
			return result;
		}

		result.ok = true;
		result.pageCount = static_cast<int>(pages.size());

		const size_t limit = maxPages > 0
			? std::min(pages.size(), static_cast<size_t>(maxPages))
			: pages.size();
		bool sawAnyTextOperator = false;
		int totalCharacters = 0;
		for (size_t i = 0; i < limit; ++i)
		{
			std::map<std::string, FontInfo> fonts;
			const std::string fontToken = dictGet(pages[i].resources, "/Font");
			if (!fontToken.empty())
			{
				const PdfDict fontDict = parseDict(document.resolve(fontToken));
				for (const std::pair<std::string, std::string>& entry : fontDict)
				{
					fonts[entry.first] = readFont(document, entry.second);
				}
			}

			TextState state;
			const std::string content = pageContent(document, pages[i].object);
			if (!content.empty())
			{
				extractContent(document, content, fonts, pages[i].resources, state, 0);
			}
			sawAnyTextOperator = sawAnyTextOperator || state.sawTextOperator;

			PdfTextPage page;
			page.number = static_cast<int>(i) + 1;
			page.text = tidy(state.out);
			totalCharacters += countNonBlank(page.text);
			result.pages.push_back(page);
		}

		// A page with image data and effectively no text is a picture of text: nothing will get
		// a specification out of it without OCR, and a caller told "scanned" can say so instead
		// of treating an empty string as an answer.
		const int budget = ScannedTextCharsPerPage * std::max(1, static_cast<int>(limit));
		result.looksScanned = document.hasImageData
			&& (totalCharacters < budget || !sawAnyTextOperator);
		return result;
	}

	std::vector<PdfTextMatch> PdfText::search(const PdfTextResult& extracted,
		const std::string& needle, int maxMatches, int contextChars)
	{
		std::vector<PdfTextMatch> matches;
		if (!extracted.ok || needle.empty() || maxMatches <= 0)
		{
			return matches;
		}
		const std::string wanted = foldedNeedle(needle);
		if (wanted.empty())
		{
			return matches;
		}
		const int context = std::max(40, contextChars);

		// Every hit, grouped by the page it is on and capped per page, before any are handed
		// back. **The grouping is the point.** Taking the first `maxMatches` in reading order
		// spends the whole quota on the introduction: measured on the user's own 48-page
		// PCA9745B datasheet, "supply voltage" occurs twice on page 1 and the specification is
		// on page 36, so a straight scan answers a question about the ratings with the summary
		// paragraph and never reaches the table.
		std::vector<std::vector<PdfTextMatch>> perPage;
		perPage.reserve(extracted.pages.size());
		for (const PdfTextPage& page : extracted.pages)
		{
			const std::string folded = searchFolded(page.text);
			std::vector<PdfTextMatch> hits;
			size_t at = 0;
			while (static_cast<int>(hits.size()) < maxMatches)
			{
				const size_t found = folded.find(wanted, at);
				if (found == std::string::npos)
				{
					break;
				}
				const size_t before = static_cast<size_t>(context) / 2;
				const size_t start = found > before ? found - before : 0;
				const size_t length = std::min(static_cast<size_t>(context),
					page.text.size() - start);
				PdfTextMatch match;
				match.page = page.number;
				// Newlines folded to spaces: a snippet is one passage quoted back to a model,
				// and the line breaks the original had are layout, not meaning.
				std::string snippet = page.text.substr(start, length);
				for (size_t i = 0; i < snippet.size(); ++i)
				{
					if (snippet[i] == '\n' || snippet[i] == '\r')
					{
						snippet[i] = ' ';
					}
				}
				match.snippet = tidy(snippet);
				hits.push_back(match);
				at = found + wanted.size();
			}
			if (!hits.empty())
			{
				perPage.push_back(hits);
			}
		}

		// One hit from each page that has one, in page order, then a second from each, and so
		// on. A term that appears once on ten pages comes back from ten pages; a term that
		// appears ten times on one page still fills the quota from it.
		for (size_t round = 0; static_cast<int>(matches.size()) < maxMatches; ++round)
		{
			bool tookAny = false;
			for (const std::vector<PdfTextMatch>& hits : perPage)
			{
				if (round >= hits.size())
				{
					continue;
				}
				matches.push_back(hits[round]);
				tookAny = true;
				if (static_cast<int>(matches.size()) >= maxMatches)
				{
					break;
				}
			}
			if (!tookAny)
			{
				break;
			}
		}
		return matches;
	}

}
