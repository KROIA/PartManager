#include "filestore/PartManager_FileStore.h"
#include "persistence/PartManager_PartRepository.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>

#if QT_ENABLED
	#include <QByteArray>
	#include <QEventLoop>
	#include <QNetworkAccessManager>
	#include <QNetworkReply>
	#include <QNetworkRequest>
	#include <QString>
	#include <QTimer>
	#include <QUrl>
#endif

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	namespace
	{
		// Lowercased copy with leading whitespace dropped, for the two prefix checks below.
		std::string trimmedLower(const std::string& text, size_t limit)
		{
			size_t start = 0;
			while (start < text.size()
				&& (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
			{
				++start;
			}
			std::string result = text.substr(start, limit);
			std::transform(result.begin(), result.end(), result.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		bool startsWith(const std::string& text, const char* prefix)
		{
			return text.rfind(prefix, 0) == 0;
		}

		// Reads a whole file as bytes. Returns false if it cannot be opened/read.
		bool readWholeFile(const std::filesystem::path& path, std::string& outBytes)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				return false;
			}
			outBytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
			return !stream.bad();
		}

		bool writeWholeFile(const std::filesystem::path& path, const std::string& bytes)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				return false;
			}
			stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			return stream.good();
		}

		// Lowercased extension including the dot, e.g. ".pdf". Empty when there is none.
		std::string extensionOf(const std::string& filename)
		{
			std::string extension = std::filesystem::path(filename).extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return extension;
		}

		// ponytail: a five-entry extension table instead of a real MIME database — mime_type is
		// display/open-with metadata (§3), nothing branches on it. Extend the table if it ever does.
		std::string mimeTypeOf(const std::string& extension)
		{
			if (extension == ".pdf")  return "application/pdf";
			if (extension == ".png")  return "image/png";
			if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
			if (extension == ".svg")  return "image/svg+xml";
			if (extension == ".zip")  return "application/zip";
			return "application/octet-stream";
		}
	}

	FileStore::FileStore(const std::string& filestorePath)
		: m_rootPath(filestorePath)
	{
	}

	const std::string& FileStore::rootPath() const
	{
		return m_rootPath;
	}

	bool FileStore::looksLikeBlockPage(const std::string& contentType, const std::string& body)
	{
		// "text/html; charset=utf-8" is the usual shape, so a prefix test rather than equality.
		if (startsWith(trimmedLower(contentType, 64), "text/html"))
		{
			return true;
		}
		const std::string head = trimmedLower(body, 512);
		return startsWith(head, "<!doctype html") || startsWith(head, "<html");
	}

	std::string FileStore::hashBytes(const std::string& bytes)
	{
		std::uint64_t hash = 14695981039346656037ULL;   // FNV-1a 64 offset basis
		for (char c : bytes)
		{
			hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
			hash *= 1099511628211ULL;                   // FNV-1a 64 prime
		}
		char buffer[17] = { 0 };
		for (int i = 15; i >= 0; --i)
		{
			buffer[i] = "0123456789abcdef"[hash & 0xF];
			hash >>= 4;
		}
		return std::string(buffer, 16);
	}

	FileStoreResult FileStore::importBytes(const std::string& bytes, const std::string& originalFilename)
	{
		FileStoreResult result;
		result.originalFilename = std::filesystem::path(originalFilename).filename().string();
		result.contentHash = hashBytes(bytes);
		result.sizeBytes = static_cast<int>(bytes.size());
		const std::string extension = extensionOf(result.originalFilename);
		result.mimeType = mimeTypeOf(extension);

		const std::string subFolder = result.contentHash.substr(0, 2);
		std::filesystem::path folder = std::filesystem::path(m_rootPath) / subFolder;
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		if (error)
		{
			result.errorMessage = "Cannot create filestore folder " + folder.string() + ": " + error.message();
			return result;
		}

		// Same content -> same name -> stored once. A name that exists but holds *different*
		// content is a 64-bit hash collision; it gets its own `_<n>` name rather than silently
		// serving the wrong file to whoever opens it.
		for (int attempt = 0; attempt < 64; ++attempt)
		{
			std::string name = result.contentHash;
			if (attempt > 0)
			{
				name += "_" + std::to_string(attempt);
			}
			name += extension;

			const std::filesystem::path candidate = folder / name;
			if (std::filesystem::exists(candidate))
			{
				std::string existing;
				if (readWholeFile(candidate, existing) && existing != bytes)
				{
					continue;
				}
			}
			else if (!writeWholeFile(candidate, bytes))
			{
				result.errorMessage = "Cannot write " + candidate.string() + ".";
				return result;
			}
			result.relativePath = subFolder + "/" + name;
			result.ok = true;
			return result;
		}
		result.errorMessage = "Cannot store file: too many hash collisions for " + result.contentHash + ".";
		return result;
	}

	FileStoreResult FileStore::importFile(const std::string& sourcePath)
	{
		std::string bytes;
		if (!readWholeFile(sourcePath, bytes))
		{
			FileStoreResult result;
			result.errorMessage = "Cannot read file: " + sourcePath;
			return result;
		}
		return importBytes(bytes, sourcePath);
	}

	std::string FileStore::absolutePath(const std::string& relativePath) const
	{
		if (relativePath.empty())
		{
			return std::string();
		}
		const std::filesystem::path full = std::filesystem::path(m_rootPath) / relativePath;
		if (!std::filesystem::exists(full))
		{
			return std::string();
		}
		return full.string();
	}

	void FileStore::setTimeoutMs(int timeoutMs)
	{
		m_timeoutMs = timeoutMs;
	}

	int FileStore::timeoutMs() const
	{
		return m_timeoutMs;
	}

