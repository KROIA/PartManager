#include "easyeda/PartManager_EasyEdaClient.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cctype>

#if QT_ENABLED
	#include <QByteArray>
	#include <QEventLoop>
	#include <QJsonArray>
	#include <QJsonDocument>
	#include <QJsonObject>
	#include <QJsonParseError>
	#include <QJsonValue>
	#include <QNetworkAccessManager>
	#include <QNetworkReply>
	#include <QNetworkRequest>
	#include <QString>
	#include <QTimer>
	#include <QUrl>
#endif

namespace PartManager
{

	const char* const EasyEdaClient::UserAgent = "PartManager/1.0";

	namespace
	{
		// Everything that is not a letter or a digit, lowercased. Mouser writes "LT1506CR-3.3PBF"
		// where LCSC writes "LT1506CR3.3PBF"; comparing the raw strings would call those different
		// parts. The dot goes too — it is a separator in exactly the same way the dash is.
		std::string partKey(const std::string& text)
		{
			std::string key;
			key.reserve(text.size());
			for (unsigned char c : text)
			{
				if (std::isalnum(c))
				{
					key += static_cast<char>(std::tolower(c));
				}
			}
			return key;
		}

		std::string lowered(const std::string& text)
		{
			std::string out = text;
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}
	}

	bool EasyEdaClient::samePartNumber(const std::string& left, const std::string& right)
	{
		const std::string a = partKey(left);
		return !a.empty() && a == partKey(right);
	}

	std::string EasyEdaClient::bestMatch(const EasyEdaSearchResult& result, const std::string& mpn,
		const std::string& manufacturer)
	{
		if (!result.ok || mpn.empty())
		{
			return std::string();
		}

		const std::string wantedMaker = lowered(manufacturer);
		std::string firstExact;
		for (const EasyEdaSearchHit& hit : result.hits)
		{
			if (!samePartNumber(hit.mpn, mpn))
			{
				// Deliberately no prefix or substring fallback. A search for "LM358DR" also
				// returns "LM358DRG" and "MSLM358DR", which are different parts with different
				// packages — accepting one of those would attach a wrong land pattern.
				continue;
			}
			if (firstExact.empty())
			{
				firstExact = hit.lcscCode;
			}
			// Several manufacturers second-source the same number. Prefer the one Mouser named.
			const std::string maker = lowered(hit.manufacturer);
			if (!wantedMaker.empty() && !maker.empty()
				&& (wantedMaker.find(maker) != std::string::npos
					|| maker.find(wantedMaker) != std::string::npos))
			{
				return hit.lcscCode;
			}
		}
		return firstExact;
	}

#if QT_ENABLED

	namespace
	{
		const char* const SearchUrl =
			"https://easyeda.com/api/eda/product/search?version=6.5.36&needAggs=false&keyword=";
		const char* const ComponentUrlPrefix = "https://easyeda.com/api/products/";
		const char* const ComponentUrlSuffix = "/components?version=6.4.19.5";

		std::string stringField(const QJsonObject& object, const char* key)
		{
			return object.value(QLatin1String(key)).toString().toStdString();
		}

		// EasyEDA's shape arrays are arrays of strings; anything else in there is not a shape.
		std::vector<std::string> shapeLines(const QJsonObject& dataStr)
		{
			std::vector<std::string> lines;
			const QJsonArray shapes = dataStr.value(QStringLiteral("shape")).toArray();
			lines.reserve(static_cast<size_t>(shapes.size()));
			for (const QJsonValue& value : shapes)
			{
				if (value.isString())
				{
					lines.push_back(value.toString().toStdString());
				}
			}
			return lines;
		}
	}

	EasyEdaSearchResult EasyEdaClient::parseSearchResponse(const std::string& json)
	{
		EasyEdaSearchResult result;

		QJsonParseError error{};
		const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
		if (error.error != QJsonParseError::NoError || !document.isObject())
		{
			result.errorMessage = "EasyEDA sent a response that is not JSON.";
			return result;
		}

		const QJsonObject root = document.object();
		// The API reports its own failures inside a 200 through `code`, exactly as Mouser does
		// through Errors[] — so an HTTP 200 alone never means success here either.
		const int code = root.value(QStringLiteral("code")).toInt(200);
		if (code != 200)
		{
			const QString message = root.value(QStringLiteral("msg")).toString();
			result.errorMessage = message.isEmpty()
				? "EasyEDA search failed with code " + std::to_string(code) + "."
				: message.toStdString();
			return result;
		}

		const QJsonObject payload = root.value(QStringLiteral("result")).toObject();
		result.total = payload.value(QStringLiteral("total")).toInt();
		const QJsonArray products = payload.value(QStringLiteral("productList")).toArray();
		for (const QJsonValue& value : products)
		{
			const QJsonObject product = value.toObject();
			EasyEdaSearchHit hit;
			// `number` is the LCSC code; `productCode` is *not* present on this endpoint.
			hit.lcscCode = stringField(product, "number");
			hit.mpn = stringField(product, "mpn");
			hit.manufacturer = stringField(product, "manufacturer");
			hit.package = stringField(product, "package");
			if (!hit.lcscCode.empty())
			{
				result.hits.push_back(hit);
			}
		}

		// Zero hits is a normal answer — LCSC simply does not carry every part.
		result.ok = true;
		return result;
	}

