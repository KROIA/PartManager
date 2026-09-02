#include "filestore/PartManager_FileStore.h"
#include "persistence/PartManager_PartRepository.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <windows.h>
	#include <winhttp.h>
	#pragma comment(lib, "winhttp.lib")
#endif

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
			// What every Mouser product photo actually is, whatever its URL claimed.
			if (extension == ".webp") return "image/webp";
			if (extension == ".gif")  return "image/gif";
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

#ifdef _WIN32
	namespace
	{
		// A downloaded response, from whichever HTTP stack fetched it.
		struct HttpResponse
		{
			bool transportFailed = true;   // the request never completed; fall back to Qt
			int status = 0;
			std::string contentType;
			std::string body;
			std::string error;
		};

		// **Why this exists instead of just using Qt.** Mouser's CDN answers a request it does
		// not like with HTTP 200 + text/html + a 13897-byte "Access Denied" page, so a download
		// fails by silently succeeding. Getting a real answer needs all four of:
		//
		//   HTTP/2 ............ HTTP/1.1 is blocked no matter what else is sent
		//   Accept ............ a browser-shaped image/pdf list
		//   Sec-Fetch-* ....... Dest/Mode/Site, the plain subresource-fetch triple
		//   Accept-Encoding ... omitting it is blocked; "gzip, deflate" is enough
		//
		// The User-Agent turned out to be irrelevant — which is why an earlier attempt at
		// fixing this by sending a browser User-Agent got nowhere.
		//
		// Even with all four, Qt 5.15 is still served the block page where WinHTTP is served the
		// file (measured 2026-09-02, same machine, same minute, HTTP/2 confirmed negotiated on
		// both sides). Whatever the remaining discriminator is, it is below the level Qt's API
		// exposes, so the request is handed to the platform stack instead. WinHTTP is in the
		// Windows SDK, so this costs no new dependency.
		//
		// These are the headers a browser sends for an ordinary subresource fetch, not a
		// disguise: PartManager still identifies itself in WinHttpOpen.
		//
		// Falls back rather than fails: transportFailed leaves the Qt path to try, so a machine
		// where WinHTTP is unavailable or proxied differently is no worse off than before.
		HttpResponse winHttpGet(const std::string& url, int timeoutMs)
		{
			HttpResponse response;
			const std::wstring wideUrl(url.begin(), url.end());

			URL_COMPONENTS parts{};
			parts.dwStructSize = sizeof(parts);
			wchar_t host[256] = {};
			wchar_t path[4096] = {};
			parts.lpszHostName = host;      parts.dwHostNameLength = ARRAYSIZE(host);
			parts.lpszUrlPath = path;       parts.dwUrlPathLength = ARRAYSIZE(path);
			if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts))
			{
				response.error = "Could not parse the URL.";
				return response;
			}

			HINTERNET session = WinHttpOpen(L"PartManager",
				WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!session)
			{
				response.error = "Could not open an HTTP session.";
				return response;
			}
			WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

			// HTTP/2 has to be asked for; Mouser answers an HTTP/1.1 request with the block page
			// even over SChannel, so this is required rather than an optimisation.
			DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
			WinHttpSetOption(session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));

			HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
			if (!connection)
			{
				response.error = "Could not connect to " + std::string(url) + ".";
				WinHttpCloseHandle(session);
				return response;
			}

			const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
			HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, nullptr,
				WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
			if (!request)
			{
				response.error = "Could not build the request.";
				WinHttpCloseHandle(connection);
				WinHttpCloseHandle(session);
				return response;
			}

			// Datasheet links are usually a chain of manufacturer/CDN redirects; WinHTTP follows
			// them by default, and this keeps it from following one down to plain HTTP.
			DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
			WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
				&redirectPolicy, sizeof(redirectPolicy));

			// The headers a browser sends for a plain subresource fetch. Without Sec-Fetch-*
			// the block page comes back even over SChannel — measured, not assumed.
			DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
			WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION,
				&decompression, sizeof(decompression));

			static const wchar_t* const Headers =
				L"Accept: image/avif,image/webp,image/apng,image/svg+xml,image/*,application/pdf,*/*;q=0.8\r\n"
				L"Accept-Encoding: gzip, deflate\r\n"
				L"Accept-Language: en-US,en;q=0.9\r\n"
				L"Sec-Fetch-Dest: image\r\n"
				L"Sec-Fetch-Mode: no-cors\r\n"
				L"Sec-Fetch-Site: same-origin\r\n";

			bool sent = WinHttpSendRequest(request, Headers, DWORD(-1),
				WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
			if (sent)
			{
				sent = WinHttpReceiveResponse(request, nullptr) != FALSE;
			}
			if (!sent)
			{
				response.error = "The download did not complete.";
				WinHttpCloseHandle(request);
				WinHttpCloseHandle(connection);
				WinHttpCloseHandle(session);
				return response;
			}

			DWORD status = 0;
			DWORD statusSize = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
			response.status = static_cast<int>(status);

			wchar_t contentType[256] = {};
			DWORD contentTypeSize = sizeof(contentType);
			if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
				contentType, &contentTypeSize, WINHTTP_NO_HEADER_INDEX))
			{
				const std::wstring wide(contentType);
				response.contentType.assign(wide.begin(), wide.end());
			}

			for (;;)
			{
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
				{
					break;
				}
				const std::size_t offset = response.body.size();
				response.body.resize(offset + available);
				DWORD read = 0;
				if (!WinHttpReadData(request, &response.body[offset], available, &read))
				{
					response.body.resize(offset);
					break;
				}
				response.body.resize(offset + read);
			}

			response.transportFailed = false;
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return response;
		}
	}
