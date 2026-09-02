// @file PartManager_EasyEdaConverter.h
// @brief EasyEDA shape lines -> KiCad `.kicad_sym` / `.kicad_mod` (§5c). Pure text, no network.
//
// EasyEDA stores geometry as tilde-delimited strings, one per shape, with a
// leading token naming the kind. Two coordinate systems, both with **Y pointing
// down** and both with the origin at the document's head x/y:
//
//     schematic   1 unit = 10 mil = 0.254 mm   (pins land on a 10-unit grid)
//     PCB         1 unit =  1 mil = 0.0254 mm
//
// KiCad symbols measure in mm with **Y up**, footprints in mm with Y down — the
// same asymmetry KicadGeometry already documents. So symbol Y is negated and
// footprint Y is not, which is the single most likely thing to be wrong here and
// the reason both directions are asserted in the tests.
//
// **What is converted, and what is not.** Symbols: rectangles, ellipses,
// polylines/polygons and pins. Footprints: pads, tracks, circles and arcs.
// Everything else — schematic text, `SOLIDREGION` copper pours, and the
// `SVGNODE` 3D model outline — is counted and named in `skipped` rather than
// approximated. A conversion that quietly dropped a shape would produce a
// footprint that looks complete and is not, which on a board is expensive; the
// caller shows the list.
//
// Nothing here touches the filesystem or the network, so every rule below is
// tested against real EasyEDA payloads captured as fixtures.
// @see docs/design/ARCHITECTURE.md §5a, §5c
// @see PartManager_EasyEdaClient.h, PartManager_KicadSymbolWriter.h
#pragma once

#include "PartManager_global.h"
#include "easyeda/PartManager_EasyEdaClient.h"
#include <string>
#include <vector>

namespace PartManager
{

	struct PART_MANAGER_API EasyEdaConversion
	{
		bool ok = false;
		std::string errorMessage;

		std::string symbolLibraryText;   // a complete `.kicad_sym`, empty when there was no symbol
		std::string footprintText;       // a complete `.kicad_mod`, empty when there was no package
		std::string symbolName;
		std::string footprintName;

		int pinCount = 0;
		int padCount = 0;
		// Shape kinds that were recognised as EasyEDA's but not converted, e.g. "SOLIDREGION x4".
		// Empty means everything in the payload made it across.
		std::vector<std::string> skipped;

		bool hasSymbol() const { return !symbolLibraryText.empty(); }
		bool hasFootprint() const { return !footprintText.empty(); }
	};

	class PART_MANAGER_API EasyEdaConverter
	{
		EasyEdaConverter() = delete;
	public:
		// The whole component in one step. Never throws; a component with neither a usable symbol
		// nor a usable footprint comes back with ok == false and a reason.
		static EasyEdaConversion convert(const EasyEdaComponent& component);

		// One `(symbol ...)` block, indented to sit inside a `(kicad_symbol_lib ...)`. Public so
		// the symbol half can be asserted on without a footprint in the fixture.
		static std::string symbolBlock(const EasyEdaComponent& component, const std::string& name,
			int& outPinCount, std::vector<std::string>& outSkipped);

		// A complete `.kicad_mod` file.
		static std::string footprintFile(const EasyEdaComponent& component, const std::string& name,
			int& outPadCount, std::vector<std::string>& outSkipped);

		// A KiCad-legal file/symbol name derived from the part. EasyEDA package names carry
		// characters KiCad refuses in a library entry.
		static std::string sanitize(const std::string& name);

		// EasyEDA's numeric layer id -> a KiCad layer name, "" for one we do not place. Public
		// because the mapping is the piece most likely to need extending.
		static std::string kicadLayer(int easyEdaLayerId);
	};

}
