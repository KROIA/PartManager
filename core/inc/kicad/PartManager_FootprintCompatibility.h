// @file PartManager_FootprintCompatibility.h
// @brief Can this part use that footprint? Scores two footprints' pads against each other (§5a).
//
// The question a user is asked when a vendor archive brings a footprint the
// database already has an equivalent of: *is the one I already have actually
// compatible?* This is the number behind that suggestion — it ranks the
// footprints already in the database against the downloaded one so the best
// candidate is offered first, and the overlay is the evidence the user confirms
// it with.
//
// **Pads only, because copper is what decides it.** Silkscreen, courtyard and
// assembly outlines differ between vendors constantly and change nothing about
// whether a part solders down (`KicadGeometry::padsOnly`).
//
// **`compatible` is a green light, not a gate.** Everything is ranked, including
// what this refuses: a user looking at the overlay can accept a footprint the
// metric declined, and that is a legitimate answer — IPC publishes three density
// levels for the same package and all of them take the same part. What the flag
// promises is the narrower thing: *nobody needs to look hard at this one*.
//
// **Rotation is deliberately not normalised.** A footprint whose pads run
// north-south is not interchangeable with one whose pads run east-west; KiCad
// would place the part turned, and it would not fit the board it was designed
// for. Two vendors placing the same pads around a different *origin* is the
// ordinary case and is handled — the comparison aligns on the pad centroid
// before measuring, so a whole-footprint translation costs nothing.
// @see docs/design/ARCHITECTURE.md §5a
// @see PartManager_KicadGeometry.h, PartManager_FootprintVariants.h
#pragma once

#include "PartManager_global.h"
#include "kicad/PartManager_KicadGeometry.h"
#include <string>
#include <vector>

namespace PartManager
{

	// What counts as "the same pad". These are physical judgements, not arbitrary epsilons —
	// see the .cpp for the numbers they were measured against.
	struct PART_MANAGER_API FootprintTolerance
	{
		// Pad-centre distance after alignment. 0.15 mm covers the spread between vendors'
		// land patterns for one package and stays well inside the gap between *different*
		// packages: 0402 and 0603 pad centres are 0.36 mm apart, more than twice this.
		double positionMm = 0.15;
		// Per-dimension pad size difference. 0.25 mm absorbs an IPC density-level step
		// without reaching the 0.3-0.4 mm that separates one chip size from the next.
		double sizeMm = 0.25;
		// A hole either takes the lead or it does not; this is tighter than the others.
		double drillMm = 0.10;
		// Pads turned relative to each other. Compared modulo 180°, because a rectangle
		// turned by half a circle is the same rectangle.
		double rotationDegrees = 1.0;
	};

	// Why a pair scored the way it did. The numbers are the answer; `summary` is a plain-text
	// rendering of them for logs and tests.
	//
	// **The UI builds its own sentence from the fields** rather than showing `summary`: this is
	// `core/`, it has no `tr()`, and a user-facing string that cannot be translated would be the
	// one untranslated line on a German screen.
	struct PART_MANAGER_API FootprintComparison
	{
		// Every hard test passed. False when the pad counts differ, when one is through-hole
		// and the other is not, or when a position/size/drill/rotation is out of tolerance.
		bool compatible = false;
		// 1.0 for identical copper, 0.0 for unusable. Ranks candidates *including* ones that
		// failed `compatible`, so the user choosing manually still sees the closest first.
		double score = 0.0;

		int padsA = 0;
		int padsB = 0;
		// The translation applied before measuring — how far apart the two origins were.
		double alignmentXMm = 0.0;
		double alignmentYMm = 0.0;

		double maxPositionOffsetMm = 0.0;
		double maxSizeDeltaMm = 0.0;
		double maxDrillDeltaMm = 0.0;
		double maxRotationDeltaDegrees = 0.0;

		// Through-hole against surface-mount. Disqualifying on its own: one needs a hole in
		// the board and the other must not have one.
		bool mountingMismatch = false;
		// Round or oval against rectangular, at otherwise equal size. Reported and penalised,
		// **not** disqualifying: the part solders either way, and the user can see it.
		bool shapeDiffers = false;
		// Pads were paired by their numbers. False means they were paired by position, which
		// happens when a footprint's pads are unnumbered — mounting holes, or a vendor file
		// that leaves them blank.
		bool matchedByPadNumber = false;

		std::string summary;
	};

	// One candidate and how well it fits, for the "use one you already have" suggestion.
	struct PART_MANAGER_API FootprintCandidate
	{
		// Index into the vector handed to rank(), so the caller can find its own object again.
		size_t index = 0;
		FootprintComparison comparison;
	};

	class PART_MANAGER_API FootprintCompatibility
	{
		FootprintCompatibility() = delete;
	public:
		// Compares the pads of two footprints. Order does not matter: swapping the arguments
		// gives the same verdict and the same numbers, with the alignment negated.
		static FootprintComparison compare(const KicadDrawing& a, const KicadDrawing& b,
			const FootprintTolerance& tolerance = FootprintTolerance());

		// Every candidate scored against `downloaded`, best first, **by score alone**. Nothing
		// is filtered out and `compatible` is not a sort key: it is a badge the caller shows
		// beside the numbers. A candidate that just missed the tolerance sits where its
		// measurements put it, because on real data the margins are hundredths of a millimetre
		// and a boolean in front of the ordering would claim a difference the copper does not
		// have. Ties break on the original index, so the order is reproducible — a suggestion
		// that reshuffles between two runs over the same data is one nobody can check.
		//
		// A zero score means *never suggest this*: through-hole against surface-mount lands
		// there, and it sinks to the bottom on its own rather than needing to be excluded.
		static std::vector<FootprintCandidate> rank(const KicadDrawing& downloaded,
			const std::vector<KicadDrawing>& candidates,
			const FootprintTolerance& tolerance = FootprintTolerance());
	};

}