#if QT_ENABLED

	FileStoreResult FileStore::downloadFile(const std::string& url, const std::string& originalFilename)
	{
		FileStoreResult result;

		const QUrl requestUrl = QUrl::fromUserInput(QString::fromStdString(url));
		if (url.empty() || !requestUrl.isValid())
		{
			// Mouser leaves DataSheetUrl empty for most parts (§6) — that is the normal case,
			// not an exception, and the caller falls back to attaching a file by hand.
			result.errorMessage = "No datasheet URL to download.";
			return result;
		}

		std::string filename = originalFilename;
		if (filename.empty())
		{
			filename = requestUrl.fileName().toStdString();
		}
		if (filename.empty())
		{
			filename = "datasheet.pdf";
		}

		QNetworkAccessManager network;
		QNetworkRequest request(requestUrl);
		// Datasheet links are usually a chain of manufacturer/CDN redirects.
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
		QNetworkReply* reply = network.get(request);

		// ponytail: same synchronous-with-timeout nested QEventLoop as MouserClient::post() —
		// one user-initiated download whose result the caller needs before continuing, and it
		// keeps core/ free of QObject/moc. Ceiling: blocks the calling thread, so a UI caller
		// runs it on a worker thread. Upgrade both call sites together if async is ever needed.
		QEventLoop loop;
		QTimer timer;
		timer.setSingleShot(true);
		bool timedOut = false;
		QObject::connect(&timer, &QTimer::timeout, &loop, [&timedOut, reply, &loop]()
			{
				timedOut = true;
				reply->abort();
				loop.quit();
			});
		QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		timer.start(m_timeoutMs);
		loop.exec();
		timer.stop();

		const QByteArray body = reply->readAll();
		const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
		const QNetworkReply::NetworkError networkError = reply->error();
		const QString networkErrorText = reply->errorString();
		reply->deleteLater();

		if (timedOut)
		{
			result.errorMessage = "Download timed out after " + std::to_string(m_timeoutMs) + " ms.";
			return result;
		}
		if (httpStatus != 0 && httpStatus != 200)
		{
			result.errorMessage = "Download failed with HTTP " + std::to_string(httpStatus) + ".";
			return result;
		}
		if (networkError != QNetworkReply::NoError)
		{
			result.errorMessage = "Download failed: " + networkErrorText.toStdString();
			return result;
		}
		if (body.isEmpty())
		{
			result.errorMessage = "Download returned an empty file.";
			return result;
		}
		// See looksLikeBlockPage() — a blocked download arrives as a successful one, so every
		// check above passes and the block page would be stored under the requested name.
		// Unless a web page really was what was asked for. Nothing in the app asks for one, but
		// downloadFile() is general.
		const QString requestPath = requestUrl.path().toLower();
		const bool wantedHtml = requestPath.endsWith(QLatin1String(".htm"))
			|| requestPath.endsWith(QLatin1String(".html"));
		if (!wantedHtml && looksLikeBlockPage(contentType.toStdString(), body.toStdString()))
		{
			result.errorMessage = "The server returned a web page instead of a file. Vendor "
				"download sites often block automated downloads this way, answering 200 with a "
				"block page rather than an error. Open the link in a browser, save the file, and "
				"attach it from disk.";
			return result;
		}
		return importBytes(body.toStdString(), filename);
	}

