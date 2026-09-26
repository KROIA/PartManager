#include "llm/PartManager_PartToolset.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "persistence/PartManager_PartRepository.h"
	#include "persistence/PartManager_PartTypeRepository.h"
	#include "persistence/PartManager_TagRepository.h"
	#include "search/PartManager_SearchEngine.h"
	#include "search/PartManager_SearchQuery.h"
	#include "units/PartManager_UnitTable.h"
	#include "units/PartManager_ValueParser.h"
	#include "SQLite.h"
#endif

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <algorithm>

namespace PartManager
{
	namespace
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

		// The two refusals every handler can answer with. A closed handle is the normal state
		// between databases (§1b) and read-only is a deliberate configuration, so both are
		// sentences the model can act on rather than conditions it has to infer from silence.
		const char* NoDatabaseMessage = "no database is open. Ask the user to open one first.";
		const char* ReadOnlyMessage = "this assistant may not change the database";

		// The group every tool in this set carries. UI-only metadata (QtLLM::Tool), never sent
		// to the model, so it is an identifier and not prose that would need translating.
		const char* ToolGroup = "Database";

		// How many entries an error message spells out before it says "and N more". Long enough
		// that a real library's category list fits, short enough that a mis-typed id on a
		// thousand-category database does not fill the model's whole context.
		constexpr int ErrorListLimit = 40;


		// ---- argument reading ------------------------------------------------------------
		// Every one of these is deliberately forgiving about *shape* and strict about *meaning*:
		// a local model quotes its integers about as often as not, and rejecting `"5"` costs a
		// whole turn to re-learn something the handler could have read. What is never guessed at
		// is a value — an id that does not exist and a number that does not parse are errors.

		// Trimmed text of `key`, or "" when the key is absent or holds something else.
		QString stringArg(const QJsonObject& args, const QString& key)
		{
			return args.value(key).toString().trimmed();
		}

		// True when `key` was actually supplied — what update_part's "only the keys present are
		// written" rule turns on. A JSON null counts as absent.
		bool hasArg(const QJsonObject& args, const QString& key)
		{
			return args.contains(key) && !args.value(key).isNull();
		}

		// A whole number from `key`, quoted or not. False when absent, fractional or not a number.
		bool intArg(const QJsonObject& args, const QString& key, int& outValue)
		{
			const QJsonValue value = args.value(key);
			if (value.isDouble())
			{
				const double raw = value.toDouble();
				const int truncated = static_cast<int>(raw);
				if (static_cast<double>(truncated) != raw)
				{
					return false;
				}
				outValue = truncated;
				return true;
			}
			if (value.isString())
			{
				bool ok = false;
				const int parsed = value.toString().trimmed().toInt(&ok);
				if (ok)
				{
					outValue = parsed;
				}
				return ok;
			}
			return false;
		}

		// A flag from `key`, accepting the spellings a model reaches for. `defaultValue` when absent.
		bool boolArg(const QJsonObject& args, const QString& key, bool defaultValue)
		{
			const QJsonValue value = args.value(key);
			if (value.isBool())
			{
				return value.toBool();
			}
			if (value.isDouble())
			{
				return value.toDouble() != 0.0;
			}
			if (value.isString())
			{
				const QString text = value.toString().trimmed().toLower();
				if (text == QStringLiteral("true") || text == QStringLiteral("1")
					|| text == QStringLiteral("yes"))
				{
					return true;
				}
				if (text == QStringLiteral("false") || text == QStringLiteral("0")
					|| text == QStringLiteral("no"))
				{
					return false;
				}
			}
			return defaultValue;
		}

		// A list of words from `key`: a JSON array, or one comma-separated string. Both shapes
		// arrive in practice and neither is ambiguous, so accepting both costs nothing.
		QStringList stringListArg(const QJsonObject& args, const QString& key)
		{
			QStringList values;
			const QJsonValue value = args.value(key);
			if (value.isArray())
			{
				const QJsonArray array = value.toArray();
				for (const QJsonValue& entry : array)
				{
					const QString text = entry.isString()
						? entry.toString().trimmed()
						: QString::number(entry.toDouble());
					if (!text.isEmpty())
					{
						values.append(text);
					}
				}
			}
			else if (value.isString())
			{
				const QStringList parts = value.toString().split(QLatin1Char(','));
				for (const QString& part : parts)
				{
					const QString text = part.trimmed();
					if (!text.isEmpty())
					{
						values.append(text);
					}
				}
			}
			return values;
		}


		// ---- shared formatting -----------------------------------------------------------

		// User data (a category name, a part name, a tag) straight into a JSON value. Never
		// translated — it is what the user typed, not the app's own chrome.
		QJsonValue jsonText(const std::string& text)
		{
			return QJsonValue(QString::fromStdString(text));
		}

		// Unicode-aware fold, the same one PartTypeTransfer uses to decide two category names are
		// the same name. std::tolower would fold `Resistor` and leave `Widerstände` alone.
		QString folded(const QString& text)
		{
			return text.trimmed().toCaseFolded();
		}

		QString folded(const std::string& text)
		{
			return folded(QString::fromStdString(text));
		}

		// "12=Resistor, 13=Capacitor, … (and 7 more)" — the half of an id error a small model reads.
		QString idNameSummary(const QStringList& entries)
		{
			if (entries.isEmpty())
			{
				return QStringLiteral("none");
			}
			QStringList shown = entries.mid(0, ErrorListLimit);
			QString text = shown.join(QStringLiteral(", "));
			if (entries.size() > shown.size())
			{
				text += QStringLiteral(" (and %1 more)").arg(entries.size() - shown.size());
			}
			return text;
		}

		QStringList categoryLabels(const std::vector<PartType>& types)
		{
			QStringList labels;
			for (const PartType& type : types)
			{
				labels.append(QStringLiteral("%1=%2").arg(type.id).arg(QString::fromStdString(type.name)));
			}
			return labels;
		}

