#include "kicad/PartManager_FootprintVariants.h"

#include <algorithm>
#include <map>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "filestore/PartManager_FileStore.h"
	#include "persistence/PartManager_PartRepository.h"
#endif

namespace PartManager
{

	int FootprintPackageGroup::partCount() const
	{
		int count = 0;
		for (const FootprintVariant& variant : variants)
		{
			count += static_cast<int>(variant.parts.size());
		}
		return count;
	}

	std::vector<FootprintPackageGroup> FootprintVariants::group(
		const std::vector<FootprintPartRef>& refs)
	{
		// std::map at both levels, so the output is ordered by package and the variants under a
		// package start from a fixed order before the count sort below reorders them. An
		// unordered container here would hand the screen a different row order every run.
		std::map<std::string, std::map<std::string, FootprintVariant>> byPackage;
		for (const FootprintPartRef& ref : refs)
		{
			if (ref.contentHash.empty())
			{
				// No hash means no attached footprint to compare — nothing to decide about.
				continue;
			}
			FootprintVariant& variant = byPackage[ref.package][ref.contentHash];
			variant.contentHash = ref.contentHash;
			// First one wins: every part in a variant holds the same bytes by definition, so any
			// of their paths reads the same file.
			if (variant.relativePath.empty())
			{
				variant.relativePath = ref.relativePath;
			}
			variant.parts.push_back(ref);
		}

		std::vector<FootprintPackageGroup> groups;
		groups.reserve(byPackage.size());
		for (std::pair<const std::string, std::map<std::string, FootprintVariant>>& entry : byPackage)
		{
			FootprintPackageGroup group;
			group.package = entry.first;
			for (std::pair<const std::string, FootprintVariant>& variantEntry : entry.second)
			{
				FootprintVariant variant = variantEntry.second;
				std::sort(variant.parts.begin(), variant.parts.end(),
					[](const FootprintPartRef& a, const FootprintPartRef& b)
					{
						// By id under an equal name, so two parts a user gave the same name to
						// still order deterministically.
						return a.partName == b.partName ? a.partId < b.partId
							: a.partName < b.partName;
					});
				group.variants.push_back(variant);
			}
			std::sort(group.variants.begin(), group.variants.end(),
				[](const FootprintVariant& a, const FootprintVariant& b)
				{
					// Most-used first — see the header. The hash breaks the tie rather than the
					// insertion order, which would depend on how the parts were listed.
					return a.parts.size() == b.parts.size()
						? a.contentHash < b.contentHash
						: a.parts.size() > b.parts.size();
				});
			groups.push_back(group);
		}
		return groups;
	}

	std::vector<FootprintPackageGroup> FootprintVariants::inconsistentOnly(
		const std::vector<FootprintPackageGroup>& groups)
	{
		std::vector<FootprintPackageGroup> out;
		for (const FootprintPackageGroup& group : groups)
		{
			if (!group.consistent())
			{
				out.push_back(group);
			}
		}
		return out;
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	std::vector<FootprintPartRef> FootprintVariants::collect(SQLiteWrapper::SQLite& db)
	{
		std::vector<FootprintPartRef> refs;
		for (const Part& part : PartRepository::listParts(db))
		{
			PartFile file;
			if (!FileStore::roleFile(db, part.id, PartFileRole::KicadFootprint, file))
			{
				continue;
			}
			FootprintPartRef ref;
			ref.partId = part.id;
			ref.partName = part.name;
			ref.package = part.package;
			ref.contentHash = file.contentHash;
			ref.relativePath = file.relativePath;
			refs.push_back(ref);
		}
		return refs;
	}

#endif

}