	EasyEdaComponent EasyEdaClient::parseComponentResponse(const std::string& json)
	{
		EasyEdaComponent component;

		QJsonParseError error{};
		const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
		if (error.error != QJsonParseError::NoError || !document.isObject())
		{
			component.errorMessage = "EasyEDA sent a response that is not JSON.";
			return component;
		}

		const QJsonObject root = document.object();
		if (!root.value(QStringLiteral("success")).toBool(true))
		{
			const QString message = root.value(QStringLiteral("message")).toString();
			component.errorMessage = message.isEmpty()
				? "EasyEDA has no component data for that part."
				: message.toStdString();
			return component;
		}

		const QJsonObject payload = root.value(QStringLiteral("result")).toObject();
		if (payload.isEmpty())
		{
			component.errorMessage = "EasyEDA has no component data for that part.";
			return component;
		}
		component.title = stringField(payload, "title");
		component.lcscCode = stringField(payload, "lcsc");

		const QJsonObject symbolData = payload.value(QStringLiteral("dataStr")).toObject();
		const QJsonObject symbolHead = symbolData.value(QStringLiteral("head")).toObject();
		const QJsonObject symbolPara = symbolHead.value(QStringLiteral("c_para")).toObject();
		component.manufacturer = stringField(symbolPara, "Manufacturer");
		component.mpn = stringField(symbolPara, "Manufacturer Part");
		component.packageName = stringField(symbolPara, "package");
		component.referencePrefix = stringField(symbolPara, "pre");
		// EasyEDA writes the prefix with its placeholder still on it ("U?"); KiCad wants "U".
		while (!component.referencePrefix.empty() && component.referencePrefix.back() == '?')
		{
			component.referencePrefix.pop_back();
		}
		if (component.lcscCode.empty())
		{
			component.lcscCode = stringField(symbolPara, "Supplier Part");
		}
		component.symbolShapes = shapeLines(symbolData);
		component.symbolOriginX = symbolHead.value(QStringLiteral("x")).toDouble();
		component.symbolOriginY = symbolHead.value(QStringLiteral("y")).toDouble();

		const QJsonObject packageDetail = payload.value(QStringLiteral("packageDetail")).toObject();
		const QJsonObject footprintData = packageDetail.value(QStringLiteral("dataStr")).toObject();
		const QJsonObject footprintHead = footprintData.value(QStringLiteral("head")).toObject();
		component.footprintShapes = shapeLines(footprintData);
		component.footprintOriginX = footprintHead.value(QStringLiteral("x")).toDouble();
		component.footprintOriginY = footprintHead.value(QStringLiteral("y")).toDouble();
		if (component.packageName.empty())
		{
			component.packageName = stringField(packageDetail, "title");
		}

		if (!component.hasSymbol() && !component.hasFootprint())
		{
			component.errorMessage = "EasyEDA returned no symbol and no footprint for that part.";
			return component;
		}

		component.ok = true;
		return component;
	}

	EasyEdaClient::EasyEdaClient()
		: m_network(new QNetworkAccessManager())
	{
	}

	EasyEdaClient::~EasyEdaClient()
	{
		delete m_network;
	}

