#include "import/PartManager_PartTypeTransfer.h"
#include "PartManager_info.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "database/PartManager_DatabaseMetadata.h"
	#include "database/PartManager_SchemaMigrator.h"
	#include "database/PartManager_Transaction.h"
	#include "persistence/PartManager_ListColumnRepository.h"
	#include "persistence/PartManager_PartTypeRepository.h"
	#include "persistence/PartManager_TagRepository.h"
	#include "SQLite.h"
#endif

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace PartManager
{
	namespace
	{
		const char* BundleFormatMarkerText = "partmanager-category-bundle";

		// Case-insensitive comparison, done through Qt rather than std::tolower. A category name is
		// arbitrary user text in UTF-8, and std::tolower works on single bytes: it would fold
		// `Resistor` but leave `Widerstände` and `widerstände` as two different categories, which
		// for a user working in German is the normal case and not an exotic one. toCaseFolded()
		// rather than toLower() because folding is what Unicode defines *for matching*.
		std::string caseFolded(const std::string& text)
		{
			return QString::fromStdString(text).toCaseFolded().toStdString();
		}

		// QString::trimmed() and not a std::isspace loop, for the same reason: it knows the
		// non-breaking space and the rest of Unicode's whitespace, which a byte test does not.
		std::string trimmed(const std::string& text)
		{
			return QString::fromStdString(text).trimmed().toStdString();
		}

		// **The** comparison form of one name — one path segment, one leaf name, one tag name.
		// Both the path match and the unique-leaf-name fallback go through this single helper, so
		// the two can never drift into disagreeing about what "the same name" means.
		std::string normalizedName(const std::string& name)
		{
			return caseFolded(trimmed(name));
		}

		// Bool as it is compared and displayed in a conflict row. Not "true"/"false": the same
		// two characters are what the column actually stores, so a conflict reads the same as
		// the database does.
		const char* boolText(bool value)
		{
			return value ? "1" : "0";
		}

		std::string joinOptions(const std::vector<std::string>& options)
		{
			std::string out;
			for (size_t i = 0; i < options.size(); ++i)
			{
				if (i > 0) { out += ", "; }
				out += options[i];
			}
			return out;
		}

		// Current UTC time as "YYYY-MM-DDTHH:MM:SS" — the shape §1a's db_meta timestamps use,
		// copied from DatabaseHandle rather than invented, so every timestamp in this project
		// sorts against every other one.
		std::string nowIso8601()
		{
			std::time_t t = std::time(nullptr);
			std::tm tmValue{};
#ifdef _MSC_VER
			gmtime_s(&tmValue, &t);
#else
			gmtime_r(&t, &tmValue);
#endif
			char buffer[32];
			std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tmValue);
			return std::string(buffer);
		}

		// The path minus its last segment. Empty for a root category. A list, so this is a drop
		// of the last element rather than a search for a separator that may sit inside a name.
		std::vector<std::string> parentOfPath(const std::vector<std::string>& segments)
		{
			if (segments.size() < 2)
			{
				return std::vector<std::string>();
			}
			return std::vector<std::string>(segments.begin(), segments.end() - 1);
		}

		std::string leafOfPath(const std::vector<std::string>& segments)
		{
			return segments.empty() ? std::string() : segments.back();
		}

		QJsonArray attributesToJson(const std::vector<PartTypeAttribute>& attributes)
		{
			QJsonArray array;
			for (const PartTypeAttribute& attribute : attributes)
			{
				QJsonObject object;
				object["key"] = QString::fromStdString(attribute.key);
				object["label"] = QString::fromStdString(attribute.label);
				object["unit"] = QString::fromStdString(attribute.unit);
				object["datatype"] = QString::fromStdString(toString(attribute.datatype));
				QJsonArray options;
				for (const std::string& option : attribute.enumOptions)
				{
					options.append(QString::fromStdString(option));
				}
				object["enumOptions"] = options;
				object["searchable"] = attribute.searchable;
				object["required"] = attribute.required;
				object["tooltip"] = QString::fromStdString(attribute.tooltip);
				object["sortOrder"] = attribute.sortOrder;
				array.append(object);
			}
			return array;
		}

		// `fileSlots`, not `slots`: Qt's moc keywords #define `slots` away to nothing.
		QJsonArray fileSlotsToJson(const std::vector<PartTypeFileSlot>& fileSlots)
		{
			QJsonArray array;
			for (const PartTypeFileSlot& slot : fileSlots)
			{
				QJsonObject object;
				object["role"] = QString::fromStdString(slot.role);
				object["label"] = QString::fromStdString(slot.label);
				object["required"] = slot.required;
				object["tooltip"] = QString::fromStdString(slot.tooltip);
				object["sortOrder"] = slot.sortOrder;
				array.append(object);
			}
			return array;
		}

		QJsonArray listColumnsToJson(const std::vector<PartTypeListColumn>& columns)
		{
			QJsonArray array;
			for (const PartTypeListColumn& column : columns)
			{
				QJsonObject object;
				object["columnKey"] = QString::fromStdString(column.columnKey);
				object["labelOverride"] = QString::fromStdString(column.labelOverride);
				object["visible"] = column.visible;
				object["sortOrder"] = column.sortOrder;
				object["widthPx"] = column.widthPx;
				array.append(object);
			}
			return array;
		}

		std::vector<PartTypeAttribute> attributesFromJson(const QJsonArray& array)
		{
			std::vector<PartTypeAttribute> result;
			for (const QJsonValue& value : array)
			{
				const QJsonObject object = value.toObject();
				PartTypeAttribute attribute;
				attribute.key = object["key"].toString().toStdString();
				attribute.label = object["label"].toString().toStdString();
				attribute.unit = object["unit"].toString().toStdString();
				attribute.datatype = attributeDataTypeFromString(object["datatype"].toString().toStdString());
				for (const QJsonValue& option : object["enumOptions"].toArray())
				{
					attribute.enumOptions.push_back(option.toString().toStdString());
				}
				attribute.searchable = object["searchable"].toBool();
				attribute.required = object["required"].toBool();
				attribute.tooltip = object["tooltip"].toString().toStdString();
				attribute.sortOrder = object["sortOrder"].toInt();
				result.push_back(attribute);
			}
			return result;
		}

		std::vector<PartTypeFileSlot> fileSlotsFromJson(const QJsonArray& array)
		{
			std::vector<PartTypeFileSlot> result;
			for (const QJsonValue& value : array)
			{
				const QJsonObject object = value.toObject();
				PartTypeFileSlot slot;
				slot.role = object["role"].toString().toStdString();
				slot.label = object["label"].toString().toStdString();
				slot.required = object["required"].toBool();
				slot.tooltip = object["tooltip"].toString().toStdString();
				slot.sortOrder = object["sortOrder"].toInt();
				result.push_back(slot);
			}
			return result;
		}

		std::vector<PartTypeListColumn> listColumnsFromJson(const QJsonArray& array)
		{
			std::vector<PartTypeListColumn> result;
			for (const QJsonValue& value : array)
			{
				const QJsonObject object = value.toObject();
				PartTypeListColumn column;
				column.columnKey = object["columnKey"].toString().toStdString();
				column.labelOverride = object["labelOverride"].toString().toStdString();
				column.visible = object["visible"].toBool();
				column.sortOrder = object["sortOrder"].toInt();
				column.widthPx = object["widthPx"].toInt();
				result.push_back(column);
			}
			return result;
		}
	}

	const char* bundleFormatMarker()
	{
		return BundleFormatMarkerText;
	}

	std::string displayPartTypePath(const std::vector<std::string>& segments)
	{
		// U+203A, spaced — the same glyph PartTypePickerDialog::partTypePath() puts between
		// category names, so one category reads the same in a picker, a warning and an error.
		static const char* const DisplaySeparator = " \xE2\x80\xBA ";
		std::string out;
		for (size_t i = 0; i < segments.size(); ++i)
		{
			if (i > 0) { out += DisplaySeparator; }
			out += segments[i];
		}
		return out;
	}

	std::vector<std::string> normalizedPartTypePath(const std::vector<std::string>& segments)
	{
		std::vector<std::string> out = segments;
		for (std::string& segment : out)
		{
			segment = normalizedName(segment);
		}
		return out;
	}

	std::vector<std::string> splitLegacyPartTypePath(const std::string& path)
	{
		std::vector<std::string> segments;
		size_t begin = 0;
		while (begin <= path.size())
		{
			size_t cut = path.find(PartTypePathSeparator, begin);
			if (cut == std::string::npos)
			{
				segments.push_back(path.substr(begin));
				break;
			}
			segments.push_back(path.substr(begin, cut - begin));
			begin = cut + 1;
		}
		return segments;
	}

	std::string toJson(const PartTypeBundle& bundle)
	{
		QJsonObject root;
		// The marker goes in first and is checked first: a JSON file that does not carry it is
		// not one of ours however plausible its other fields look.
		root["format"] = QString::fromLatin1(BundleFormatMarkerText);
		// Always the current version, never the bundle's own: a v1 file read in and written back
		// out is a v2 file, because its path is now a list and a v1 reader would mis-split it.
		root["formatVersion"] = CurrentBundleFormatVersion;
		root["sourceToolVersion"] = QString::fromStdString(bundle.sourceToolVersion);
		root["sourceDatabaseName"] = QString::fromStdString(bundle.sourceDatabaseName);
		root["exportedAt"] = QString::fromStdString(bundle.exportedAt);

		QJsonArray nodes;
		for (const PartTypeNodeBundle& node : bundle.nodes)
		{
			QJsonObject object;
			// An array, one entry per category from the root down. A joined string was the v1
			// shape and could not survive a category named `Crystal / Oscilator` (see header).
			QJsonArray pathSegments;
			for (const std::string& segment : node.path)
			{
				pathSegments.append(QString::fromStdString(segment));
			}
			object["path"] = pathSegments;
			// No "id" and no "parentTypeId": both are local primary keys and would be a lie in
			// any other database (see header). The parent is `path` minus its last segment.
			object["name"] = QString::fromStdString(node.type.name);
			object["domain"] = QString::fromStdString(node.type.domain);
			object["kicadRelevant"] = node.type.kicadRelevant;
			object["kicadCategory"] = QString::fromStdString(node.type.kicadCategory);
			object["description"] = QString::fromStdString(node.type.description);
			object["searchKeywords"] = QString::fromStdString(node.type.searchKeywords);
			object["excludedKeywords"] = QString::fromStdString(node.type.excludedKeywords);
			object["nameTemplate"] = QString::fromStdString(node.type.nameTemplate);
			// §14c. Carried so a category arrives looking like itself; an older file simply has
			// neither key, and "" / 0 is the "derive it from the name" value.
			object["iconGlyph"] = QString::fromStdString(node.type.iconGlyph);
			object["iconColour"] = static_cast<int>(node.type.iconColour);
			object["attributes"] = attributesToJson(node.ownAttributes);
			object["fileSlots"] = fileSlotsToJson(node.ownFileSlots);
			object["listColumns"] = listColumnsToJson(node.ownListColumns);
			QJsonArray tags;
			for (const std::string& tagName : node.defaultTagNames)
			{
				tags.append(QString::fromStdString(tagName));
			}
			object["defaultTags"] = tags;
			nodes.append(object);
		}
		root["nodes"] = nodes;

		return QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
	}

	bool fromJson(const std::string& text, PartTypeBundle& outBundle, std::string& outError)
	{
		outError.clear();
		outBundle = PartTypeBundle();

		QJsonParseError parseError{};
		QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(text), &parseError);
		if (parseError.error != QJsonParseError::NoError || !document.isObject())
		{
			outError = "This file is not readable JSON.";
			return false;
		}
		const QJsonObject root = document.object();

		if (root.value("format").toString().toStdString() != BundleFormatMarkerText)
		{
			outError = std::string("This file is not a PartManager category bundle (expected a \"format\": \"")
				+ BundleFormatMarkerText + "\" marker).";
			return false;
		}

		const int formatVersion = root.value("formatVersion").toInt(0);
		if (formatVersion <= 0)
		{
			outError = "This category bundle carries no format version.";
			return false;
		}
		if (formatVersion > CurrentBundleFormatVersion)
		{
			// §1c's stance, applied to a file: refuse what is newer rather than read it
			// half-understood. A field this build has never heard of is silently dropped
			// otherwise, and the user finds out when the category is already wrong.
			outError = "This category bundle was written by a newer PartManager (bundle format v"
				+ std::to_string(formatVersion) + ") — this copy only supports up to v"
				+ std::to_string(CurrentBundleFormatVersion) + ". Update PartManager to import it.";
			return false;
		}

		outBundle.formatVersion = formatVersion;
		outBundle.sourceToolVersion = root.value("sourceToolVersion").toString().toStdString();
		outBundle.sourceDatabaseName = root.value("sourceDatabaseName").toString().toStdString();
		outBundle.exportedAt = root.value("exportedAt").toString().toStdString();

		for (const QJsonValue& value : root.value("nodes").toArray())
		{
			const QJsonObject object = value.toObject();
			PartTypeNodeBundle node;
			const QJsonValue pathValue = object["path"];
			if (pathValue.isArray())
			{
				// v2 and later: one entry per category, whatever characters are in it.
				for (const QJsonValue& segment : pathValue.toArray())
				{
					node.path.push_back(segment.toString().toStdString());
				}
			}
			else
			{
				// v1: one '/'-joined string. Split back, and lossily so — a v1 writer had no way
				// to escape a '/' that belonged to a category's own name, so a v1 file holding
				// `Crystal / Oscilator` arrives as two segments and there is no information left
				// in the file to tell that apart from a real parent and child. Reading it at all
				// is the point: a .pmcat exported before v2 must still import.
				const std::string legacy = pathValue.toString().toStdString();
				if (!legacy.empty())
				{
					node.path = splitLegacyPartTypePath(legacy);
				}
			}
			node.type.name = object["name"].toString().toStdString();
			node.type.domain = object["domain"].toString().toStdString();
			node.type.kicadRelevant = object["kicadRelevant"].toBool();
			node.type.kicadCategory = object["kicadCategory"].toString().toStdString();
			node.type.description = object["description"].toString().toStdString();
			node.type.searchKeywords = object["searchKeywords"].toString().toStdString();
			node.type.excludedKeywords = object["excludedKeywords"].toString().toStdString();
			node.type.nameTemplate = object["nameTemplate"].toString().toStdString();
			node.type.iconGlyph = object["iconGlyph"].toString().toStdString();
			node.type.iconColour = static_cast<std::uint32_t>(object["iconColour"].toInt());
			node.ownAttributes = attributesFromJson(object["attributes"].toArray());
			node.ownFileSlots = fileSlotsFromJson(object["fileSlots"].toArray());
			node.ownListColumns = listColumnsFromJson(object["listColumns"].toArray());
			for (const QJsonValue& tagValue : object["defaultTags"].toArray())
			{
				node.defaultTagNames.push_back(tagValue.toString().toStdString());
			}
			outBundle.nodes.push_back(node);
		}
		return true;
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		// The local tree, read once. Every path question below is answered from this rather than
		// from a query per ancestor — planMerge() walks the whole forest and would otherwise do
		// one SELECT per level per category.
		struct LocalTree
		{
			std::vector<PartType> types;
			std::unordered_map<int, size_t> indexById;

			const PartType* find(int id) const
			{
				auto it = indexById.find(id);
				return it == indexById.end() ? nullptr : &types[it->second];
			}

			// Root first, one segment per category, using the real (untrimmed, uncased) names. A
			// parent_type_id cycle stops the walk instead of looping, exactly as
			// ancestorChainRootFirst() does.
			std::vector<std::string> pathOf(int id) const
			{
				std::vector<std::string> segments;
				std::unordered_set<int> visited;
				int current = id;
				while (current != NoParentType && visited.insert(current).second)
				{
					const PartType* type = find(current);
					if (!type) { break; }
					segments.push_back(type->name);
					current = type->parentTypeId;
				}
				std::reverse(segments.begin(), segments.end());
				return segments;
			}
		};

		LocalTree readLocalTree(SQLiteWrapper::SQLite& db)
		{
			LocalTree tree;
			tree.types = PartTypeRepository::listTypes(db);
			for (size_t i = 0; i < tree.types.size(); ++i)
			{
				tree.indexById[tree.types[i].id] = i;
			}
			return tree;
		}

		// How many parts sit directly under one category. An older database may have no `part`
		// table at all, in which case fetchAll returns nothing and the honest answer is zero.
		int partCountOfType(SQLiteWrapper::SQLite& db, int typeId)
		{
			std::vector<std::vector<std::string>> rows = db.fetchAll(
				"SELECT COUNT(*) FROM part WHERE part_type_id=" + std::to_string(typeId) + ";");
			if (rows.empty() || rows.front().empty())
			{
				return 0;
			}
			return std::atoi(rows.front().front().c_str());
		}

		// True when `candidateAncestorId` is `typeId` itself or sits below it — i.e. making it
		// `typeId`'s parent would close a loop in parent_type_id.
		bool wouldCycle(const LocalTree& tree, int typeId, int candidateParentId)
		{
			std::unordered_set<int> visited;
			int current = candidateParentId;
			while (current != NoParentType && visited.insert(current).second)
			{
				if (current == typeId)
				{
					return true;
				}
				const PartType* type = tree.find(current);
				if (!type) { break; }
				current = type->parentTypeId;
			}
			return false;
		}

		void addConflict(TypeMergeEntry& entry, const std::string& fieldKey, const std::string& displayLabel,
			const std::string& localValue, const std::string& incomingValue)
		{
			if (localValue == incomingValue)
			{
				return;
			}
			FieldConflict conflict;
			conflict.fieldKey = fieldKey;
			conflict.displayLabel = displayLabel;
			conflict.localValue = localValue;
			conflict.incomingValue = incomingValue;
			conflict.resolution = FieldResolution::TakeIncoming;
			entry.conflicts.push_back(conflict);
		}

		// The tag with this name, creating it if the local vocabulary has none. Never a tag id
		// from the other database (§2d): ids are local, names are the thing the user manages.
		// A created tag gets a neutral colour — the bundle deliberately carries names only, and
		// inventing the source database's palette here would recolour tags nobody asked about.
		int ensureTagByName(SQLiteWrapper::SQLite& db, const std::string& name)
		{
			const std::string wanted = normalizedName(name);
			for (const Tag& tag : TagRepository::listTags(db))
			{
				if (normalizedName(tag.name) == wanted)
				{
					return tag.id;
				}
			}
			Tag created;
			created.name = trimmed(name);
			created.color = "#9E9E9E";
			return TagRepository::insertTag(db, created);
		}
	}

	std::vector<std::string> partTypePathOf(SQLiteWrapper::SQLite& db, int typeId)
	{
		return readLocalTree(db).pathOf(typeId);
	}

	PartTypeBundle bundleFromDatabase(SQLiteWrapper::SQLite& db, const std::vector<int>& typeIds)
	{
		PartTypeBundle bundle;
		bundle.formatVersion = CurrentBundleFormatVersion;
		bundle.sourceToolVersion = LibraryInfo::version.toString();
		bundle.exportedAt = nowIso8601();
		{
			const std::filesystem::path dbPath(db.getDBPath());
			bundle.sourceDatabaseName = dbPath.parent_path().filename().string();
		}

		const LocalTree tree = readLocalTree(db);

		// The selection, widened to every ancestor of every requested category: §2b makes a
		// subtype meaningless without the parent whose attributes it inherits, so a bundle that
		// held the child alone would simply not be importable anywhere.
		std::unordered_set<int> selected;
		if (typeIds.empty())
		{
			for (const PartType& type : tree.types)
			{
				selected.insert(type.id);
			}
		}
		else
		{
			for (int id : typeIds)
			{
				std::unordered_set<int> visited;
				int current = id;
				while (current != NoParentType && visited.insert(current).second)
				{
					const PartType* type = tree.find(current);
					if (!type) { break; }
					selected.insert(current);
					current = type->parentTypeId;
				}
			}
		}

		// Parents first, so an importer can resolve a parent path with one forward pass.
		std::unordered_set<int> emitted;
		std::vector<int> order;
		// `emitType`, not `emit`: Qt reserves that one too.
		std::function<void(int)> emitType = [&](int id)
		{
			if (selected.find(id) == selected.end() || emitted.find(id) != emitted.end())
			{
				return;
			}
			const PartType* type = tree.find(id);
			if (!type) { return; }
			emitted.insert(id);              // inserted before the recursion, so a cycle cannot loop
			if (type->parentTypeId != NoParentType)
			{
				emitType(type->parentTypeId);
			}
			order.push_back(id);
		};
		for (const PartType& type : tree.types)
		{
			emitType(type.id);
		}

		for (int id : order)
		{
			const PartType* local = tree.find(id);
			if (!local) { continue; }

			PartTypeNodeBundle node;
			node.path = tree.pathOf(id);
			node.type = *local;
			// Cleared on the way out, not just ignored on the way in: a bundle that still carried
			// them would round-trip differently from one that never had them, and the first thing
			// anyone would do with the numbers is trust them.
			node.type.id = NoParentType;
			node.type.parentTypeId = NoParentType;

			// OWN rows only (see file header) — effectiveAttributes() here would flatten §2b away.
			node.ownAttributes = PartTypeRepository::listOwnAttributes(db, id);
			for (PartTypeAttribute& attribute : node.ownAttributes)
			{
				attribute.id = 0;
				attribute.partTypeId = 0;
			}
			node.ownFileSlots = PartTypeRepository::listOwnFileSlots(db, id);
			for (PartTypeFileSlot& slot : node.ownFileSlots)
			{
				slot.id = 0;
				slot.partTypeId = 0;
			}
			node.ownListColumns = ListColumnRepository::listOwnColumns(db, id);
			for (PartTypeListColumn& column : node.ownListColumns)
			{
				column.id = NoListColumnId;
				column.partTypeId = 0;
			}
			for (const Tag& tag : TagRepository::listTypeDefaultTags(db, id))
			{
				node.defaultTagNames.push_back(tag.name);
			}

			bundle.nodes.push_back(node);
		}
		return bundle;
	}

	bool bundleFromDatabaseFile(const std::string& pmdbOrSqlitePath, PartTypeBundle& outBundle,
		std::string& outError)
	{
		outError.clear();
		namespace fs = std::filesystem;

		// §1a: every path in a database folder is a fixed sibling name resolved relative to
		// wherever the .pmdb entry file sits — the same rule DatabaseHandle::siblingPath() uses.
		fs::path given(pmdbOrSqlitePath);
		fs::path databaseFile = given;
		if (caseFolded(given.extension().string()) == ".pmdb")
		{
			databaseFile = given.parent_path() / "partmanager.db";
		}

		std::error_code errorCode;
		if (!fs::exists(databaseFile, errorCode))
		{
			outError = "No database file at " + databaseFile.string();
			return false;
		}

		// A second connection, which nothing in this codebase did before (see header). Scoped so
		// it is closed before this function returns however it returns.
		SQLiteWrapper::SQLite source(databaseFile.string());
		if (!source.open())
		{
			outError = "Failed to open " + databaseFile.string();
			return false;
		}
		// The whole point of this connection is that it reads. query_only makes that a property
		// of the connection rather than a promise about the code below it, so no future edit here
		// can write to somebody else's database by accident.
		source.execute("PRAGMA query_only=ON;");

		DatabaseMetadataValues values;
		if (!DatabaseMetadata::readDbMeta(source, values))
		{
			outError = databaseFile.string() + " is not a PartManager database (no db_meta table).";
			source.close();
			return false;
		}
		if (values.schemaVersion > CurrentSchemaVersion)
		{
			// §1c, unchanged in spirit: never read a structure newer than this build knows. Its
			// part_type tables may carry columns that would be silently dropped on the way in.
			outError = "This database was last saved by a newer PartManager (schema v"
				+ std::to_string(values.schemaVersion) + ", tool v" + values.toolVersionLastSaved
				+ ") — this copy only supports up to schema v" + std::to_string(CurrentSchemaVersion)
				+ ". Update PartManager to import from it.";
			source.close();
			return false;
		}
		if (!source.tableExists("part_type"))
		{
			outError = databaseFile.string() + " holds no component categories.";
			source.close();
			return false;
		}

		outBundle = bundleFromDatabase(source);
		if (outBundle.sourceDatabaseName.empty())
		{
			outBundle.sourceDatabaseName = given.stem().string();
		}
		source.close();
		return true;
	}

	MergePlan planMerge(SQLiteWrapper::SQLite& db, const PartTypeBundle& bundle)
	{
		MergePlan plan;
		const LocalTree tree = readLocalTree(db);

		// Both indexes are built once: full normalized path -> ids, and normalized leaf name -> ids.
		// The path index is keyed by the segment *list* — a std::map rather than an unordered_map,
		// because the key is a vector and the alternative would be joining it back into a string,
		// which is the whole bug (see header).
		std::map<std::vector<std::string>, std::vector<int>> idsByPath;
		std::unordered_map<std::string, std::vector<int>> idsByLeafName;
		std::unordered_map<int, std::vector<std::string>> pathById;
		for (const PartType& type : tree.types)
		{
			const std::vector<std::string> path = tree.pathOf(type.id);
			pathById[type.id] = path;
			idsByPath[normalizedPartTypePath(path)].push_back(type.id);
			idsByLeafName[normalizedName(type.name)].push_back(type.id);
		}

		std::unordered_set<int> claimed;

		for (const PartTypeNodeBundle& node : bundle.nodes)
		{
			TypeMergeEntry entry;
			entry.path = node.path;
			entry.action = TypeMergeAction::AddNew;

			const std::vector<std::string> wantedPath = normalizedPartTypePath(node.path);
			const std::string wantedLeaf = normalizedName(leafOfPath(node.path));

			// A local category already claimed by an earlier incoming one is out of the running:
			// two incoming categories merging into one local category would make the second
			// silently overwrite the first.
			auto unclaimed = [&](const std::vector<int>& ids)
			{
				std::vector<int> result;
				for (int id : ids)
				{
					if (claimed.find(id) == claimed.end()) { result.push_back(id); }
				}
				return result;
			};

			std::vector<int> candidates;
			auto pathHit = idsByPath.find(wantedPath);
			if (pathHit != idsByPath.end())
			{
				candidates = unclaimed(pathHit->second);
			}

			bool byLeafName = false;
			if (candidates.empty())
			{
				// Fallback (rule 2): the same name somewhere else in the tree. A flat `Resistor`
				// here and a `Passive/Resistor` there are one category, not two.
				auto leafHit = idsByLeafName.find(wantedLeaf);
				if (leafHit != idsByLeafName.end())
				{
					candidates = unclaimed(leafHit->second);
					byLeafName = true;
				}
			}

			if (candidates.size() == 1)
			{
				entry.action = TypeMergeAction::MergeInto;
				entry.localTypeId = candidates.front();
				claimed.insert(entry.localTypeId);
			}
			else if (candidates.size() > 1)
			{
				// Rule 3: never guess. DECISIONS.md 2026-09-26 — the fix for a bad guess is a
				// guess that knows it is unsure, because a wrongly merged category rewrites the
				// attribute template of parts that never belonged to it.
				std::string names;
				for (size_t i = 0; i < candidates.size(); ++i)
				{
					if (i > 0) { names += ", "; }
					names += displayPartTypePath(pathById[candidates[i]]);
				}
				plan.warnings.push_back("\"" + displayPartTypePath(node.path)
					+ "\" matches more than one local category ("
					+ names + ") " + (byLeafName ? "by name" : "by path")
					+ " — imported as a new category rather than guessing which one it is.");
			}

			if (entry.action == TypeMergeAction::MergeInto)
			{
				const PartType* local = tree.find(entry.localTypeId);
				const std::vector<std::string> localPath = pathById[entry.localTypeId];
				entry.affectedPartCount = partCountOfType(db, entry.localTypeId);

				// Every differing part_type scalar column becomes one reviewable row.
				addConflict(entry, "name", "Name", local->name, node.type.name);
				addConflict(entry, "domain", "Domain", local->domain, node.type.domain);
				addConflict(entry, "kicad_relevant", "KiCad relevant",
					boolText(local->kicadRelevant), boolText(node.type.kicadRelevant));
				addConflict(entry, "kicad_category", "KiCad category", local->kicadCategory, node.type.kicadCategory);
				addConflict(entry, "description", "Description", local->description, node.type.description);
				addConflict(entry, "search_keywords", "Search keywords", local->searchKeywords, node.type.searchKeywords);
				addConflict(entry, "excluded_keywords", "Excluded keywords", local->excludedKeywords, node.type.excludedKeywords);
				addConflict(entry, "name_template", "Name template", local->nameTemplate, node.type.nameTemplate);

				// OWN rows against OWN rows (§2b): an attribute the local type only *inherits* is
				// not a local row, so an incoming one of the same key is an added override here,
				// not a conflict — which is exactly what it will become once written.
				std::unordered_map<std::string, PartTypeAttribute> localAttributes;
				for (const PartTypeAttribute& attribute : PartTypeRepository::listOwnAttributes(db, entry.localTypeId))
				{
					localAttributes[caseFolded(attribute.key)] = attribute;
				}
				for (const PartTypeAttribute& incoming : node.ownAttributes)
				{
					auto hit = localAttributes.find(caseFolded(incoming.key));
					if (hit == localAttributes.end())
					{
						entry.addedAttributeKeys.push_back(incoming.key);
						if (incoming.required)
						{
							// §11: a required attribute arriving on a category that already holds
							// parts makes every one of them incomplete. On screen before OK, not after.
							entry.newRequiredAttributeKeys.push_back(incoming.key);
						}
						continue;
					}
					const PartTypeAttribute& localAttribute = hit->second;
					const std::string prefix = "attribute:" + incoming.key + ":";
					const std::string named = "Attribute \"" + incoming.key + "\"";
					addConflict(entry, prefix + "label", named + ": Label",
						localAttribute.label, incoming.label);
					addConflict(entry, prefix + "unit", named + ": Unit",
						localAttribute.unit, incoming.unit);
					addConflict(entry, prefix + "datatype", named + ": Data type",
						toString(localAttribute.datatype), toString(incoming.datatype));
					addConflict(entry, prefix + "enum_options", named + ": Options",
						joinOptions(localAttribute.enumOptions), joinOptions(incoming.enumOptions));
					addConflict(entry, prefix + "required", named + ": Required",
						boolText(localAttribute.required), boolText(incoming.required));
					addConflict(entry, prefix + "searchable", named + ": Searchable",
						boolText(localAttribute.searchable), boolText(incoming.searchable));
					addConflict(entry, prefix + "tooltip", named + ": Tooltip",
						localAttribute.tooltip, incoming.tooltip);

					if (localAttribute.datatype != incoming.datatype)
					{
						// The dangerous one. The values already sitting in part.attributes were
						// written under the old datatype and are not rewritten by this merge
						// (they are never touched at all — see applyMerge's invariant), so a
						// Text-to-Number change leaves numbers that were never numbers.
						plan.warnings.push_back(displayPartTypePath(localPath) + ": attribute \"" + incoming.key
							+ "\" changes data type from " + toString(localAttribute.datatype) + " to "
							+ toString(incoming.datatype) + "; " + std::to_string(entry.affectedPartCount)
							+ " part(s) already hold values written under the old data type.");
					}
				}

				std::unordered_map<std::string, PartTypeFileSlot> localSlots;
				for (const PartTypeFileSlot& slot : PartTypeRepository::listOwnFileSlots(db, entry.localTypeId))
				{
					localSlots[caseFolded(slot.role)] = slot;
				}
				for (const PartTypeFileSlot& incoming : node.ownFileSlots)
				{
					auto hit = localSlots.find(caseFolded(incoming.role));
					if (hit == localSlots.end())
					{
						continue;
					}
					const PartTypeFileSlot& localSlot = hit->second;
					const std::string prefix = "fileslot:" + incoming.role + ":";
					const std::string named = "File slot \"" + incoming.role + "\"";
					addConflict(entry, prefix + "label", named + ": Label", localSlot.label, incoming.label);
					addConflict(entry, prefix + "required", named + ": Required",
						boolText(localSlot.required), boolText(incoming.required));
					addConflict(entry, prefix + "tooltip", named + ": Tooltip",
						localSlot.tooltip, incoming.tooltip);
				}

				std::unordered_map<std::string, PartTypeListColumn> localColumns;
				for (const PartTypeListColumn& column : ListColumnRepository::listOwnColumns(db, entry.localTypeId))
				{
					localColumns[caseFolded(column.columnKey)] = column;
				}
				for (const PartTypeListColumn& incoming : node.ownListColumns)
				{
					auto hit = localColumns.find(caseFolded(incoming.columnKey));
					if (hit == localColumns.end())
					{
						continue;
					}
					const PartTypeListColumn& localColumn = hit->second;
					const std::string prefix = "listcolumn:" + incoming.columnKey + ":";
					const std::string named = "Column \"" + incoming.columnKey + "\"";
					addConflict(entry, prefix + "label_override", named + ": Label",
						localColumn.labelOverride, incoming.labelOverride);
					addConflict(entry, prefix + "visible", named + ": Visible",
						boolText(localColumn.visible), boolText(incoming.visible));
					addConflict(entry, prefix + "sort_order", named + ": Position",
						std::to_string(localColumn.sortOrder), std::to_string(incoming.sortOrder));
					addConflict(entry, prefix + "width_px", named + ": Width",
						std::to_string(localColumn.widthPx), std::to_string(incoming.widthPx));
				}

				// The re-parent this match implies, if any — see applyMerge() for why only a
				// deepening move is performed.
				const std::vector<std::string> incomingParent = parentOfPath(wantedPath);
				if (!incomingParent.empty() && normalizedPartTypePath(localPath) != wantedPath)
				{
					plan.warnings.push_back("\"" + displayPartTypePath(localPath)
						+ "\" is re-parented to \"" + displayPartTypePath(node.path)
						+ "\" — the same category, filed differently in the two databases.");
				}
			}

			plan.entries.push_back(entry);
		}

		for (const PartType& type : tree.types)
		{
			if (claimed.find(type.id) == claimed.end())
			{
				plan.unmatchedLocalTypeIds.push_back(type.id);
			}
		}
		return plan;
	}

	bool applyMerge(SQLiteWrapper::SQLite& db, const PartTypeBundle& bundle, const MergePlan& plan,
		std::string& outError)
	{
		outError.clear();

		// ------------------------------------------------------------------------------------
		// INVARIANT, and the load-bearing one: nothing below writes `part.attributes` and
		// nothing below changes any `part.part_type_id`. This merges *templates*, not parts.
		//
		// An attribute that this merge removes from a category leaves whatever the user typed
		// sitting in that part's `attributes` JSON — orphaned, unreferenced, and intact. That is
		// the deliberate choice: a value somebody typed is data, a category is only a description
		// of it, and an import that quietly deleted the first to tidy up the second would be
		// destroying work nobody asked it to touch. Re-filing parts onto other categories is a
		// separate, later, user-driven step.
		//
		// Every write here goes to part_type, part_type_attribute, part_type_file_slot,
		// part_type_list_column, tag and part_type_tag. No other table is opened for writing.
		// ------------------------------------------------------------------------------------

		Transaction transaction(db);
		if (!transaction.active())
		{
			outError = "Could not start a database transaction — the merge was not applied.";
			return false;
		}

		// Keyed by the normalized segment *list*, never by a joined string (see header).
		std::map<std::vector<std::string>, const TypeMergeEntry*> entryByPath;
		for (const TypeMergeEntry& entry : plan.entries)
		{
			entryByPath[normalizedPartTypePath(entry.path)] = &entry;
		}

		// Normalized path -> local id, seeded with the tree as it is now and extended as the
		// merge creates or re-files categories. One forward pass works because the bundle
		// guarantees parents precede children.
		LocalTree tree = readLocalTree(db);
		std::map<std::vector<std::string>, int> resolvedByPath;
		for (const PartType& type : tree.types)
		{
			const std::vector<std::string> path = normalizedPartTypePath(tree.pathOf(type.id));
			if (resolvedByPath.find(path) == resolvedByPath.end())
			{
				resolvedByPath[path] = type.id;
			}
		}

		for (const PartTypeNodeBundle& node : bundle.nodes)
		{
			const std::vector<std::string> nodePath = normalizedPartTypePath(node.path);
			// The whole category name, as the file spells it, for every message below: a user who
			// reads one of these has to be able to see what was actually in the bundle.
			const std::string nodeDisplay = displayPartTypePath(node.path);
			auto entryHit = entryByPath.find(nodePath);
			if (entryHit == entryByPath.end())
			{
				continue; // the plan says nothing about this category, so neither does this merge
			}
			const TypeMergeEntry& entry = *entryHit->second;
			if (entry.action == TypeMergeAction::Skip)
			{
				continue;
			}

			// Which fields of this entry the user left to the local database.
			std::unordered_set<std::string> keptLocal;
			for (const FieldConflict& conflict : entry.conflicts)
			{
				if (conflict.resolution == FieldResolution::KeepLocal)
				{
					keptLocal.insert(conflict.fieldKey);
				}
			}
			auto takeIncoming = [&](const std::string& fieldKey)
			{
				return keptLocal.find(fieldKey) == keptLocal.end();
			};

			const std::vector<std::string> parentPath = parentOfPath(nodePath);
			int parentId = NoParentType;
			if (!parentPath.empty())
			{
				auto parentHit = resolvedByPath.find(parentPath);
				if (parentHit == resolvedByPath.end())
				{
					// Both paths in full, and the parent's from the *incoming* names rather than
					// the normalized ones, so the user reads what the file actually said.
					outError = "Cannot import \"" + nodeDisplay
						+ "\": its parent category \""
						+ displayPartTypePath(parentOfPath(node.path))
						+ "\" is neither present locally nor part of this import.";
					return false;
				}
				parentId = parentHit->second;
			}

			int targetId = entry.localTypeId;

			if (entry.action == TypeMergeAction::AddNew)
			{
				PartType created = node.type;
				created.id = NoParentType;
				created.parentTypeId = parentId;
				targetId = PartTypeRepository::insertType(db, created);
				if (targetId == NoParentType)
				{
					outError = "Failed to create the category \"" + nodeDisplay + "\".";
					return false;
				}
			}
			else
			{
				PartType local;
				if (!PartTypeRepository::findType(db, targetId, local))
				{
					outError = "The local category this plan merges \"" + nodeDisplay
						+ "\" into no longer exists.";
					return false;
				}
				if (takeIncoming("name")) { local.name = node.type.name; }
				if (takeIncoming("domain")) { local.domain = node.type.domain; }
				if (takeIncoming("kicad_relevant")) { local.kicadRelevant = node.type.kicadRelevant; }
				if (takeIncoming("kicad_category")) { local.kicadCategory = node.type.kicadCategory; }
				if (takeIncoming("description")) { local.description = node.type.description; }
				if (takeIncoming("search_keywords")) { local.searchKeywords = node.type.searchKeywords; }
				if (takeIncoming("excluded_keywords")) { local.excludedKeywords = node.type.excludedKeywords; }
				if (takeIncoming("name_template")) { local.nameTemplate = node.type.nameTemplate; }

				// Re-parenting, and only ever *into* a parent the incoming side names. An incoming
				// root category never pulls a local one out of the tree it already sits in: the
				// flat-vs-nested case the fallback match exists for is "the other database knows
				// where this belongs", and the reverse — "the other database does not say" — is
				// not an instruction to flatten anything.
				if (parentId != NoParentType && parentId != local.parentTypeId)
				{
					if (wouldCycle(tree, local.id, parentId))
					{
						outError = "Cannot re-parent \"" + local.name
							+ "\": it would become its own ancestor. Nothing was imported.";
						return false;
					}
					local.parentTypeId = parentId;
					auto indexHit = tree.indexById.find(local.id);
					if (indexHit != tree.indexById.end())
					{
						tree.types[indexHit->second].parentTypeId = parentId;   // keep the cycle check honest
					}
				}
				if (!PartTypeRepository::updateType(db, local))
				{
					outError = "Failed to update the category \"" + local.name + "\".";
					return false;
				}
			}

			// --- attributes -------------------------------------------------------------
			std::unordered_map<std::string, PartTypeAttribute> localAttributes;
			for (const PartTypeAttribute& attribute : PartTypeRepository::listOwnAttributes(db, targetId))
			{
				localAttributes[caseFolded(attribute.key)] = attribute;
			}
			for (const PartTypeAttribute& incoming : node.ownAttributes)
			{
				auto hit = localAttributes.find(caseFolded(incoming.key));
				if (hit == localAttributes.end())
				{
					PartTypeAttribute created = incoming;
					created.id = 0;
					created.partTypeId = targetId;
					// Through the repository, never a raw INSERT: this is what runs
					// ensureAttrColumn() so a searchable numeric attribute's attr_<key>
					// fast-filter column exists before any part of this category is saved (§2).
					if (PartTypeRepository::insertAttribute(db, created) == 0)
					{
						outError = "Failed to add the attribute \"" + incoming.key + "\" to \""
							+ nodeDisplay + "\".";
						return false;
					}
					continue;
				}
				PartTypeAttribute merged = hit->second;
				const std::string prefix = "attribute:" + incoming.key + ":";
				if (takeIncoming(prefix + "label")) { merged.label = incoming.label; }
				if (takeIncoming(prefix + "unit")) { merged.unit = incoming.unit; }
				if (takeIncoming(prefix + "datatype")) { merged.datatype = incoming.datatype; }
				if (takeIncoming(prefix + "enum_options")) { merged.enumOptions = incoming.enumOptions; }
				if (takeIncoming(prefix + "required")) { merged.required = incoming.required; }
				if (takeIncoming(prefix + "searchable")) { merged.searchable = incoming.searchable; }
				if (takeIncoming(prefix + "tooltip")) { merged.tooltip = incoming.tooltip; }
				if (!PartTypeRepository::updateAttribute(db, merged))
				{
					outError = "Failed to update the attribute \"" + incoming.key + "\" on \""
						+ nodeDisplay + "\".";
					return false;
				}
			}

			// --- file slots -------------------------------------------------------------
			std::unordered_map<std::string, PartTypeFileSlot> localSlots;
			for (const PartTypeFileSlot& slot : PartTypeRepository::listOwnFileSlots(db, targetId))
			{
				localSlots[caseFolded(slot.role)] = slot;
			}
			for (const PartTypeFileSlot& incoming : node.ownFileSlots)
			{
				auto hit = localSlots.find(caseFolded(incoming.role));
				if (hit == localSlots.end())
				{
					PartTypeFileSlot created = incoming;
					created.id = 0;
					created.partTypeId = targetId;
					if (PartTypeRepository::insertFileSlot(db, created) == 0)
					{
						outError = "Failed to add the file slot \"" + incoming.role + "\" to \""
							+ nodeDisplay + "\".";
						return false;
					}
					continue;
				}
				PartTypeFileSlot merged = hit->second;
				const std::string prefix = "fileslot:" + incoming.role + ":";
				if (takeIncoming(prefix + "label")) { merged.label = incoming.label; }
				if (takeIncoming(prefix + "required")) { merged.required = incoming.required; }
				if (takeIncoming(prefix + "tooltip")) { merged.tooltip = incoming.tooltip; }
				if (!PartTypeRepository::updateFileSlot(db, merged))
				{
					outError = "Failed to update the file slot \"" + incoming.role + "\" on \""
						+ nodeDisplay + "\".";
					return false;
				}
			}

			// --- list columns (§7b) ------------------------------------------------------
			// saveColumns() replaces a type's whole layout, so the union is assembled here first:
			// a local column the bundle says nothing about is part of the user's saved layout and
			// is not something an import may drop.
			if (!node.ownListColumns.empty())
			{
				std::vector<PartTypeListColumn> merged = ListColumnRepository::listOwnColumns(db, targetId);
				for (const PartTypeListColumn& incoming : node.ownListColumns)
				{
					const std::string prefix = "listcolumn:" + incoming.columnKey + ":";
					auto existing = std::find_if(merged.begin(), merged.end(),
						[&](const PartTypeListColumn& column)
						{
							return caseFolded(column.columnKey) == caseFolded(incoming.columnKey);
						});
					if (existing == merged.end())
					{
						PartTypeListColumn created = incoming;
						created.id = NoListColumnId;
						created.partTypeId = targetId;
						merged.push_back(created);
						continue;
					}
					if (takeIncoming(prefix + "label_override")) { existing->labelOverride = incoming.labelOverride; }
					if (takeIncoming(prefix + "visible")) { existing->visible = incoming.visible; }
					if (takeIncoming(prefix + "sort_order")) { existing->sortOrder = incoming.sortOrder; }
					if (takeIncoming(prefix + "width_px")) { existing->widthPx = incoming.widthPx; }
				}
				// saveColumns() takes sort_order from the vector's order, so the order has to be
				// made real before it is written.
				std::stable_sort(merged.begin(), merged.end(),
					[](const PartTypeListColumn& a, const PartTypeListColumn& b)
					{
						return a.sortOrder < b.sortOrder;
					});
				if (!ListColumnRepository::saveColumns(db, targetId, merged))
				{
					outError = "Failed to write the table layout of \"" + nodeDisplay + "\".";
					return false;
				}
			}

			// --- default tags (§2d), by name --------------------------------------------
			for (const std::string& tagName : node.defaultTagNames)
			{
				if (trimmed(tagName).empty())
				{
					continue;
				}
				const int tagId = ensureTagByName(db, tagName);
				if (tagId == NoTagId)
				{
					outError = "Failed to create the tag \"" + tagName + "\".";
					return false;
				}
				if (!TagRepository::addTypeDefaultTag(db, targetId, tagId))
				{
					outError = "Failed to attach the tag \"" + tagName + "\" to \"" + nodeDisplay + "\".";
					return false;
				}
			}

			resolvedByPath[nodePath] = targetId;
		}

		if (!transaction.commit())
		{
			outError = "Failed to commit the category merge — nothing was changed.";
			return false;
		}
		return true;
	}

#endif

}
