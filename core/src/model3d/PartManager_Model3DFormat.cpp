#include "model3d/PartManager_Model3DFormat.h"
#include "PartManager_global.h"

namespace PartManager
{

	namespace
	{
		// The extension after the last dot, lowercased. Returns empty for a name with no dot, or
		// one whose dot is inside a folder name ("C:/a.b/model" has no extension).
		std::string extensionOf(const std::string& path)
		{
			const size_t dot = path.find_last_of('.');
			if (dot == std::string::npos)
			{
				return std::string();
			}
			const size_t separator = path.find_last_of("/\\");
			if (separator != std::string::npos && dot < separator)
			{
				return std::string();
			}
			std::string extension = path.substr(dot + 1);
			for (char& c : extension)
			{
				if (c >= 'A' && c <= 'Z')
				{
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return extension;
		}
	}

	Model3DFormat model3DFormatOf(const std::string& fileNameOrPath)
	{
		const std::string extension = extensionOf(fileNameOrPath);
		if (extension == "obj")                                { return Model3DFormat::Obj; }
		if (extension == "stl")                                { return Model3DFormat::Stl; }
		if (extension == "ply")                                { return Model3DFormat::Ply; }
		if (extension == "gltf" || extension == "glb")         { return Model3DFormat::Gltf; }
		// KiCad writes ".step" from the mechanical exporter and ".stp" is the same thing under
		// the older 8.3-era name; both appear in real libraries.
		if (extension == "step" || extension == "stp")         { return Model3DFormat::Step; }
		if (extension == "wrl" || extension == "vrml"
			|| extension == "x3d")                             { return Model3DFormat::Vrml; }
		if (extension == "iges" || extension == "igs")         { return Model3DFormat::Iges; }
		return Model3DFormat::Unknown;
	}

	bool isRenderableModel3D(Model3DFormat format)
	{
		// Exactly what Qt3D's own geometry loaders read. Deliberately not "everything we
		// recognise": a viewer that opens a STEP file and shows nothing is worse than one that
		// says it cannot draw it.
		switch (format)
		{
		case Model3DFormat::Obj:
		case Model3DFormat::Stl:
		case Model3DFormat::Ply:
		case Model3DFormat::Gltf:
			return true;
		default:
			return false;
		}
	}

	bool isModel3D(Model3DFormat format)
	{
		return format != Model3DFormat::Unknown;
	}

	std::string model3DFormatName(Model3DFormat format)
	{
		switch (format)
		{
		case Model3DFormat::Obj:  return "OBJ";
		case Model3DFormat::Stl:  return "STL";
		case Model3DFormat::Ply:  return "PLY";
		case Model3DFormat::Gltf: return "glTF";
		case Model3DFormat::Step: return "STEP";
		case Model3DFormat::Vrml: return "VRML";
		case Model3DFormat::Iges: return "IGES";
		default:                  return std::string();
		}
	}

	std::vector<std::string> model3DExtensions()
	{
		// Renderable first, so a file dialog's default filter offers what will actually draw.
		return { "obj", "stl", "ply", "gltf", "glb", "step", "stp", "wrl", "x3d", "iges", "igs" };
	}

}