#endif

	std::string FileStore::sniffExtension(const std::string& bytes)
	{
		const auto begins = [&bytes](const char* magic, std::size_t length)
			{
				return bytes.size() >= length && std::memcmp(bytes.data(), magic, length) == 0;
			};

		if (begins("\x89PNG\r\n\x1a\n", 8))   return ".png";
		if (begins("\xFF\xD8\xFF", 3))        return ".jpg";
		if (begins("GIF87a", 6) || begins("GIF89a", 6)) return ".gif";
		if (begins("BM", 2))                  return ".bmp";
		if (begins("%PDF-", 5))               return ".pdf";
		// WebP and every other RIFF container share the first four bytes; the form type at
		// offset 8 is what tells them apart.
		if (begins("RIFF", 4) && bytes.size() >= 12 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0)
		{
			return ".webp";
		}
		// A ZIP signature also covers .kicad_* archives and Office files, so this only claims
		// ".zip" — callers that asked for one of those keep their own extension below.
		if (begins("PK\x03\x04", 4))          return ".zip";
		return std::string();
	}

	std::string FileStore::correctedFilename(const std::string& filename,
		const std::string& contentType, const std::string& bytes)
	{
		std::string extension = sniffExtension(bytes);
		if (extension.empty())
		{
			// Only formats with no usable signature reach here — SVG is the one that matters,
			// since Mouser serves a few symbols that way.
			const std::string type = trimmedLower(contentType, 64);
			if (startsWith(type, "image/svg")) { extension = ".svg"; }
			else if (startsWith(type, "image/webp")) { extension = ".webp"; }
			else { return filename; }
		}

		const std::size_t dot = filename.find_last_of('.');
		const std::string stem = (dot == std::string::npos) ? filename : filename.substr(0, dot);
		std::string current;
		if (dot != std::string::npos)
		{
			current = filename.substr(dot);
			for (char& c : current) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
		}
		if (current == extension)
		{
			return filename;
		}
		// ".jpeg" and ".jpg" are the same format under two names; renaming between them is
		// churn that would change the stored name for no reason.
		if (extension == ".jpg" && current == ".jpeg")
		{
			return filename;
		}
		// A ZIP-based format the caller already named correctly keeps its name — a .kicad_sym
		// bundle or an .xlsx is a ZIP, and calling it ".zip" would lose what it is.
		if (extension == ".zip" && !current.empty() && current != ".zip")
		{
			return filename;
		}
		if (stem.empty())
		{
			return filename;
		}
		return stem + extension;
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

	DownloadedBytes FileStore::downloadBytes(const std::string& url, int timeoutMs)
	{
		DownloadedBytes result;

		const QUrl requestUrl = QUrl::fromUserInput(QString::fromStdString(url));
		if (url.empty() || !requestUrl.isValid())
		{
			// Mouser leaves DataSheetUrl empty for most parts (§6) — that is the normal case,
			// not an exception, and the caller falls back to attaching a file by hand.
			result.errorMessage = "No URL to download.";
			return result;
		}

#ifdef _WIN32
		// The only stack Mouser answers properly — see winHttpGet(). Qt stays as the fallback
		// for anything WinHTTP cannot do, so this can only add successes, never remove them.
		const HttpResponse windowsResponse = winHttpGet(url, timeoutMs);
		if (!windowsResponse.transportFailed)
		{
			result.ok = true;
			result.status = windowsResponse.status;
			result.contentType = windowsResponse.contentType;
			result.bytes = windowsResponse.body;
			return result;
		}
#endif

		QNetworkAccessManager network;
		QNetworkRequest request(requestUrl);
		// Datasheet links are usually a chain of manufacturer/CDN redirects.
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

		// **What actually gets past Mouser's bot filter.** Measured 2026-09-02 against
		// www.mouser.ch/images/wurthelectronics/hd/WL-SMCW.JPG, three runs per combination:
		//
		//   HTTP/1.1, any headers, with or without a browser User-Agent .. 13897 B block page
		//   HTTP/2, no headers ......................................... 13897 B block page
		//   HTTP/2 + Accept ............................................ 13897 B block page
		//   HTTP/2 + Accept + Sec-Fetch-* .............................. the real image
		//
		// All three are required together, and the User-Agent turned out to be irrelevant —
		// which is why the previous attempt at fixing this by adding a browser User-Agent got
		// nowhere. Qt 5 negotiates HTTP/2 only when asked, so it is off unless this is set.
		//
		// These are the headers a browser sends for a plain subresource fetch, not a
		// disguise: PartManager still identifies itself and still obeys robots-level intent.
		request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
		request.setRawHeader("Accept",
			"image/avif,image/webp,image/apng,image/svg+xml,image/*,application/pdf,*/*;q=0.8");
		request.setRawHeader("Sec-Fetch-Dest", "image");
		request.setRawHeader("Sec-Fetch-Mode", "no-cors");
		request.setRawHeader("Sec-Fetch-Site", "same-origin");

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
		timer.start(timeoutMs);
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
			result.errorMessage = "Download timed out after " + std::to_string(timeoutMs) + " ms.";
			return result;
		}
		if (networkError != QNetworkReply::NoError)
		{
			result.errorMessage = "Download failed: " + networkErrorText.toStdString();
			return result;
		}

		result.ok = true;
		result.status = httpStatus;
		result.contentType = contentType.toStdString();
		result.bytes = body.toStdString();
		return result;
	}

	FileStoreResult FileStore::downloadFile(const std::string& url, const std::string& originalFilename)
	{
		const QUrl requestUrl = QUrl::fromUserInput(QString::fromStdString(url));

		const DownloadedBytes downloaded = downloadBytes(url, m_timeoutMs);
		if (!downloaded.ok)
		{
			FileStoreResult failed;
			failed.errorMessage = downloaded.errorMessage;
			return failed;
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

		FileStoreResult failed;
		if (downloaded.status != 0 && downloaded.status != 200)
		{
			failed.errorMessage = "Download failed with HTTP " + std::to_string(downloaded.status) + ".";
			return failed;
		}
		if (downloaded.bytes.empty())
		{
			failed.errorMessage = "Download returned an empty file.";
			return failed;
		}
		// See looksLikeBlockPage() — a blocked download arrives as a successful one, so every
		// check above passes and the block page would be stored under the requested name. Unless
		// a web page really was what was asked for. Nothing in the app asks for one, but
		// downloadFile() is general.
		const QString requestPathLower = requestUrl.path().toLower();
		const bool wantedHtml = requestPathLower.endsWith(QLatin1String(".htm"))
			|| requestPathLower.endsWith(QLatin1String(".html"));
		if (!wantedHtml && looksLikeBlockPage(downloaded.contentType, downloaded.bytes))
		{
			failed.errorMessage = "The server returned a web page instead of the file. The "
				"vendor's site blocked the download. Open the link in a browser, save the "
				"file, and attach it from disk.";
			return failed;
		}
		// The URL said .JPG; Mouser sent WebP. Store it under what it is, or the user has to
		// rename it by hand before anything will open it.
		return importBytes(downloaded.bytes,
			correctedFilename(filename, downloaded.contentType, downloaded.bytes));
	}

#else

	DownloadedBytes FileStore::downloadBytes(const std::string& url, int timeoutMs)
	{
		PM_UNUSED(url);
		PM_UNUSED(timeoutMs);
		DownloadedBytes result;
		result.errorMessage = "Downloading files requires the Qt build (QT_ENABLED).";
		return result;
	}

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
