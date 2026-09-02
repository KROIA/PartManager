#include "model3d/PartManager_MeshBounds.h"
#include "model3d/PartManager_Model3DFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace PartManager
{
	namespace
	{
		// Accumulates one vertex into the box, opening it on the first one so an all-negative
		// mesh does not come out containing the origin.
		void take(MeshBounds& bounds, double x, double y, double z)
		{
			if (!bounds.ok)
			{
				bounds.ok = true;
				bounds.minX = bounds.maxX = x;
				bounds.minY = bounds.maxY = y;
				bounds.minZ = bounds.maxZ = z;
				return;
			}
			bounds.minX = std::min(bounds.minX, x);
			bounds.maxX = std::max(bounds.maxX, x);
			bounds.minY = std::min(bounds.minY, y);
			bounds.maxY = std::max(bounds.maxY, y);
			bounds.minZ = std::min(bounds.minZ, z);
			bounds.maxZ = std::max(bounds.maxZ, z);
		}

		float littleEndianFloat(const unsigned char* p)
		{
			// STL is little-endian by specification. Assembled by hand rather than memcpy'd so
			// this stays correct if it is ever built somewhere big-endian, where a straight copy
			// would silently produce a box that is merely wrong rather than obviously so.
			const std::uint32_t bits = static_cast<std::uint32_t>(p[0])
				| (static_cast<std::uint32_t>(p[1]) << 8)
				| (static_cast<std::uint32_t>(p[2]) << 16)
				| (static_cast<std::uint32_t>(p[3]) << 24);
			float value = 0.0f;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		}

		// The only reliable way to tell the two STL flavours apart. "Does it start with `solid`"
		// is the usual test and it is wrong: FreeCAD writes binary files whose 80-byte header is
		// free text, and plenty of exporters put "solid" in it. The length arithmetic cannot lie.
		bool looksBinary(const std::string& data)
		{
			if (data.size() < 84)
			{
				return false;
			}
			const std::uint32_t triangles = static_cast<std::uint32_t>(
				static_cast<unsigned char>(data[80]))
				| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[81])) << 8)
				| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[82])) << 16)
				| (static_cast<std::uint32_t>(static_cast<unsigned char>(data[83])) << 24);
			return data.size() == 84u + 50ull * triangles;
		}

		MeshBounds fromBinaryStl(const std::string& data)
		{
			MeshBounds bounds;
			const std::uint32_t triangles =
				static_cast<std::uint32_t>((data.size() - 84) / 50);
			const unsigned char* base =
				reinterpret_cast<const unsigned char*>(data.data());
			for (std::uint32_t i = 0; i < triangles; ++i)
			{
				// 50 bytes each: a normal we do not want, three vertices, two attribute bytes.
				const unsigned char* record = base + 84 + 50ull * i + 12;
				for (int vertex = 0; vertex < 3; ++vertex)
				{
					const unsigned char* p = record + 12 * vertex;
					take(bounds, littleEndianFloat(p), littleEndianFloat(p + 4),
						littleEndianFloat(p + 8));
				}
			}
			return bounds;
		}

		MeshBounds fromTextVertices(const std::string& data, const std::string& keyword)
		{
			// One scan for both ASCII STL (`vertex x y z`) and OBJ (`v x y z`); the only thing
			// that differs is the token. Anything that is not three numbers after it is skipped
			// rather than guessed at — OBJ's `vt` and `vn` start with the same letter.
			MeshBounds bounds;
			std::istringstream stream(data);
			std::string line;
			while (std::getline(stream, line))
			{
				std::istringstream fields(line);
				std::string token;
				if (!(fields >> token) || token != keyword)
				{
					continue;
				}
				double x = 0.0, y = 0.0, z = 0.0;
				if (fields >> x >> y >> z)
				{
					take(bounds, x, y, z);
				}
			}
			return bounds;
		}
	}

	double MeshBounds::radius() const
	{
		if (!ok)
		{
			return 0.0;
		}
		const double half = 0.5 * std::sqrt(sizeX() * sizeX() + sizeY() * sizeY()
			+ sizeZ() * sizeZ());
		// A flat or degenerate mesh still needs a distance to be framed from, and a zero radius
		// would put the camera exactly on it.
		return half > 1e-6 ? half : 1.0;
	}

	MeshBounds meshBoundsOf(const std::string& path)
	{
		const Model3DFormat format = model3DFormatOf(path);
		if (format != Model3DFormat::Stl && format != Model3DFormat::Obj)
		{
			return MeshBounds();
		}

		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			return MeshBounds();
		}
		const std::string data((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		if (data.empty())
		{
			return MeshBounds();
		}

		if (format == Model3DFormat::Obj)
		{
			return fromTextVertices(data, "v");
		}
		return looksBinary(data) ? fromBinaryStl(data) : fromTextVertices(data, "vertex");
	}

}