		QJsonArray categoryChoices(const std::vector<PartType>& types)
		{
			QJsonArray array;
			for (const PartType& type : types)
			{
				QJsonObject entry;
				entry["id"] = type.id;
				entry["name"] = jsonText(type.name);
				entry["parentId"] = type.parentTypeId;
				array.append(entry);
			}
			return array;
		}

		// Rule 2 of PartManager_PartToolset.h: an id the model invented comes back as a correction
		// that names the ones that exist, in the message *and* as a list a caller can parse.
		QJsonObject unknownCategoryError(SQLiteWrapper::SQLite& db, int categoryId)
		{
			const std::vector<PartType> types = PartTypeRepository::listTypes(db);
			QJsonObject extra;
			extra["categories"] = categoryChoices(types);
			return llmError(QStringLiteral("no category with id %1. Existing categories (id=name): %2")
				.arg(categoryId)
				.arg(idNameSummary(categoryLabels(types))), extra);
		}

		// The same shape for a part. The id list is deliberately *not* spelled out: a library has
		// thousands of parts and the way to find one is search_parts, which the message names.
		QJsonObject unknownPartError(SQLiteWrapper::SQLite& db, int partId)
		{
			const size_t count = PartRepository::listParts(db).size();
			QJsonObject extra;
			extra["partCount"] = static_cast<int>(count);
			return llmError(QStringLiteral("no part with id %1. The database holds %2 parts; "
				"use search_parts to find the right id.")
				.arg(partId)
				.arg(static_cast<int>(count)), extra);
		}

		// "an integer id" errors read the same everywhere, so a model that mis-sends one parameter
		// learns the rule for all of them.
		QJsonObject missingIdError(const QString& name, const QString& hint)
		{
			return llmError(QStringLiteral("'%1' is required and must be an integer id. %2")
				.arg(name).arg(hint));
		}

		// §2b nearest-wins walk for the name template: a name is one string, so the nearest
		// declaration replaces an ancestor's instead of accumulating. Cycle-safe like every other
		// walk over parent_type_id.
		std::string nameTemplateFor(const std::vector<PartType>& types, int typeId)
		{
			std::vector<int> visited;
			int current = typeId;
			while (current != NoParentType
				&& std::find(visited.begin(), visited.end(), current) == visited.end())
			{
				visited.push_back(current);
				const PartType* found = nullptr;
				for (const PartType& type : types)
				{
					if (type.id == current)
					{
						found = &type;
					}
				}
				if (found == nullptr)
				{
					break;
				}
				if (!found->nameTemplate.empty())
				{
					return found->nameTemplate;
				}
				current = found->parentTypeId;
			}
			return std::string();
		}

		// The part's §2a attribute object. An unreadable column comes back empty rather than
		// throwing — a part written by an older build must still be readable.
		QJsonObject attributeObject(const Part& part)
		{
			QJsonParseError error;
			const QJsonDocument document =
				QJsonDocument::fromJson(QByteArray::fromStdString(part.attributes), &error);
			if (error.error != QJsonParseError::NoError || !document.isObject())
			{
				return QJsonObject();
			}
			return document.object();
		}

		std::string attributeJson(const QJsonObject& attributes)
		{
			return QString::fromUtf8(QJsonDocument(attributes).toJson(QJsonDocument::Compact))
				.toStdString();
		}

		// One attribute definition as the model sees it. enumOptions only when it means something —
		// an empty array on every text field is tokens spent saying nothing.
		QJsonObject attributeSchema(const PartTypeAttribute& attribute)
		{
			QJsonObject entry;
			entry["key"] = jsonText(attribute.key);
			entry["label"] = jsonText(attribute.label);
			entry["unit"] = jsonText(attribute.unit);
			entry["datatype"] = jsonText(toString(attribute.datatype));
			entry["required"] = attribute.required;
			entry["searchable"] = attribute.searchable;
			if (attribute.datatype == AttributeDataType::Enum)
			{
				QJsonArray options;
				for (const std::string& option : attribute.enumOptions)
				{
					options.append(jsonText(option));
				}
				entry["enumOptions"] = options;
			}
			return entry;
		}

		QStringList attributeKeys(const std::vector<PartTypeAttribute>& attributes)
		{
			QStringList keys;
			for (const PartTypeAttribute& attribute : attributes)
			{
				keys.append(QString::fromStdString(attribute.key));
			}
			return keys;
		}

		// The canonical §2a dropdown spelling of `unit`, or false when it is not one of them.
		// "ohm" does not fold to "Ω" — the point of the check is that it comes back as a
		// correction with the list, not as a column holding a unit nothing else recognises.
		bool canonicalUnit(const QString& typed, std::string& outUnit)
		{
			const std::string text = typed.toStdString();
			for (const std::string& unit : UnitTable::units())
			{
				if (UnitTable::unitsEqual(unit, text))
				{
					outUnit = unit;
					return true;
				}
			}
			return false;
		}

		QString unitChoices()
		{
			QStringList units;
			for (const std::string& unit : UnitTable::units())
			{
				units.append(unit.empty()
					? QStringLiteral("\"\" (no unit)")
					: QString::fromStdString(unit));
			}
			return units.join(QStringLiteral(", "));
		}

		// The §2a dropdown as an enum the schema can carry, so a unit outside it is caught by the
		// client-side validator as well as by the handler. "" is kept in the list on purpose: it is
		// a real entry of the dropdown ("(no unit)"), and leaving it out would turn a legitimate
		// "this field has no unit" into a rejection the model has to guess its way out of.
		QStringList unitEnumValues()
		{
			QStringList values;
			for (const std::string& unit : UnitTable::units())
			{
				values.append(QString::fromStdString(unit));
			}
			return values;
		}

		// The three values `part_type.domain` may hold. Named once because two tools spell them and
		// a fourth place spelling them differently is exactly how a fixed vocabulary stops being one.
		const QStringList& domainValues()
		{
			static const QStringList values{ QStringLiteral("electronic"),
				QStringLiteral("mechanical"), QStringLiteral("generic") };
			return values;
		}

