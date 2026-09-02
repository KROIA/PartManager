#pragma once

#include "UnitTest.h"
#include "model3d/PartManager_Model3DFormat.h"
#include <algorithm>

// §13's format table. Small, but it is the thing that decides whether a file the user attached is
// *stored and unusable* or *stored and simply not drawable here* — and those must never be
// confused, because a KiCad STEP model is the second and looks like the first.
class TST_Model3DFormat : public UnitTest::Test
{
	TEST_CLASS(TST_Model3DFormat)
public:
	TST_Model3DFormat()
		: Test("TST_Model3DFormat")
	{
		ADD_TEST(TST_Model3DFormat::extensionsMapCaseInsensitively);
		ADD_TEST(TST_Model3DFormat::stepIsAModelButNotARenderableOne);
		ADD_TEST(TST_Model3DFormat::everyListedExtensionIsRecognised);
	}

private:

	TEST_FUNCTION(extensionsMapCaseInsensitively)
	{
		TEST_START;

		TEST_ASSERT(PartManager::model3DFormatOf("cube.obj") == PartManager::Model3DFormat::Obj);
		TEST_ASSERT(PartManager::model3DFormatOf("CUBE.OBJ") == PartManager::Model3DFormat::Obj);
		TEST_ASSERT(PartManager::model3DFormatOf("C:/lib/3d/R_0603.StL")
			== PartManager::Model3DFormat::Stl);
		// Both spellings KiCad actually writes.
		TEST_ASSERT(PartManager::model3DFormatOf("SOIC-8.step") == PartManager::Model3DFormat::Step);
		TEST_ASSERT(PartManager::model3DFormatOf("SOIC-8.stp") == PartManager::Model3DFormat::Step);
		// KiCad's board-viewer model, which sits next to the .step in every one of its libraries.
		TEST_ASSERT(PartManager::model3DFormatOf("SOIC-8.wrl") == PartManager::Model3DFormat::Vrml);

		TEST_ASSERT(PartManager::model3DFormatOf("datasheet.pdf") == PartManager::Model3DFormat::Unknown);
		TEST_ASSERT(PartManager::model3DFormatOf("README") == PartManager::Model3DFormat::Unknown);
		// A dot in a folder name is not an extension — "C:/v1.2/model" has none.
		TEST_ASSERT(PartManager::model3DFormatOf("C:/v1.2/model") == PartManager::Model3DFormat::Unknown);
		TEST_ASSERT(PartManager::model3DFormatOf("") == PartManager::Model3DFormat::Unknown);
	}

	TEST_FUNCTION(stepIsAModelButNotARenderableOne)
	{
		TEST_START;

		// The distinction this whole header exists for. A STEP file is exactly what §5a's KiCad
		// export needs, so it must be storable; it is a NURBS boundary representation, so it
		// cannot be drawn by a mesh loader. Collapsing the two would either refuse the file the
		// user most wants to keep, or open a blank viewer and call it success.
		TEST_ASSERT(PartManager::isModel3D(PartManager::Model3DFormat::Step));
		TEST_ASSERT(!PartManager::isRenderableModel3D(PartManager::Model3DFormat::Step));
		TEST_ASSERT(PartManager::isModel3D(PartManager::Model3DFormat::Vrml));
		TEST_ASSERT(!PartManager::isRenderableModel3D(PartManager::Model3DFormat::Vrml));
		TEST_ASSERT(PartManager::isModel3D(PartManager::Model3DFormat::Iges));
		TEST_ASSERT(!PartManager::isRenderableModel3D(PartManager::Model3DFormat::Iges));

		// What Qt3D's own geometry loaders read, and nothing else.
		TEST_ASSERT(PartManager::isRenderableModel3D(PartManager::Model3DFormat::Obj));
		TEST_ASSERT(PartManager::isRenderableModel3D(PartManager::Model3DFormat::Stl));
		TEST_ASSERT(PartManager::isRenderableModel3D(PartManager::Model3DFormat::Ply));
		TEST_ASSERT(PartManager::isRenderableModel3D(PartManager::Model3DFormat::Gltf));

		// Unknown is neither, which is what makes "attach it anyway?" a question worth asking.
		TEST_ASSERT(!PartManager::isModel3D(PartManager::Model3DFormat::Unknown));
		TEST_ASSERT(!PartManager::isRenderableModel3D(PartManager::Model3DFormat::Unknown));

		TEST_COMPARE(PartManager::model3DFormatName(PartManager::Model3DFormat::Step),
			std::string("STEP"));
		TEST_ASSERT(PartManager::model3DFormatName(PartManager::Model3DFormat::Unknown).empty());
	}

	TEST_FUNCTION(everyListedExtensionIsRecognised)
	{
		TEST_START;

		// The file dialog's filter is built from this list, so an entry model3DFormatOf() does
		// not know would let the user pick a file the attach path then questions.
		const std::vector<std::string> extensions = PartManager::model3DExtensions();
		TEST_ASSERT(!extensions.empty());
		for (const std::string& extension : extensions)
		{
			TEST_ASSERT_M(PartManager::isModel3D(PartManager::model3DFormatOf("model." + extension)),
				"the dialog offers ." + extension + " but the format table does not know it");
		}

		// Renderable first, so a dialog's default filter offers what will actually draw.
		TEST_COMPARE(extensions.front(), std::string("obj"));
		TEST_ASSERT_M(std::find(extensions.begin(), extensions.end(), std::string("step"))
			!= extensions.end(), "STEP must be attachable even though it cannot be drawn");
	}
};

TEST_INSTANTIATE(TST_Model3DFormat);
