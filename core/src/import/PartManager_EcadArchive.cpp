#include "import/PartManager_EcadArchive.h"

#include <algorithm>
#include <cctype>

#if QT_ENABLED
	#include <QString>
	#include <QVector>
	#include <private/qzipreader_p.h>
#endif

namespace PartManager
{
	namespace
	{
		std::string toLower(const std::string& text)
		{
			std::string out = text;
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		bool endsWith(const std::string& text, const std::string& suffix)
		{
			return text.size() >= suffix.size()
				&& text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		// Archives use '/', but a hand-made one on Windows may carry '\'.
		std::vector<std::string> segmentsOf(const std::string& path)
		{
			std::vector<std::string> segments;
			std::string current;
			for (char c : path)
			{
				if (c == '/' || c == '\\')
				{
					if (!current.empty())
					{
						segments.push_back(current);
					}
					current.clear();
					continue;
				}
				current += c;
			}
			if (!current.empty())
			{
				segments.push_back(current);
			}
			return segments;
		}

		std::string baseNameOf(const std::string& path)
		{
			const std::vector<std::string> segments = segmentsOf(path);
			return segments.empty() ? std::string() : segments.back();
		}

		// A directory entry, or one of the ZIP conventions that is not a file we could attach.
		bool isSkippableEntry(const std::string& path)
		{
			return path.empty() || endsWith(path, "/") || endsWith(path, "\\")
				|| toLower(path).find("__macosx/") != std::string::npos;
		}

		// Better = under a KiCad folder first, then fewer path segments, then a shorter name.
		// Deterministic all the way down, so the same archive always yields the same choice.
		bool isBetterCandidate(const std::string& candidate, const std::string& incumbent)
		{
			if (incumbent.empty())
			{
				return true;
			}
			const bool candidateKicad = EcadArchive::isUnderKicadFolder(candidate);
			const bool incumbentKicad = EcadArchive::isUnderKicadFolder(incumbent);
			if (candidateKicad != incumbentKicad)
			{
				return candidateKicad;
			}
			const size_t candidateDepth = segmentsOf(candidate).size();
			const size_t incumbentDepth = segmentsOf(incumbent).size();
			if (candidateDepth != incumbentDepth)
			{
				return candidateDepth < incumbentDepth;
			}
			return candidate.size() < incumbent.size();
		}
	}

	bool EcadArchive::isUnderKicadFolder(const std::string& path)
	{
		const std::vector<std::string> segments = segmentsOf(path);
		// The last segment is the file itself, so a file merely *called* "kicad" does not count.
		for (size_t i = 0; i + 1 < segments.size(); ++i)
		{
			if (toLower(segments[i]) == "kicad")
			{
				return true;
			}
		}
		return false;
	}

	EcadArchiveContents EcadArchive::classify(const std::vector<std::string>& entryPaths)
	{
		EcadArchiveContents contents;
		bool sawLegacyKicad = false;

		for (const std::string& path : entryPaths)
		{
			if (isSkippableEntry(path))
			{
				continue;
			}
			const std::string lower = toLower(path);

			// The modern extensions are unambiguous — nothing but KiCad writes them — so they are
			// taken wherever they sit in the archive.
			if (endsWith(lower, ".kicad_sym"))
			{
				if (isBetterCandidate(path, contents.symbolEntry)) { contents.symbolEntry = path; }
				continue;
			}
			if (endsWith(lower, ".kicad_mod"))
			{
				if (isBetterCandidate(path, contents.footprintEntry)) { contents.footprintEntry = path; }
				continue;
			}
			if (endsWith(lower, ".step") || endsWith(lower, ".stp") || endsWith(lower, ".wrl"))
			{
				// The model is the one file that is normally NOT under KiCad/ — it sits in a 3D/
				// folder shared by every tool. isBetterCandidate() still prefers a KiCad one when
				// the archive happens to ship both.
				if (isBetterCandidate(path, contents.modelEntry)) { contents.modelEntry = path; }
				continue;
			}

			// `.lib` and `.mod` are KiCad 5, and are also CADSTAR's and other tools' extensions —
			// `CADSTAR/....lib` is in the same archive. Only inside a KiCad folder do they mean
			// KiCad, and even then they are reported rather than imported.
			if ((endsWith(lower, ".lib") || endsWith(lower, ".dcm") || endsWith(lower, ".mod"))
				&& isUnderKicadFolder(path))
			{
				sawLegacyKicad = true;
				continue;
			}

			++contents.ignoredEntries;
		}

		// Only worth saying when the archive really had nothing modern to offer.
		contents.legacyKicadOnly = sawLegacyKicad
			&& contents.symbolEntry.empty() && contents.footprintEntry.empty();
		contents.ok = true;
		return contents;
	}

#if QT_ENABLED

	EcadArchivePayload EcadArchive::read(const std::string& zipPath)
	{
		EcadArchivePayload payload;

		QZipReader reader(QString::fromStdString(zipPath));
		if (!reader.isReadable() || reader.status() != QZipReader::NoError)
		{
			payload.contents.errorMessage = "Cannot read the archive: " + zipPath;
			return payload;
		}

		std::vector<std::string> entryPaths;
		const QVector<QZipReader::FileInfo> infos = reader.fileInfoList();
		for (const QZipReader::FileInfo& info : infos)
		{
			if (!info.isFile)
			{
				continue;
			}
			entryPaths.push_back(info.filePath.toStdString());
		}
		if (entryPaths.empty())
		{
			payload.contents.errorMessage = "The archive is empty.";
			return payload;
		}

		payload.contents = classify(entryPaths);

		// One read per wanted entry; the archive stays open across all three.
		auto pull = [&reader](const std::string& entry, std::string& outBytes, std::string& outName)
		{
			if (entry.empty())
			{
				return;
			}
			const QByteArray data = reader.fileData(QString::fromStdString(entry));
			if (data.isEmpty())
			{
				return;   // an entry that will not extract is treated as absent, not as a failure
			}
			outBytes.assign(data.constData(), static_cast<size_t>(data.size()));
			outName = baseNameOf(entry);
		};
		pull(payload.contents.symbolEntry, payload.symbolBytes, payload.symbolName);
		pull(payload.contents.footprintEntry, payload.footprintBytes, payload.footprintName);
		pull(payload.contents.modelEntry, payload.modelBytes, payload.modelName);

		// An entry that was listed but would not extract must not still be advertised.
		if (payload.symbolBytes.empty())    { payload.contents.symbolEntry.clear(); }
		if (payload.footprintBytes.empty()) { payload.contents.footprintEntry.clear(); }
		if (payload.modelBytes.empty())     { payload.contents.modelEntry.clear(); }
		return payload;
	}

#endif

}