		const QStringList& datatypeValues()
		{
			static const QStringList values{ QStringLiteral("number"), QStringLiteral("dimension"),
				QStringLiteral("text"), QStringLiteral("bool"), QStringLiteral("enum") };
			return values;
		}

		QJsonArray asJsonArray(const QStringList& values)
		{
			QJsonArray array;
			for (const QString& value : values)
			{
				array.append(value);
			}
			return array;
		}


		// ---- the tools -------------------------------------------------------------------

		LlmTool makeListCategories(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("list_categories")
				.setDescription("Lists every part category in the database with its id, its parent "
					"and how many parts it holds. Call this before create_part or create_category: "
					"every other tool takes a categoryId from here, never a category name.")
				.setGroup(ToolGroup);

			tool.handler = [context](const QJsonObject&) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				SQLiteWrapper::SQLite& db = context.database->connection();
				const std::vector<PartType> types = PartTypeRepository::listTypes(db);
				const std::vector<Part> parts = PartRepository::listParts(db);

				QJsonArray categories;
				for (const PartType& type : types)
				{
					int partCount = 0;
					for (const Part& part : parts)
					{
						if (part.partTypeId == type.id)
						{
							++partCount;
						}
					}
					QJsonObject entry;
					entry["id"] = type.id;
					entry["name"] = jsonText(type.name);
					entry["parentId"] = type.parentTypeId;
					entry["domain"] = jsonText(PartTypeRepository::effectiveDomain(db, type.id));
					entry["partCount"] = partCount;
					categories.append(entry);
				}

				QJsonObject payload;
				payload["categories"] = categories;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeGetCategory(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("get_category")
				.setDescription("The full definition of one category: the attributes a part filed "
					"under it carries and the files it expects. Attributes inherited from parent "
					"categories are included and marked with no distinction, because a part carries "
					"them exactly as if the category declared them itself.")
				.setGroup(ToolGroup)
				.addParameter("categoryId", "integer", "Category id from list_categories.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int categoryId = 0;
				if (!intArg(args, QStringLiteral("categoryId"), categoryId))
				{
					return missingIdError(QStringLiteral("categoryId"),
						QStringLiteral("Call list_categories to get one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				PartType type;
				if (!PartTypeRepository::findType(db, categoryId, type))
				{
					return unknownCategoryError(db, categoryId);
				}

				QJsonArray attributes;
				for (const PartTypeAttribute& attribute :
					PartTypeRepository::effectiveAttributes(db, categoryId))
				{
					attributes.append(attributeSchema(attribute));
				}

				QJsonArray fileSlots;
				for (const PartTypeFileSlot& slot : PartTypeRepository::effectiveFileSlots(db, categoryId))
				{
					QJsonObject entry;
					entry["role"] = jsonText(slot.role);
					entry["label"] = jsonText(slot.label);
					entry["required"] = slot.required;
					fileSlots.append(entry);
				}

				QJsonObject payload;
				payload["id"] = type.id;
				payload["name"] = jsonText(type.name);
				payload["parentId"] = type.parentTypeId;
				payload["domain"] = jsonText(PartTypeRepository::effectiveDomain(db, categoryId));
				payload["kicadCategory"] = jsonText(PartTypeRepository::effectiveKicadCategory(db, categoryId));
				payload["nameTemplate"] =
					jsonText(nameTemplateFor(PartTypeRepository::listTypes(db), categoryId));
				payload["description"] = jsonText(type.description);
				payload["attributes"] = attributes;
				payload["fileSlots"] = fileSlots;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeCreateCategory(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("create_category")
				.setDescription("Creates a part category, or returns the existing one when a category "
					"of that name already sits under the same parent. Safe to call twice: the second "
					"call answers with the same id and created=false, so there is no need to check "
					"first and no way to end up with two of them.")
				.setGroup(ToolGroup)
				.addParameter("name", "string", "Category name, e.g. \"Varistor\".", true)
				.addParameter("parentId", "integer",
					"Id of the parent category this one inherits attributes from. Omit for a "
					"top-level category.", false)
				.addEnumParameter("domain", domainValues(),
					"PartManager's own three-value classification of what a category holds: "
					"\"electronic\" for a component on a PCB, \"mechanical\" for a screw or a "
					"bracket, \"generic\" for anything else. This is NOT the product family, market "
					"segment or supplier category the part comes from — \"Circuit Protection\" is a "
					"category name, not a domain. It decides which default file slots and which "
					"KiCad handling the category gets, and every child category inherits it. "
					"Defaults to \"electronic\".", false)
				.addParameter("description", "string", "Free-text note about the category.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				if (!context.allowWrites)
				{
					return llmError(ReadOnlyMessage);
				}

				const QString name = stringArg(args, QStringLiteral("name"));
				if (name.isEmpty())
				{
					return llmError("'name' is required and must be a non-empty category name.");
				}

				int parentId = NoParentType;
				if (hasArg(args, QStringLiteral("parentId"))
					&& !intArg(args, QStringLiteral("parentId"), parentId))
				{
					return missingIdError(QStringLiteral("parentId"),
						QStringLiteral("Omit it for a top-level category."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				if (parentId != NoParentType)
				{
					PartType parent;
					if (!PartTypeRepository::findType(db, parentId, parent))
					{
						return unknownCategoryError(db, parentId);
					}
				}

				// Rule 3, and the measurement behind it: asked for a domain the model answered
				// "Circuit Protection" — it read the word as the supplier's product family rather
				// than as part_type.domain's three-value vocabulary. Free text there writes a value
				// effectiveDomain() then inherits down the whole §2b subtree, so it is checked here
				// as well as in the schema, and an unknown one comes back with the list.
				std::string domain = "electronic";
				if (hasArg(args, QStringLiteral("domain")))
				{
					const QString typed = folded(stringArg(args, QStringLiteral("domain")));
					if (!domainValues().contains(typed))
					{
						QJsonObject extra;
						extra["allowed_values"] = asJsonArray(domainValues());
						return llmError(QStringLiteral("'domain' must be one of %1 — got \"%2\". It is "
							"PartManager's own classification of what the category holds, not the "
							"product family the part comes from.")
							.arg(domainValues().join(QStringLiteral(", ")))
							.arg(stringArg(args, QStringLiteral("domain"))), extra);
					}
					domain = typed.toStdString();
				}

				// Rule 1: idempotent on (name, parentId). A model handed a create that forks on every
				// call keeps calling it — this is the line that ends that loop on the second try.
				const std::vector<PartType> types = PartTypeRepository::listTypes(db);
				for (const PartType& existing : types)
				{
					if (existing.parentTypeId == parentId && folded(existing.name) == folded(name))
					{
						QJsonObject payload;
						payload["id"] = existing.id;
						payload["created"] = false;
						payload["name"] = jsonText(existing.name);
						return llmOk(payload);
					}
				}

				PartType type;
				type.name = name.toStdString();
				type.parentTypeId = parentId;
				type.domain = domain;
				type.description = stringArg(args, QStringLiteral("description")).toStdString();

				const int newId = PartTypeRepository::insertType(db, type);
				if (newId == NoParentType)
				{
					return llmError(QStringLiteral("the category \"%1\" could not be written.").arg(name));
				}

				QJsonObject payload;
				payload["id"] = newId;
				payload["created"] = true;
				payload["name"] = jsonText(type.name);
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeAddCategoryAttribute(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("add_category_attribute")
				.setDescription("Adds a data field to a category, so every part filed under it can "
					"carry that value. Like create_category this is safe to call twice: a key the "
					"category already declares answers with the existing id and created=false.")
				.setGroup(ToolGroup)
				.addParameter("categoryId", "integer", "Category id from list_categories.", true)
				.addParameter("key", "string",
					"Stable lower-case identifier, letters digits and underscore only, e.g. "
					"\"clamping_voltage\". This is what set_part_attribute takes.", true)
				.addParameter("label", "string", "Human-readable field name, e.g. \"Clamping voltage\".", true)
				.addEnumParameter("datatype", datatypeValues(),
					"What kind of value a part stores in this field, from PartManager's own five-value "
					"set — not an SQL or C++ type name. \"dimension\" = a physical quantity with a "
					"unit, stored as a base-SI number, which is what nearly every electrical rating "
					"is. \"number\" = a bare count with no unit. \"text\" = free text. \"bool\" = "
					"yes/no. \"enum\" = one of the fixed values listed in enumOptions.", true)
				.addEnumParameter("unit", unitEnumValues(),
					"The physical unit of a dimension field, from PartManager's own fixed list — not "
					"free text. \"\xCE\xA9\" not \"Ohm\", \"F\" not \"Farad\". The SI prefix belongs to "
					"the value and not to the unit, so a capacitance field's unit is \"F\" and \"100n\" "
					"is a legal value for it. Omit it, or pass \"\", for a field with no unit.", false)
				.addParameter("required", "boolean", "Whether a part must fill this field. Default false.", false)
				.addParameter("searchable", "boolean",
					"Whether the value gets a fast-filter column so search_parts can compare against "
					"it. Default true for number and dimension.", false)
				.addParameter("enumOptions", "array",
					"The allowed values, required when datatype is enum.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				if (!context.allowWrites)
				{
					return llmError(ReadOnlyMessage);
				}

				int categoryId = 0;
				if (!intArg(args, QStringLiteral("categoryId"), categoryId))
				{
					return missingIdError(QStringLiteral("categoryId"),
						QStringLiteral("Call list_categories to get one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				PartType type;
				if (!PartTypeRepository::findType(db, categoryId, type))
				{
					return unknownCategoryError(db, categoryId);
				}

				const QString key = stringArg(args, QStringLiteral("key"));
				if (key.isEmpty())
				{
					return llmError("'key' is required: a stable lower-case identifier such as "
						"\"clamping_voltage\".");
				}
				for (const QChar& character : key)
				{
					if (!character.isLetterOrNumber() && character != QLatin1Char('_'))
					{
						return llmError(QStringLiteral("'key' may only contain letters, digits and "
							"underscores — got \"%1\". Put the readable spelling in 'label' and give "
							"'key' an identifier such as \"clamping_voltage\".").arg(key));
					}
				}

				const QString label = stringArg(args, QStringLiteral("label"));
				if (label.isEmpty())
				{
					return llmError("'label' is required: what the field is called on screen.");
				}

				const QString datatypeText = folded(stringArg(args, QStringLiteral("datatype")));
				if (!datatypeValues().contains(datatypeText))
				{
					QJsonObject extra;
					extra["allowed_values"] = asJsonArray(datatypeValues());
					return llmError(QStringLiteral("'datatype' must be one of %1 — got \"%2\". It says "
						"what kind of value a part stores in the field, not an SQL type.")
						.arg(datatypeValues().join(QStringLiteral(", ")))
						.arg(stringArg(args, QStringLiteral("datatype"))), extra);
				}
				const AttributeDataType datatype = attributeDataTypeFromString(datatypeText.toStdString());

				std::string unit;
				if (hasArg(args, QStringLiteral("unit")))
				{
					const QString typedUnit = stringArg(args, QStringLiteral("unit"));
					if (!typedUnit.isEmpty() && !canonicalUnit(typedUnit, unit))
					{
						QJsonObject extra;
						extra["allowed_values"] = asJsonArray(unitEnumValues());
						return llmError(QStringLiteral("\"%1\" is not one of the units this database "
							"knows. Allowed: %2. The SI prefix belongs to the value, not the unit — a "
							"field measured in kilohms has the unit \"%3\".")
							.arg(typedUnit).arg(unitChoices())
							.arg(QString::fromUtf8(UnitTable::OhmSymbol)), extra);
					}
				}
				if (!unit.empty() && datatype != AttributeDataType::Dimension)
				{
					return llmError(QStringLiteral("a unit only means something on a dimension field; "
						"\"%1\" was given on a %2 field.")
						.arg(QString::fromStdString(unit)).arg(datatypeText));
				}

				std::vector<std::string> enumOptions;
				for (const QString& option : stringListArg(args, QStringLiteral("enumOptions")))
				{
					enumOptions.push_back(option.toStdString());
				}
				if (datatype == AttributeDataType::Enum && enumOptions.empty())
				{
					return llmError("'enumOptions' is required when datatype is enum: the list of "
						"values a part may choose from.");
				}

				// Adding the same field twice is the same loop create_category guards against, so it
				// answers the same way. An *inherited* key is left alone: overriding one is a
				// legitimate §2b move and the row that does it belongs to this category.
				const std::vector<PartTypeAttribute> ownAttributes =
					PartTypeRepository::listOwnAttributes(db, categoryId);
				for (const PartTypeAttribute& existing : ownAttributes)
				{
					if (folded(existing.key) == folded(key))
					{
						QJsonObject payload;
						payload["id"] = existing.id;
						payload["created"] = false;
						payload["key"] = jsonText(existing.key);
						return llmOk(payload);
					}
				}

				PartTypeAttribute attribute;
				attribute.partTypeId = categoryId;
				attribute.key = key.toStdString();
				attribute.label = label.toStdString();
				attribute.unit = unit;
				attribute.datatype = datatype;
				attribute.enumOptions = enumOptions;
				attribute.required = boolArg(args, QStringLiteral("required"), false);
				attribute.searchable = boolArg(args, QStringLiteral("searchable"),
					datatype == AttributeDataType::Number || datatype == AttributeDataType::Dimension);
				attribute.sortOrder = static_cast<int>(ownAttributes.size());

				const int newId = PartTypeRepository::insertAttribute(db, attribute);
				if (newId == 0)
				{
					return llmError(QStringLiteral("the attribute \"%1\" could not be written.").arg(key));
				}

				QJsonObject payload;
				payload["id"] = newId;
				payload["created"] = true;
				payload["key"] = jsonText(attribute.key);
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSearchParts(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("search_parts")
				.setDescription("Finds parts using the app's own search box grammar. Plain words match "
					"name, part number, manufacturer and description; \"resistance>1k\" compares an "
					"attribute; \"tag:SMD\" filters by tag. An empty query lists everything in scope. "
					"Call this before create_part to check the part is not already in the library.")
				.setGroup(ToolGroup)
				.addParameter("query", "string",
					"Search text, e.g. \"LM358\" or \"resistance>1k tag:SMD\". Empty matches everything.", true)
				.addParameter("categoryId", "integer",
					"Restrict to this one category. Child categories are not included. Omit to search "
					"the whole database.", false)
				.addParameter("limit", "integer", "Maximum rows to return, 1-200. Default 25.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}

				int categoryId = 0;
				if (hasArg(args, QStringLiteral("categoryId"))
					&& !intArg(args, QStringLiteral("categoryId"), categoryId))
				{
					return missingIdError(QStringLiteral("categoryId"),
						QStringLiteral("Omit it to search the whole database."));
				}

				int limit = 25;
				if (hasArg(args, QStringLiteral("limit")) && !intArg(args, QStringLiteral("limit"), limit))
				{
					return llmError("'limit' must be a whole number between 1 and 200.");
				}
				limit = std::max(1, std::min(200, limit));

				SQLiteWrapper::SQLite& db = context.database->connection();
				if (categoryId != 0)
				{
					PartType type;
					if (!PartTypeRepository::findType(db, categoryId, type))
					{
						return unknownCategoryError(db, categoryId);
					}
				}

				const QString queryText = stringArg(args, QStringLiteral("query"));
				const SearchQuery query = SearchQuery::parse(queryText.toStdString());
				if (!query.ok)
				{
					return llmError(QStringLiteral("the query \"%1\" could not be read: %2")
						.arg(queryText).arg(QString::fromStdString(query.error)));
				}

				const std::vector<Part> found = SearchEngine::search(db, query, categoryId);
				const std::vector<PartType> types = PartTypeRepository::listTypes(db);

				QJsonArray parts;
				for (const Part& part : found)
				{
					if (parts.size() >= limit)
					{
						break;
					}
					std::string categoryName;
					for (const PartType& type : types)
					{
						if (type.id == part.partTypeId)
						{
							categoryName = type.name;
						}
					}
					QJsonObject entry;
					entry["id"] = part.id;
					entry["name"] = jsonText(part.name);
					entry["mpn"] = jsonText(part.mpn);
					entry["manufacturer"] = jsonText(part.manufacturer);
					entry["categoryId"] = part.partTypeId;
					entry["categoryName"] = jsonText(categoryName);
					entry["stockQty"] = part.stockQty;
					parts.append(entry);
				}

				QJsonObject payload;
				payload["parts"] = parts;
				payload["total"] = static_cast<int>(found.size());
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeGetPart(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("get_part")
				.setDescription("Everything stored about one part: its record, every attribute its "
					"category declares together with the value this part carries, its tags and its "
					"attached files.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts or create_part.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				PartType type;
				PartTypeRepository::findType(db, part.partTypeId, type);

				const QJsonObject stored = attributeObject(part);
				QJsonArray attributes;
				for (const PartTypeAttribute& attribute :
					PartTypeRepository::effectiveAttributes(db, part.partTypeId))
				{
					QJsonObject entry = attributeSchema(attribute);
					const QString key = QString::fromStdString(attribute.key);
					if (stored.contains(key))
					{
						const QJsonValue raw = stored.value(key);
						entry["value"] = raw.isObject() ? raw.toObject().value(QStringLiteral("value")) : raw;
					}
					attributes.append(entry);
				}

				QJsonArray tags;
				for (const Tag& tag : TagRepository::listPartTags(db, partId))
				{
					tags.append(jsonText(tag.name));
				}

				QJsonArray files;
				for (const PartFile& file : PartRepository::listFiles(db, partId))
				{
					QJsonObject entry;
					entry["id"] = file.id;
					entry["role"] = jsonText(file.role);
					entry["filename"] = jsonText(file.originalFilename);
					entry["mimeType"] = jsonText(file.mimeType);
					entry["sizeBytes"] = file.sizeBytes;
					files.append(entry);
				}

				QJsonObject payload;
				payload["id"] = part.id;
				payload["name"] = jsonText(part.name);
				payload["mpn"] = jsonText(part.mpn);
				payload["manufacturer"] = jsonText(part.manufacturer);
				payload["description"] = jsonText(part.description);
				payload["package"] = jsonText(part.package);
				payload["categoryId"] = part.partTypeId;
				payload["categoryName"] = jsonText(type.name);
				payload["stockQty"] = part.stockQty;
				payload["stockMinQty"] = part.stockMinQty;
				payload["attributes"] = attributes;
				payload["tags"] = tags;
				payload["files"] = files;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeCreatePart(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("create_part")
				.setDescription("Creates a part in one category. The categoryId must come from "
					"list_categories or create_category — a category name is not accepted, because two "
					"branches may legitimately hold a category of the same name. Attribute values are "
					"set afterwards with set_part_attribute.")
				.setGroup(ToolGroup)
				.addParameter("categoryId", "integer", "Category id from list_categories.", true)
				.addParameter("name", "string", "Display name for the part, e.g. \"LM358N\".", true)
				.addParameter("mpn", "string", "Manufacturer part number.", false)
				.addParameter("manufacturer", "string", "Who makes it, e.g. \"Texas Instruments\".", false)
				.addParameter("description", "string", "Free-text notes.", false)
				.addParameter("package", "string", "Package or form factor, e.g. \"SOIC-8\".", false)
				.addParameter("stockQty", "integer", "Opening quantity on the shelf. Default 0.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				if (!context.allowWrites)
				{
					return llmError(ReadOnlyMessage);
				}

				int categoryId = 0;
				if (!intArg(args, QStringLiteral("categoryId"), categoryId))
				{
					return missingIdError(QStringLiteral("categoryId"),
						QStringLiteral("Call list_categories and use an id from there — a category "
							"name is not accepted."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				PartType type;
				if (!PartTypeRepository::findType(db, categoryId, type))
				{
					return unknownCategoryError(db, categoryId);
				}

				const QString name = stringArg(args, QStringLiteral("name"));
				if (name.isEmpty())
				{
					return llmError("'name' is required and must be a non-empty part name.");
				}

				int stockQty = 0;
				if (hasArg(args, QStringLiteral("stockQty"))
					&& !intArg(args, QStringLiteral("stockQty"), stockQty))
				{
					return llmError("'stockQty' must be a whole number.");
				}

				Part part;
				part.partTypeId = categoryId;
				part.name = name.toStdString();
				part.mpn = stringArg(args, QStringLiteral("mpn")).toStdString();
				part.manufacturer = stringArg(args, QStringLiteral("manufacturer")).toStdString();
				part.description = stringArg(args, QStringLiteral("description")).toStdString();
				part.package = stringArg(args, QStringLiteral("package")).toStdString();
				part.stockQty = stockQty;

				const int newId = PartRepository::insertPart(db, part);
				if (newId == 0)
				{
					return llmError(QStringLiteral("the part \"%1\" could not be written.").arg(name));
				}

				QJsonObject payload;
				payload["id"] = newId;
				payload["categoryId"] = categoryId;
				payload["categoryName"] = jsonText(type.name);
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeUpdatePart(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("update_part")
				.setDescription("Changes fields on an existing part. Only the parameters actually "
					"supplied are written; everything else keeps its current value. Attribute values "
					"are not set here — use set_part_attribute.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts or create_part.", true)
				.addParameter("name", "string", "New display name.", false)
				.addParameter("mpn", "string", "New manufacturer part number.", false)
				.addParameter("manufacturer", "string", "New manufacturer.", false)
				.addParameter("description", "string", "New free-text notes.", false)
				.addParameter("package", "string", "New package or form factor.", false)
				.addParameter("stockMinQty", "integer", "New reorder threshold.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				if (!context.allowWrites)
				{
					return llmError(ReadOnlyMessage);
				}

				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				QJsonArray changed;
				if (hasArg(args, QStringLiteral("name")))
				{
					const QString name = stringArg(args, QStringLiteral("name"));
					if (name.isEmpty())
					{
						return llmError("'name' was given but is empty. Omit it to keep the current name.");
					}
					part.name = name.toStdString();
					changed.append(QStringLiteral("name"));
				}
				if (hasArg(args, QStringLiteral("mpn")))
				{
					part.mpn = stringArg(args, QStringLiteral("mpn")).toStdString();
					changed.append(QStringLiteral("mpn"));
				}
				if (hasArg(args, QStringLiteral("manufacturer")))
				{
					part.manufacturer = stringArg(args, QStringLiteral("manufacturer")).toStdString();
					changed.append(QStringLiteral("manufacturer"));
				}
				if (hasArg(args, QStringLiteral("description")))
				{
					part.description = stringArg(args, QStringLiteral("description")).toStdString();
					changed.append(QStringLiteral("description"));
				}
				if (hasArg(args, QStringLiteral("package")))
				{
					part.package = stringArg(args, QStringLiteral("package")).toStdString();
					changed.append(QStringLiteral("package"));
				}
				if (hasArg(args, QStringLiteral("stockMinQty")))
				{
					int stockMinQty = 0;
					if (!intArg(args, QStringLiteral("stockMinQty"), stockMinQty))
					{
						return llmError("'stockMinQty' must be a whole number.");
					}
					part.stockMinQty = stockMinQty;
					changed.append(QStringLiteral("stockMinQty"));
				}

				if (changed.isEmpty())
				{
					return llmError("nothing to update: pass at least one of name, mpn, manufacturer, "
						"description, package, stockMinQty.");
				}
				if (!PartRepository::updatePart(db, part))
				{
					return llmError(QStringLiteral("part %1 could not be written.").arg(partId));
				}

				QJsonObject payload;
				payload["id"] = partId;
				payload["updated"] = changed;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSetPartAttribute(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("set_part_attribute")
				.setDescription("Writes one attribute value onto a part. The value is parsed the way "
					"the app's own entry fields parse it, so \"4k7\" in an ohms field is stored as "
					"4700 and \"100n\" in a farads field as 1e-7. Write the value as the datasheet "
					"spells it and let the parser do the conversion; a value it cannot read is "
					"refused rather than stored as zero.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts or create_part.", true)
				.addParameter("key", "string",
					"Attribute key as get_category lists it, e.g. \"resistance\".", true)
				.addParameter("value", "string",
					"The value as written on the datasheet, e.g. \"4k7\", \"100nF\", \"0603\".", true)
				.addEnumParameter("unit", unitEnumValues(),
					"Optional cross-check: the unit you believe the field uses, from PartManager's own "
					"fixed list. The write is refused when it disagrees with the unit the category "
					"declared, so a field misread as farads never lands in the database as one.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				if (!context.allowWrites)
				{
					return llmError(ReadOnlyMessage);
				}

				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				const QString key = stringArg(args, QStringLiteral("key"));
				if (key.isEmpty())
				{
					return llmError("'key' is required: an attribute key from get_category.");
				}

				const std::vector<PartTypeAttribute> attributes =
					PartTypeRepository::effectiveAttributes(db, part.partTypeId);
				const PartTypeAttribute* attribute = nullptr;
				for (const PartTypeAttribute& candidate : attributes)
				{
					if (folded(candidate.key) == folded(key))
					{
						attribute = &candidate;
					}
				}
				if (attribute == nullptr)
				{
					QJsonObject extra;
					QJsonArray keys;
					for (const QString& known : attributeKeys(attributes))
					{
						keys.append(known);
					}
					extra["keys"] = keys;
					return llmError(QStringLiteral("this part's category declares no attribute \"%1\". "
						"It has: %2. Use add_category_attribute to declare a new one.")
						.arg(key).arg(idNameSummary(attributeKeys(attributes))), extra);
				}

				if (!hasArg(args, QStringLiteral("value")))
				{
					return llmError("'value' is required.");
				}
				const QJsonValue rawValue = args.value(QStringLiteral("value"));
				const QString valueText = rawValue.isString()
					? rawValue.toString().trimmed()
					: (rawValue.isBool()
						? (rawValue.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
						: QString::number(rawValue.toDouble(), 'g', 15));

				// The optional cross-check. A model that believes a resistance field is in farads has
				// misread the category, and letting the write through would put the misreading in the
				// database where nothing looks at it again.
				if (hasArg(args, QStringLiteral("unit")))
				{
					const QString claimed = stringArg(args, QStringLiteral("unit"));
					if (!claimed.isEmpty() && !UnitTable::unitsEqual(claimed.toStdString(), attribute->unit))
					{
						return llmError(QStringLiteral("\"%1\" declares the unit \"%2\", not \"%3\". "
							"Write the value in %2, or omit the unit parameter.")
							.arg(QString::fromStdString(attribute->key))
							.arg(attribute->unit.empty()
								? QStringLiteral("(no unit)")
								: QString::fromStdString(attribute->unit))
							.arg(claimed));
					}
				}

				QJsonObject attributeJsonObject = attributeObject(part);
				const QString storedKey = QString::fromStdString(attribute->key);
				QJsonValue storedValue;
				QString storedUnit = QString::fromStdString(attribute->unit);

				switch (attribute->datatype)
				{
				case AttributeDataType::Number:
				case AttributeDataType::Dimension:
				{
					// §2a: the typed text never reaches the column. What is stored is the base-SI
					// number the parser produced, under the unit the category declared.
					const ValueParseResult parsed = ValueParser::parse(valueText.toStdString(), attribute->unit);
					if (!parsed.ok)
					{
						return llmError(QStringLiteral("\"%1\" is not a value \"%2\" can hold. It expects "
							"a number, optionally with an SI prefix and the unit \"%3\" — \"4k7\", "
							"\"4.7k\", \"4700\" and \"4.7 k%3\" are all accepted. Nothing was written.")
							.arg(valueText)
							.arg(QString::fromStdString(attribute->key))
							.arg(attribute->unit.empty()
								? QStringLiteral("(no unit)")
								: QString::fromStdString(attribute->unit)));
					}
					QJsonObject entry;
					entry["value"] = parsed.value;
					entry["unit"] = jsonText(attribute->unit);
					attributeJsonObject[storedKey] = entry;
					storedValue = parsed.value;
					break;
				}
				case AttributeDataType::Bool:
				{
					const QString text = folded(valueText);
					bool flag = false;
					if (text == QStringLiteral("true") || text == QStringLiteral("1")
						|| text == QStringLiteral("yes"))
					{
						flag = true;
					}
					else if (text == QStringLiteral("false") || text == QStringLiteral("0")
						|| text == QStringLiteral("no"))
					{
						flag = false;
					}
					else
					{
						return llmError(QStringLiteral("\"%1\" is not a yes/no value. \"%2\" expects true "
							"or false. Nothing was written.")
							.arg(valueText).arg(QString::fromStdString(attribute->key)));
					}
					attributeJsonObject[storedKey] = flag;
					storedValue = flag;
					storedUnit = QString();
					break;
				}
				case AttributeDataType::Enum:
				{
					// Rule 3: the enum is checked here as well as by the client-side validator. The
					// model that answered a glyph enum with an emoji got through the schema.
					std::string chosen;
					for (const std::string& option : attribute->enumOptions)
					{
						if (folded(option) == folded(valueText))
						{
							chosen = option;
						}
					}
					if (chosen.empty())
					{
						QStringList options;
						for (const std::string& option : attribute->enumOptions)
						{
							options.append(QString::fromStdString(option));
						}
						QJsonObject extra;
						QJsonArray allowed;
						for (const QString& option : options)
						{
							allowed.append(option);
						}
						extra["allowed_values"] = allowed;
						return llmError(QStringLiteral("\"%1\" is not one of the values \"%2\" allows: "
							"%3. Nothing was written.")
							.arg(valueText)
							.arg(QString::fromStdString(attribute->key))
							.arg(options.isEmpty() ? QStringLiteral("none") : options.join(QStringLiteral(", "))),
							extra);
					}
					attributeJsonObject[storedKey] = jsonText(chosen);
					storedValue = QString::fromStdString(chosen);
					storedUnit = QString();
					break;
				}
				case AttributeDataType::Text:
				default:
				{
					attributeJsonObject[storedKey] = valueText;
					storedValue = valueText;
					storedUnit = QString();
					break;
				}
				}

				part.attributes = attributeJson(attributeJsonObject);
				if (!PartRepository::updatePart(db, part))
				{
					return llmError(QStringLiteral("part %1 could not be written.").arg(partId));
				}

				QJsonObject payload;
				payload["key"] = jsonText(attribute->key);
				payload["storedValue"] = storedValue;
				payload["storedUnit"] = storedUnit;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeListTags(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("list_tags")
				.setDescription("Every tag the database knows, with the family it belongs to. "
					"set_part_tags only accepts names from this list — tags are a managed vocabulary "
					"and are not invented per part.")
				.setGroup(ToolGroup);

			tool.handler = [context](const QJsonObject&) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				SQLiteWrapper::SQLite& db = context.database->connection();
				const std::vector<TagCategory> categories = TagRepository::listCategories(db);

				QJsonArray tags;
				for (const Tag& tag : TagRepository::listTags(db))
				{
					std::string categoryName;
					for (const TagCategory& category : categories)
					{
						if (category.id == tag.categoryId)
						{
							categoryName = category.name;
						}
					}
					QJsonObject entry;
					entry["id"] = tag.id;
					entry["name"] = jsonText(tag.name);
					entry["category"] = jsonText(categoryName);
					tags.append(entry);
				}

				QJsonObject payload;
				payload["tags"] = tags;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSetPartTags(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("set_part_tags")
				.setDescription("Replaces a part's whole tag set with the names given. Names must "
					"already exist — call list_tags first. Names that do not are reported back in "
					"\"unknown\" and change nothing, so one bad name does not lose the good ones. An "
					"empty list clears the part's tags.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts or create_part.", true)
				.addParameter("tags", "array", "Tag names from list_tags, e.g. [\"SMD\", \"I2C\"].", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				if (!context.allowWrites)
				{
					return llmError(ReadOnlyMessage);
				}

				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}
				if (!hasArg(args, QStringLiteral("tags")))
				{
					return llmError("'tags' is required: the list of tag names the part should carry. "
						"Pass an empty list to clear them.");
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				const std::vector<Tag> known = TagRepository::listTags(db);
				std::vector<int> tagIds;
				QJsonArray applied;
				QJsonArray unknown;
				for (const QString& name : stringListArg(args, QStringLiteral("tags")))
				{
					const Tag* match = nullptr;
					for (const Tag& tag : known)
					{
						if (folded(tag.name) == folded(name))
						{
							match = &tag;
						}
					}
					if (match == nullptr)
					{
						unknown.append(name);
						continue;
					}
					if (std::find(tagIds.begin(), tagIds.end(), match->id) == tagIds.end())
					{
						tagIds.push_back(match->id);
						applied.append(jsonText(match->name));
					}
				}

				if (!TagRepository::setPartTags(db, partId, tagIds))
				{
					return llmError(QStringLiteral("the tags of part %1 could not be written.").arg(partId));
				}

				QJsonObject payload;
				payload["applied"] = applied;
				payload["unknown"] = unknown;
				return llmOk(payload);
			};
			return tool;
		}

#endif // SQLITEWRAPPER_LIBRARY_AVAILABLE
	}

	std::vector<LlmTool> PartToolset::tools(const LlmToolContext& context)
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		std::vector<LlmTool> tools;
		tools.push_back(makeListCategories(context));
		tools.push_back(makeGetCategory(context));
		tools.push_back(makeCreateCategory(context));
		tools.push_back(makeAddCategoryAttribute(context));
		tools.push_back(makeSearchParts(context));
		tools.push_back(makeGetPart(context));
		tools.push_back(makeCreatePart(context));
		tools.push_back(makeUpdatePart(context));
		tools.push_back(makeSetPartAttribute(context));
		tools.push_back(makeListTags(context));
		tools.push_back(makeSetPartTags(context));
		return tools;
#else
		// No SQLite means no database to expose. The toolset still exists so a caller does not have
		// to know which optional dependency was left out; it simply offers nothing.
		PM_UNUSED(context);
		return std::vector<LlmTool>();
#endif
	}

	std::vector<QString> PartToolset::migrationToolNames()
	{
		// Every database tool, not a hand-picked few. **Measured 2026-09-26:** the migration loop
		// (search -> suggest category -> list categories -> create category -> import) ran in five
		// calls and 71 s with all fourteen tools advertised — eleven here plus the three Mouser
		// ones. The length of the list is not what a small model trips over; a parameter whose
		// meaning it can misread is (see create_category's `domain`). So this trims nothing for
		// size, and exists to keep the migration's tool set named in one place instead of
		// assembled at every call site.
		return {
			QStringLiteral("list_categories"),
			QStringLiteral("get_category"),
			QStringLiteral("create_category"),
			QStringLiteral("add_category_attribute"),
			QStringLiteral("search_parts"),
			QStringLiteral("get_part"),
			QStringLiteral("create_part"),
			QStringLiteral("update_part"),
			QStringLiteral("set_part_attribute"),
			QStringLiteral("list_tags"),
			QStringLiteral("set_part_tags")
		};
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