	std::string EasyEdaClient::get(const std::string& url, bool& outOk, std::string& outError)
	{
		outOk = false;

		QNetworkRequest request{ QUrl(QString::fromStdString(url)) };
		// The header this whole client depends on — see the class note. Qt's default
		// `Mozilla/5.0` is the one value CloudFront rejects.
		request.setHeader(QNetworkRequest::UserAgentHeader, QLatin1String(UserAgent));
		request.setRawHeader("Accept", "application/json, text/plain, */*");

		QNetworkReply* reply = m_network->get(request);

		// ponytail: the same synchronous-with-timeout nested QEventLoop as MouserClient::post(),
		// for the same reason — one user-initiated lookup whose answer the caller needs before it
		// can continue, and it keeps core/ free of QObject/moc. Ceiling: blocks the calling
		// thread, so a UI caller runs it on a worker thread.
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
		const QNetworkReply::NetworkError networkError = reply->error();
		const QString networkErrorText = reply->errorString();
		reply->deleteLater();

		if (timedOut)
		{
			outError = "EasyEDA request timed out after " + std::to_string(m_timeoutMs) + " ms.";
			return std::string();
		}
		if (httpStatus == 403)
		{
			// Worth its own message: a 403 here is CloudFront, not the API, and the User-Agent is
			// the only thing that has ever caused it.
			outError = "EasyEDA refused the request (HTTP 403).";
			return std::string();
		}
		if (httpStatus != 0 && httpStatus != 200)
		{
			outError = "EasyEDA returned HTTP " + std::to_string(httpStatus) + ".";
			return std::string();
		}
		if (networkError != QNetworkReply::NoError)
		{
			outError = "EasyEDA request failed: " + networkErrorText.toStdString();
			return std::string();
		}

		outOk = true;
		return body.toStdString();
	}

	EasyEdaSearchResult EasyEdaClient::search(const std::string& keyword)
	{
		EasyEdaSearchResult result;
		if (keyword.empty())
		{
			result.errorMessage = "No part number to look up.";
			return result;
		}

		bool ok = false;
		const std::string url = SearchUrl
			+ QUrl::toPercentEncoding(QString::fromStdString(keyword)).toStdString();
		const std::string body = get(url, ok, result.errorMessage);
		if (!ok)
		{
			return result;
		}
		return parseSearchResponse(body);
	}

	EasyEdaComponent EasyEdaClient::fetch(const std::string& lcscCode)
	{
		EasyEdaComponent component;
		if (lcscCode.empty())
		{
			component.errorMessage = "No LCSC part code to fetch.";
			return component;
		}

		bool ok = false;
		const std::string url = ComponentUrlPrefix
			+ QUrl::toPercentEncoding(QString::fromStdString(lcscCode)).toStdString()
			+ ComponentUrlSuffix;
		const std::string body = get(url, ok, component.errorMessage);
		if (!ok)
		{
			return component;
		}
		return parseComponentResponse(body);
	}

#else

	// No Qt: no JSON parser and no network stack. The DTOs and the matching rules above still
	// compile, so bestMatch()/samePartNumber() stay testable; everything that needs Qt fails
	// cleanly rather than silently returning empty data.
	EasyEdaSearchResult EasyEdaClient::parseSearchResponse(const std::string& json)
	{
		PM_UNUSED(json);
		EasyEdaSearchResult result;
		result.errorMessage = "EasyEDA support requires the Qt build (QT_ENABLED).";
		return result;
	}

	EasyEdaComponent EasyEdaClient::parseComponentResponse(const std::string& json)
	{
		PM_UNUSED(json);
		EasyEdaComponent component;
		component.errorMessage = "EasyEDA support requires the Qt build (QT_ENABLED).";
		return component;
	}

	EasyEdaClient::EasyEdaClient() = default;
	EasyEdaClient::~EasyEdaClient() = default;

	std::string EasyEdaClient::get(const std::string& url, bool& outOk, std::string& outError)
	{
		PM_UNUSED(url);
		outOk = false;
		outError = "EasyEDA support requires the Qt build (QT_ENABLED).";
		return std::string();
	}

	EasyEdaSearchResult EasyEdaClient::search(const std::string& keyword)
	{
		PM_UNUSED(keyword);
		return parseSearchResponse(std::string());
	}

	EasyEdaComponent EasyEdaClient::fetch(const std::string& lcscCode)
	{
		PM_UNUSED(lcscCode);
		return parseComponentResponse(std::string());
	}

#endif

	void EasyEdaClient::setTimeoutMs(int timeoutMs)
	{
		m_timeoutMs = timeoutMs;
	}

	int EasyEdaClient::timeoutMs() const
	{
		return m_timeoutMs;
	}

	EasyEdaComponent EasyEdaClient::lookup(const std::string& mpn, const std::string& manufacturer)
	{
		EasyEdaComponent component;

		const EasyEdaSearchResult found = search(mpn);
		if (!found.ok)
		{
			component.errorMessage = found.errorMessage;
			return component;
		}

		const std::string code = bestMatch(found, mpn, manufacturer);
		if (code.empty())
		{
			// The expected outcome for most Würth/TE/Molex parts. Phrased as a fact about
			// coverage rather than as a failure, because the caller offers a vendor ZIP next.
			component.errorMessage = "EasyEDA has no exact match for " + mpn + ".";
			return component;
		}
		return fetch(code);
	}

}
