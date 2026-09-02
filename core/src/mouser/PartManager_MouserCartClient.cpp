#include "mouser/PartManager_MouserCartClient.h"

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

	const char* const MouserCartClient::ApiKeyEnvVar = "MOUSER_CART_API";

	namespace
	{
		// Same minimal escaping as MouserClient, and for the same reason: the request body is
		// built by hand so the non-Qt build still compiles this file. Only a part number ever
		// reaches it, and the spec constrains that to 80 characters of article number.
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

	bool MouserCartResult::hasRejectedLines() const
	{
		for (const MouserCartLine& line : lines)
		{
			if (!line.errorMessage.empty())
			{
				return true;
			}
		}
		return false;
	}

	std::string MouserCartClient::buildInsertBody(const std::string& cartKey,
		const std::vector<MouserCartItemRequest>& items)
	{
		std::string body = "{";
		if (!cartKey.empty())
		{
			// Omitted entirely rather than sent empty: the spec types CartKey as a uuid, and an
			// empty string is not one — sending it is how you get a validation error instead of
			// a new cart.
			body += "\"CartKey\":\"" + jsonEscape(cartKey) + "\",";
		}
		body += "\"CartItems\":[";
		bool first = true;
		for (const MouserCartItemRequest& item : items)
		{
			if (item.mouserPartNumber.empty() || item.quantity <= 0)
			{
				// A line without an article number cannot be ordered; sending it would fail the
				// whole request. OrderRepository::lines() flags these as not stageable so the UI
				// can say which ones, and they are skipped here as the last line of defence.
				continue;
			}
			if (!first)
			{
				body += ",";
			}
			first = false;
			body += "{\"MouserPartNumber\":\"" + jsonEscape(item.mouserPartNumber) + "\"";
			body += ",\"Quantity\":" + std::to_string(item.quantity) + "}";
		}
		body += "]}";
		return body;
	}

	std::string MouserCartClient::cartUrl(const std::string& cartKey)
	{
		PM_UNUSED(cartKey);
		// The Swagger spec publishes no browser URL that takes a CartKey — the key is an API
		// handle, not a share link. The cart it built is the signed-in account's cart, so the
		// plain cart page is where the user finds it. Deliberately not guessing a deep link:
		// a wrong URL that looks right is worse than an obvious one that works.
		return "https://www.mouser.com/cart";
	}

#if QT_ENABLED

	namespace
	{
		const char* const MouserApiRoot = "https://api.mouser.com/api/v1/";

		std::string readApiKey()
		{
			// qEnvironmentVariable rather than std::getenv: getenv is C4996 under MSVC and this
			// library promotes C4996 to an error.
			return qEnvironmentVariable(MouserCartClient::ApiKeyEnvVar).toStdString();
		}

		std::string str(const QJsonObject& obj, const char* key)
		{
			return obj.value(QLatin1String(key)).toString().toStdString();
		}

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
	}

	MouserCartResult MouserCartClient::parseCartResponse(const std::string& json)
	{
		MouserCartResult result;

		QJsonParseError parseError;
		const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &parseError);
		if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		{
			result.errorMessage = "Malformed Mouser cart response: " + parseError.errorString().toStdString();
			return result;
		}
		const QJsonObject root = doc.object();

		// Cart-level failures arrive inside a 200 body, so this comes before anything else.
		const std::string errors = joinErrors(root.value(QLatin1String("Errors")).toArray());
		if (!errors.empty())
		{
			result.errorMessage = errors;
			return result;
		}

		result.cartKey = str(root, "CartKey");
		result.currencyCode = str(root, "CurrencyCode");
		result.merchandiseTotal = root.value(QLatin1String("MerchandiseTotal")).toDouble();
		result.totalItemCount = root.value(QLatin1String("TotalItemCount")).toInt();

		for (const QJsonValue& value : root.value(QLatin1String("CartItems")).toArray())
		{
			const QJsonObject obj = value.toObject();
			MouserCartLine line;
			line.mouserPartNumber = str(obj, "MouserPartNumber");
			line.manufacturerPartNumber = str(obj, "MfrPartNumber");
			line.description = str(obj, "Description");
			line.quantity = obj.value(QLatin1String("Quantity")).toInt();
			line.unitPrice = obj.value(QLatin1String("UnitPrice")).toDouble();
			line.extendedPrice = obj.value(QLatin1String("ExtendedPrice")).toDouble();
			// Per-line rejection: the cart as a whole can succeed while one part in it did not.
			line.errorMessage = joinErrors(obj.value(QLatin1String("Errors")).toArray());
			result.lines.push_back(line);
		}
		result.ok = true;
		return result;
	}

	bool MouserCartClient::hasApiKey()
	{
		return !readApiKey().empty();
	}

	MouserCartClient::MouserCartClient()
		: m_network(new QNetworkAccessManager())
	{
	}

	MouserCartClient::~MouserCartClient()
	{
		delete m_network;
	}

	MouserCartResult MouserCartClient::send(const std::string& endpointPath, const std::string& jsonBody,
		bool isPost)
	{
		MouserCartResult result;

		const std::string apiKey = readApiKey();
		if (apiKey.empty())
		{
			result.errorMessage = std::string("Mouser Cart API key is not set. Set the ")
				+ ApiKeyEnvVar + " environment variable and restart the application.";
			return result;
		}

		QUrl url(QString::fromLatin1(MouserApiRoot) + QString::fromStdString(endpointPath));
		// Seeded from the URL rather than default-constructed: readCart() puts cartKey in the
		// path string, and a fresh QUrlQuery would replace it instead of adding to it.
		QUrlQuery query(url);
		query.addQueryItem(QStringLiteral("apiKey"), QString::fromStdString(apiKey));
		url.setQuery(query);

		QNetworkRequest request(url);
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

		// ponytail: same synchronous-with-timeout nested QEventLoop as MouserClient::post(), same
		// ceiling — it blocks the calling thread, so a UI caller runs it on a worker or accepts
		// the freeze. Kept identical on purpose rather than shared: factoring one HTTP helper out
		// of two 40-line call sites buys nothing and couples the two keys' code paths together.
		QNetworkReply* reply = isPost
			? m_network->post(request, QByteArray::fromStdString(jsonBody))
			: m_network->get(request);

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
			result.errorMessage = "Mouser cart request timed out after "
				+ std::to_string(m_timeoutMs) + " ms.";
			return result;
		}
		if (httpStatus != 0 && httpStatus != 200)
		{
			result.errorMessage = "Mouser cart returned HTTP " + std::to_string(httpStatus) + ".";
			return result;
		}
		if (networkError != QNetworkReply::NoError)
		{
			result.errorMessage = "Mouser cart request failed: " + networkErrorText.toStdString();
			return result;
		}
		return parseCartResponse(body.toStdString());
	}

