#pragma once

#include "UnitTest.h"
#include "model3d/PartManager_MeshBounds.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

// §13's mesh measuring, which is what decides where the camera goes. The quiet mistake here is
// the STL flavour test: "does it start with `solid`" is the usual one and it is wrong for exactly
// the files this app produces, and getting it wrong reads 26 KB of binary triangles as text and
// reports an empty box — a viewer that frames nothing, with nothing on screen to say why.
class TST_MeshBounds : public UnitTest::Test
{
	TEST_CLASS(TST_MeshBounds)
public:
	TST_MeshBounds()
		: Test("TST_MeshBounds")
	{
		ADD_TEST(TST_MeshBounds::binaryStlIsMeasuredNotMistakenForText);
		ADD_TEST(TST_MeshBounds::asciiStlAndObjAreRead);
		ADD_TEST(TST_MeshBounds::whatCannotBeMeasuredSaysSo);
	}

private:

	// Not `near`: windows.h #defines it (a 16-bit-memory-model leftover), and the error it
	// produces points at this line rather than at the macro.
	// An STL stores floats, so a value that reads 2.4 in the file arrives as 2.4000000953674316.
	// Comparing those exactly is a test that fails for being right.
	static bool roughly(double actual, double expected)
	{
		return std::abs(actual - expected) < 1e-5;
	}

	static std::filesystem::path scratch()
	{
		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / "PartManager_TST_MeshBounds";
		std::filesystem::create_directories(folder);
		return folder;
	}

	static void write(const std::filesystem::path& path, const std::string& bytes)
	{
		std::ofstream out(path, std::ios::binary);
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	}

	static void appendFloat(std::string& out, float value)
	{
		std::uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		for (int shift = 0; shift < 32; shift += 8)
		{
			out += static_cast<char>((bits >> shift) & 0xFF);
		}
	}

	// One triangle, in the 50-byte record a binary STL uses.
	static void appendTriangle(std::string& out, const float (&a)[3], const float (&b)[3],
		const float (&c)[3])
	{
		for (int i = 0; i < 3; ++i) { appendFloat(out, 0.0f); }   // the normal, unused here
		for (const float* vertex : { a, b, c })
		{
			appendFloat(out, vertex[0]);
			appendFloat(out, vertex[1]);
			appendFloat(out, vertex[2]);
		}
		out += '\0';
		out += '\0';
	}

	// Tests

	TEST_FUNCTION(binaryStlIsMeasuredNotMistakenForText)
	{
		TEST_START;

		// FreeCAD's own header, which is what PartManager's converted meshes actually carry —
		// 80 bytes of free text that happens not to be "solid", followed by the triangle count.
		std::string header = "MESH-MESH-MESH-MESH";
		header.resize(80, '-');
		std::string file = header;
		file += static_cast<char>(2);   // two triangles, little-endian uint32
		file += '\0'; file += '\0'; file += '\0';

		const float a[3] = { -1.2f, -1.45f, 0.0f };
		const float b[3] = { 1.2f, 1.45f, 1.2f };
		const float c[3] = { 0.0f, 0.0f, 0.6f };
		appendTriangle(file, a, b, c);
		appendTriangle(file, c, b, a);

		const std::filesystem::path path = scratch() / "binary.stl";
		write(path, file);

		const PartManager::MeshBounds bounds = PartManager::meshBoundsOf(path.string());
		TEST_ASSERT_M(bounds.ok, "a binary STL with a non-'solid' header must still be measured");
		// The real 2N7002 numbers: an SOT-23 sitting on the board plane.
		TEST_ASSERT(roughly(bounds.minX, -1.2));
		TEST_ASSERT(roughly(bounds.maxY, 1.45));
		TEST_ASSERT_M(roughly(bounds.minZ, 0.0),
			"KiCad models sit on z = 0; the board is drawn there and nowhere else");
		TEST_ASSERT(roughly(bounds.sizeX(), 2.4));
		TEST_ASSERT(roughly(bounds.sizeZ(), 1.2));
		TEST_ASSERT(roughly(bounds.centreZ(), 0.6));

		// The radius is what the camera distance is computed from, so a wrong one is a part that
		// fills the panel or vanishes in it. Half the diagonal of 2.4 x 2.9 x 1.2.
		TEST_ASSERT(roughly(bounds.radius(),
			0.5 * std::sqrt(2.4 * 2.4 + 2.9 * 2.9 + 1.2 * 1.2)));

		// A header that *does* say "solid" must not flip the file to the text reader — the
		// length arithmetic is the test, not the first five bytes.
		std::string lying = file;
		lying.replace(0, 5, "solid");
		const std::filesystem::path liar = scratch() / "liar.stl";
		write(liar, lying);
		const PartManager::MeshBounds still = PartManager::meshBoundsOf(liar.string());
		TEST_ASSERT_M(still.ok, "a binary STL whose header begins with 'solid' is still binary");
		TEST_ASSERT(roughly(still.sizeX(), 2.4));
	}