#else

	FileStoreResult FileStore::downloadFile(const std::string& url, const std::string& originalFilename)
	{
		PM_UNUSED(url);
		PM_UNUSED(originalFilename);
		FileStoreResult result;
		result.errorMessage = "Downloading files requires the Qt build (QT_ENABLED).";
		return result;
	}

#endif

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	bool FileStore::roleFile(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
		PartFile& outFile)
	{
		if (partId == 0)
		{
			return false;
		}
		bool found = false;
		for (const PartFile& file : PartRepository::listFiles(db, partId))
		{
			if (partFileRoleFromString(file.role) == role && (!found || file.id > outFile.id))
			{
				outFile = file;
				found = true;
			}
		}
		return found;
	}

	int FileStore::adoptStoredFile(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
		const FileStoreResult& stored, std::string* outError)
	{
		if (!stored.ok)
		{
			if (outError)
			{
				*outError = stored.errorMessage;
			}
			return 0;
		}

		// Read *before* the insert. roleFile() resolves a slot by highest id, so asking
		// afterwards returns the row just written and the old one would never be detached —
		// which is how a slot quietly ends up holding two files.
		PartFile previous;
		const bool hadPrevious = roleFile(db, partId, role, previous);

		PartFile file;
		file.partId = partId;
		file.role = toString(role);
		file.relativePath = stored.relativePath;
		file.contentHash = stored.contentHash;
		file.sizeBytes = stored.sizeBytes;
		file.mimeType = stored.mimeType;
		file.originalFilename = stored.originalFilename;

		const int fileId = PartRepository::insertFile(db, file);
		if (fileId == 0)
		{
			if (outError)
			{
				*outError = "Could not insert the part_file row for " + stored.originalFilename + ".";
			}
			return 0;
		}
		if (hadPrevious && previous.id != fileId)
		{
			detachFile(db, previous.id);
		}
		return fileId;
	}

	int FileStore::replaceRoleFile(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
		const std::string& sourcePath, std::string* outError)
	{
		// Import first, replace second — a failed import leaves the old file in place rather
		// than losing both.
		return adoptStoredFile(db, partId, role, importFile(sourcePath), outError);
	}

	int FileStore::replaceRoleFileBytes(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
		const std::string& bytes, const std::string& originalFilename, std::string* outError)
	{
		return adoptStoredFile(db, partId, role, importBytes(bytes, originalFilename), outError);
	}

	int FileStore::attachFile(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
		const std::string& sourcePath, std::string* outError)
	{
		const FileStoreResult stored = importFile(sourcePath);
		if (!stored.ok)
		{
			if (outError)
			{
				*outError = stored.errorMessage;
			}
			return 0;
		}

		PartFile file;
		file.partId = partId;
		file.role = toString(role);
		file.relativePath = stored.relativePath;
		file.contentHash = stored.contentHash;
		file.sizeBytes = stored.sizeBytes;
		file.mimeType = stored.mimeType;
		file.originalFilename = stored.originalFilename;

		const int fileId = PartRepository::insertFile(db, file);
		if (fileId == 0 && outError)
		{
			// The stored file stays put — it is content-addressed, so the next attach of the
			// same content reuses it instead of leaving a second copy behind.
			*outError = "Could not insert the part_file row for " + stored.originalFilename + ".";
		}
		return fileId;
	}

	bool FileStore::detachFile(SQLiteWrapper::SQLite& db, int fileId)
	{
		PartFile file;
		if (!PartRepository::findFile(db, fileId, file))
		{
			return false;
		}
		if (!PartRepository::deleteFile(db, fileId))
		{
			return false;
		}
		// Dedup means the same stored file can back several rows; it only goes away with the last one.
		if (PartRepository::countFilesWithPath(db, file.relativePath) == 0)
		{
			std::error_code error;
			std::filesystem::remove(std::filesystem::path(m_rootPath) / file.relativePath, error);
		}
		return true;
	}

#endif

}
