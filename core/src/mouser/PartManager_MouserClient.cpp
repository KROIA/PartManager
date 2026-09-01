#include "mouser/PartManager_MouserClient.h"

#include <string>

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
	#include <QUrlQuery>
#endif

namespace PartManager
{

	const char* const MouserClient::ApiKeyEnvVar = "MOUSER_SEARCH_API";

	namespace
	{
		// Minimal JSON string escaping — enough for the four scalar fields we ever send
		// (a part number, a keyword, two integers). Control characters below 0x20 are
		// dropped rather than \u-escaped; they cannot occur in a valid part number.
		// ponytail: request bodies are built by hand instead of via QJsonDocument so the
		// non-Qt build still compiles this file. Swap to QJsonDocument if the request
		// shape ever grows past a flat envelope.
		std::string jsonEscape(const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 8);
			for (char c : text)
			{
				if (c == '"' || c == '\\')
				{
					out += '\\';
					out += c;
				}
				else if (static_cast<unsigned char>(c) >= 0x20)
				{
					out += c;
				}
			}
			return out;
		}
	}

#if QT_ENABLED

	namespace
	{
		// Swagger 2.0 spec: host api.mouser.com, scheme https, path /api/v{version}/...
		const char* const MouserApiRoot = "https://api.mouser.com/api/v1/";

		std::string readApiKey()
		{
			// qEnvironmentVariable rather than std::getenv: getenv is C4996 under MSVC and
			// this library promotes C4996 to an error.
			return qEnvironmentVariable(MouserClient::ApiKeyEnvVar).toStdString();
		}

		std::string str(const QJsonObject& obj, const char* key)
		{
			return obj.value(QLatin1String(key)).toString().toStdString();
		}

		// Joins Mouser's in-body Errors[] into one human message. Message is the useful field;
		// Code and Id are the fallbacks when Mouser sends a bare code.
		std::string joinErrors(const QJsonArray& errors)
		{
			std::string joined;
			for (const QJsonValue& value : errors)
			{
				const QJsonObject error = value.toObject();
				std::string one = str(error, "Message");
				if (one.empty())
				{
					one = str(error, "Code");
				}
				if (one.empty())
				{
					one = std::to_string(error.value(QLatin1String("Id")).toInt());
				}
				if (one.empty())
				{
					continue;
				}
				if (!joined.empty())
				{
					joined += "; ";
				}
				joined += one;
			}
			return joined;
		}

		MouserPartDto readPart(const QJsonObject& obj)
		{
			MouserPartDto part;
			part.mouserPartNumber = str(obj, "MouserPartNumber");
			part.manufacturerPartNumber = str(obj, "ManufacturerPartNumber");
			part.manufacturer = str(obj, "Manufacturer");
			part.description = str(obj, "Description");
			part.category = str(obj, "Category");
			part.dataSheetUrl = str(obj, "DataSheetUrl");
			part.productDetailUrl = str(obj, "ProductDetailUrl");
			part.imagePath = str(obj, "ImagePath");
			part.lifecycleStatus = str(obj, "LifecycleStatus");
			part.availability = str(obj, "Availability");
			part.availabilityInStock = str(obj, "AvailabilityInStock");
			part.rohsStatus = str(obj, "ROHSStatus");

			const QJsonArray attributes = obj.value(QLatin1String("ProductAttributes")).toArray();
			for (const QJsonValue& value : attributes)
			{
				const QJsonObject attributeObj = value.toObject();
				MouserProductAttribute attribute;
				attribute.name = str(attributeObj, "AttributeName");
				attribute.value = str(attributeObj, "AttributeValue");
				if (!attribute.name.empty())
				{
					part.productAttributes.push_back(attribute);
				}
			}

			const QJsonArray priceBreaks = obj.value(QLatin1String("PriceBreaks")).toArray();
			for (const QJsonValue& value : priceBreaks)
			{
				const QJsonObject breakObj = value.toObject();
				MouserPriceBreak priceBreak;
				priceBreak.quantity = breakObj.value(QLatin1String("Quantity")).toInt();
				priceBreak.price = str(breakObj, "Price");
				priceBreak.currency = str(breakObj, "Currency");
				part.priceBreaks.push_back(priceBreak);
			}
			return part;
		}
	}

	MouserSearchResult MouserClient::parseSearchResponse(const std::string& json)
	{
		MouserSearchResult result;

		QJsonParseError parseError;
		const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &parseError);
		if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		{
			result.errorMessage = "Malformed Mouser response: " + parseError.errorString().toStdString();
			return result;
		}
		const QJsonObject root = doc.object();

		// Mouser reports failures inside a 200 body, so this check comes before anything else.
		const std::string errors = joinErrors(root.value(QLatin1String("Errors")).toArray());
		if (!errors.empty())
		{
			result.errorMessage = errors;
			return result;
		}

		const QJsonObject searchResults = root.value(QLatin1String("SearchResults")).toObject();
		result.numberOfResults = searchResults.value(QLatin1String("NumberOfResult")).toInt();
		const QJsonArray parts = searchResults.value(QLatin1String("Parts")).toArray();
		for (const QJsonValue& value : parts)
		{
			result.parts.push_back(readPart(value.toObject()));
		}
		// Zero results is a valid answer, not a failure.
		result.ok = true;
		return result;
	}

	bool MouserClient::hasApiKey()
	{
		return !readApiKey().empty();
	}

	MouserClient::MouserClient()
		: m_network(new QNetworkAccessManager())
	{
	}

	MouserClient::~MouserClient()
	{
		delete m_network;
	}

	MouserSearchResult MouserClient::post(const std::string& endpointPath, const std::string& jsonBody)
	{
		MouserSearchResult result;

		const std::string apiKey = readApiKey();
		if (apiKey.empty())
		{
			// Deliberately no fallback and no placeholder: without a key there is no request.
			result.errorMessage = std::string("Mouser Search API key is not set. Set the ")
				+ ApiKeyEnvVar + " environment variable and restart the application.";
			return result;
		}

		QUrl url(QString::fromLatin1(MouserApiRoot) + QString::fromStdString(endpointPath));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("apiKey"), QString::fromStdString(apiKey));
		url.setQuery(query);

		QNetworkRequest request(url);
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

		QNetworkReply* reply = m_network->post(request, QByteArray::fromStdString(jsonBody));

		// ponytail: synchronous-with-timeout via a nested QEventLoop. A search is one
		// user-initiated request whose answer the caller needs before it can do anything
		// else, and this keeps core/ free of QObject/moc and the tests free of spinning
		// an event loop themselves. Ceiling: it blocks the calling thread and re-enters
		// the event loop, so a UI caller must run it on a worker thread (or accept the
		// freeze). Upgrade to a callback/QFuture API if more than one request is ever
		// in flight at a time.
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
			result.errorMessage = "Mouser request timed out after " + std::to_string(m_timeoutMs) + " ms.";
			return result;
		}
		if (httpStatus != 0 && httpStatus != 200)
		{
			result.errorMessage = "Mouser returned HTTP " + std::to_string(httpStatus) + ".";
			return result;
		}
		if (networkError != QNetworkReply::NoError)
		{
			result.errorMessage = "Mouser request failed: " + networkErrorText.toStdString();
			return result;
		}
		return parseSearchResponse(body.toStdString());
	}