	TEST_FUNCTION(asciiStlAndObjAreRead)
	{
		TEST_START;

		const std::filesystem::path ascii = scratch() / "ascii.stl";
		write(ascii,
			"solid part\n"
			"  facet normal 0 0 1\n"
			"    outer loop\n"
			"      vertex 0 0 0\n"
			"      vertex 3 0 0\n"
			"      vertex 0 4 2\n"
			"    endloop\n"
			"  endfacet\n"
			"endsolid part\n");
		const PartManager::MeshBounds stl = PartManager::meshBoundsOf(ascii.string());
		TEST_ASSERT(stl.ok);
		TEST_COMPARE(stl.sizeX(), 3.0);
		TEST_COMPARE(stl.sizeY(), 4.0);
		TEST_COMPARE(stl.sizeZ(), 2.0);

		const std::filesystem::path obj = scratch() / "part.obj";
		write(obj,
			"# a comment\n"
			"v -1 -1 -1\n"
			"vt 0.5 0.5\n"          // must not be counted: same first letter, not a vertex
			"vn 0 0 1\n"
			"v 1 2 3\n"
			"f 1 2 1\n");
		const PartManager::MeshBounds wavefront = PartManager::meshBoundsOf(obj.string());
		TEST_ASSERT(wavefront.ok);
		TEST_COMPARE(wavefront.minX, -1.0);
		TEST_COMPARE(wavefront.maxZ, 3.0);
		TEST_ASSERT_M(std::abs(wavefront.maxY - 2.0) < 1e-9,
			"a texture coordinate must not widen the box");

		// An all-negative mesh must not come out containing the origin, which is what an
		// unopened box zero-initialised to (0,0,0) would do.
		const std::filesystem::path away = scratch() / "away.obj";
		write(away, "v -10 -10 -10\nv -8 -9 -7\n");
		const PartManager::MeshBounds offset = PartManager::meshBoundsOf(away.string());
		TEST_ASSERT(offset.ok);
		TEST_COMPARE(offset.maxX, -8.0);
		TEST_COMPARE(offset.centreY(), -9.5);
	}

	TEST_FUNCTION(whatCannotBeMeasuredSaysSo)
	{
		TEST_START;

		// Not a failure — the viewer falls back to a fixed standoff. What matters is that it is
		// reported rather than returned as a box of zeroes the camera would then divide by.
		TEST_ASSERT(!PartManager::meshBoundsOf("").ok);
		TEST_ASSERT(!PartManager::meshBoundsOf("C:/nowhere/model.stl").ok);
		TEST_COMPARE(PartManager::meshBoundsOf("C:/nowhere/model.stl").radius(), 0.0);

		const std::filesystem::path ply = scratch() / "model.ply";
		write(ply, "ply\nformat ascii 1.0\n");
		TEST_ASSERT_M(!PartManager::meshBoundsOf(ply.string()).ok,
			"PLY is renderable but not measured here; it must say so rather than guess");

		const std::filesystem::path empty = scratch() / "empty.stl";
		write(empty, "");
		TEST_ASSERT(!PartManager::meshBoundsOf(empty.string()).ok);

		// A file with the right extension and no vertices in it at all.
		const std::filesystem::path blank = scratch() / "blank.obj";
		write(blank, "# nothing but comments\n");
		TEST_ASSERT(!PartManager::meshBoundsOf(blank.string()).ok);
	}
};

TEST_INSTANTIATE(TST_MeshBounds);