#else

	// No Qt: no JSON parser and no network stack. The DTOs and buildInsertBody() above still
	// compile so the staging logic stays testable; every call that needs Qt fails cleanly.
	MouserCartResult MouserCartClient::parseCartResponse(const std::string& json)
	{
		PM_UNUSED(json);
		MouserCartResult result;
		result.errorMessage = "Mouser support requires the Qt build (QT_ENABLED).";
		return result;
	}

	bool MouserCartClient::hasApiKey()
	{
		return false;
	}

	MouserCartClient::MouserCartClient()
	{
	}

	MouserCartClient::~MouserCartClient()
	{
	}

	MouserCartResult MouserCartClient::send(const std::string& endpointPath, const std::string& jsonBody,
		bool isPost)
	{
		PM_UNUSED(endpointPath);
		PM_UNUSED(jsonBody);
		PM_UNUSED(isPost);
		MouserCartResult result;
		result.errorMessage = "Mouser support requires the Qt build (QT_ENABLED).";
		return result;
	}

#endif

	void MouserCartClient::setTimeoutMs(int timeoutMs)
	{
		m_timeoutMs = timeoutMs;
	}

	int MouserCartClient::timeoutMs() const
	{
		return m_timeoutMs;
	}

	MouserCartResult MouserCartClient::insertItems(const std::string& cartKey,
		const std::vector<MouserCartItemRequest>& items)
	{
		return send("cart/items/insert", buildInsertBody(cartKey, items), true);
	}

	MouserCartResult MouserCartClient::updateItems(const std::string& cartKey,
		const std::vector<MouserCartItemRequest>& items)
	{
		// Same body shape as insert; only the endpoint differs, and with it the semantics —
		// update sets the quantity, insert adds to it.
		return send("cart/items/update", buildInsertBody(cartKey, items), true);
	}

	MouserCartResult MouserCartClient::readCart(const std::string& cartKey)
	{
		// GET /cart takes the key as a query parameter alongside apiKey.
		return send("cart?cartKey=" + cartKey, std::string(), false);
	}

}