#else

	// No Qt: no JSON parser and no network stack. The DTOs above still compile so the
	// mapping layer (MouserSearchService) stays available and testable; every call that
	// needs Qt fails cleanly instead of silently returning empty data.
	MouserSearchResult MouserClient::parseSearchResponse(const std::string& json)
	{
		PM_UNUSED(json);
		MouserSearchResult result;
		result.errorMessage = "Mouser support requires the Qt build (QT_ENABLED).";
		return result;
	}

	bool MouserClient::hasApiKey()
	{
		return false;
	}

	MouserClient::MouserClient()
	{
	}

	MouserClient::~MouserClient()
	{
	}

	MouserSearchResult MouserClient::post(const std::string& endpointPath, const std::string& jsonBody)
	{
		PM_UNUSED(endpointPath);
		PM_UNUSED(jsonBody);
		MouserSearchResult result;
		result.errorMessage = "Mouser support requires the Qt build (QT_ENABLED).";
		return result;
	}

#endif

	void MouserClient::setTimeoutMs(int timeoutMs)
	{
		m_timeoutMs = timeoutMs;
	}

	int MouserClient::timeoutMs() const
	{
		return m_timeoutMs;
	}

	MouserSearchResult MouserClient::searchByPartNumber(const std::string& partNumber, bool exactMatch)
	{
		// SearchByPartRequestRoot: one property whose name repeats the request type.
		std::string body = "{\"SearchByPartRequest\":{\"mouserPartNumber\":\"" + jsonEscape(partNumber) + "\"";
		body += ",\"partSearchOptions\":\"";
		body += exactMatch ? "Exact" : "None";
		body += "\"}}";
		return post("search/partnumber", body);
	}

	MouserSearchResult MouserClient::searchByKeyword(const std::string& keyword, int records, int startingRecord)
	{
		std::string body = "{\"SearchByKeywordRequest\":{\"keyword\":\"" + jsonEscape(keyword) + "\"";
		body += ",\"records\":" + std::to_string(records);
		body += ",\"startingRecord\":" + std::to_string(startingRecord);
		body += ",\"searchOptions\":\"None\"}}";
		return post("search/keyword", body);
	}

}
