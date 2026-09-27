#pragma once

#include "UnitTest.h"
#include "kicad/PartManager_FootprintCompatibility.h"
#include "kicad/PartManager_KicadGeometry.h"
#include <cmath>
#include <string>
#include <vector>

// §5a's "you already have a footprint like this one" suggestion. The metric decides which
// existing footprint is offered first when a vendor archive brings a new one, and the user is
// asked to trust it — so the cases that matter are the two ends: land patterns that genuinely
// take the same part must score compatible, and one package size must never match its
// neighbour, however close they look in a list.
class TST_FootprintCompatibility : public UnitTest::Test
{
	TEST_CLASS(TST_FootprintCompatibility)
public:
	TST_FootprintCompatibility()
		: Test("TST_FootprintCompatibility")
	{
		ADD_TEST(TST_FootprintCompatibility::twoVendors0603LandPatternsAreCompatible);
		ADD_TEST(TST_FootprintCompatibility::aDifferentPackageSizeNeverMatches);
		ADD_TEST(TST_FootprintCompatibility::movingTheWholeFootprintCostsNothing);
		ADD_TEST(TST_FootprintCompatibility::throughHoleAndSurfaceMountAreNeverCompatible);
		ADD_TEST(TST_FootprintCompatibility::padCountIsTheFirstThingChecked);
		ADD_TEST(TST_FootprintCompatibility::theVerdictDoesNotDependOnArgumentOrder);
		ADD_TEST(TST_FootprintCompatibility::rankingPutsTheBestFitFirst);
		ADD_TEST(TST_FootprintCompatibility::aCloseMissOutranksABarelyPassingFit);
	}

private:
	// A two-pad chip land pattern, the shape every vendor writes for a passive: pads at ±x,
	// centred on the origin unless `shift` moves the whole thing.
	static PartManager::KicadDrawing chipLand(double centreX, double padX, double padY,
		double shiftX = 0.0, double shiftY = 0.0)
	{
		PartManager::KicadDrawing drawing;
		drawing.yAxisPointsUp = false;
		drawing.name = "chip";
		for (int side = 0; side < 2; ++side)
		{
			PartManager::KicadShape pad;
			pad.kind = PartManager::KicadShapeKind::Pad;
			pad.points.push_back({ (side == 0 ? -centreX : centreX) + shiftX, shiftY });
			pad.sizeX = padX;
			pad.sizeY = padY;
			pad.label = side == 0 ? "1" : "2";
			pad.layer = "F.Cu";
			drawing.shapes.push_back(pad);
		}
		// Silkscreen and a courtyard, so every case also proves the metric ignores them.
		PartManager::KicadShape silk;
		silk.kind = PartManager::KicadShapeKind::Polyline;
		silk.points.push_back({ -5.0 + shiftX, -5.0 + shiftY });
		silk.points.push_back({ 5.0 + shiftX, -5.0 + shiftY });
		silk.layer = "F.SilkS";
		drawing.shapes.push_back(silk);
		return drawing;
	}

	static PartManager::KicadDrawing throughHoleLand(double centreX, double diameter, double drill)
	{
		PartManager::KicadDrawing drawing;
		drawing.yAxisPointsUp = false;
		for (int side = 0; side < 2; ++side)
		{
			PartManager::KicadShape pad;
			pad.kind = PartManager::KicadShapeKind::Pad;
			pad.points.push_back({ side == 0 ? -centreX : centreX, 0.0 });
			pad.sizeX = diameter;
			pad.sizeY = diameter;
			pad.roundPad = true;
			pad.throughHole = true;
			pad.drillDiameter = drill;
			pad.label = side == 0 ? "1" : "2";
			pad.layer = "*.Cu";
			drawing.shapes.push_back(pad);
		}
		return drawing;
	}

public:

	// The case the feature exists for: the same 0603 from two vendors, differing by the ~0.05 mm
	// of pad width measured in the user's own library. These take the same part and the user
	// should not have to think about them.
	TEST_FUNCTION(twoVendors0603LandPatternsAreCompatible)
	{
		TEST_START;

		const PartManager::KicadDrawing yageo = chipLand(0.8375, 0.9, 0.95);
		const PartManager::KicadDrawing bourns = chipLand(0.85, 0.95, 0.95);

		const PartManager::FootprintComparison result =
			PartManager::FootprintCompatibility::compare(yageo, bourns);
		TEST_ASSERT_M(result.compatible, result.summary);
		TEST_COMPARE(result.padsA, 2);
		TEST_ASSERT_M(result.matchedByPadNumber, "both files number their pads, so 1 must meet 1");
		TEST_ASSERT_M(result.maxPositionOffsetMm < 0.02, result.summary);
		TEST_ASSERT_M(std::abs(result.maxSizeDeltaMm - 0.05) < 1e-9, result.summary);
		TEST_ASSERT_M(result.score > 0.8, result.summary);
		// The silkscreen in the fixture is far bigger than either land pattern; if it were being
		// measured the offsets above could not be this small.
		TEST_ASSERT_M(result.summary.find("same 2 pad(s)") != std::string::npos, result.summary);
	}

	// The failure that would matter most: offering an 0402 land pattern for an 0603 part. The
	// numbers are close enough to look plausible in a list and are not close at all in copper.
	TEST_FUNCTION(aDifferentPackageSizeNeverMatches)
	{
		TEST_START;

		const PartManager::KicadDrawing land0603 = chipLand(0.8375, 0.9, 0.95);
		const PartManager::KicadDrawing land0402 = chipLand(0.48, 0.6, 0.6);

		const PartManager::FootprintComparison result =
			PartManager::FootprintCompatibility::compare(land0603, land0402);
		TEST_ASSERT_M(!result.compatible, result.summary);
		// Rejected twice over — position *and* size — which is the margin the tolerances were
		// chosen for rather than a lucky single failure.
		TEST_ASSERT_M(result.maxPositionOffsetMm > 0.15, result.summary);
		TEST_ASSERT_M(result.maxSizeDeltaMm > 0.25, result.summary);
	}

	// Two vendors drawing the same pads around a different origin is the ordinary case, not a
	// difference: the comparison aligns on the pad centroid first.
	TEST_FUNCTION(movingTheWholeFootprintCostsNothing)
	{
		TEST_START;

		const PartManager::KicadDrawing atOrigin = chipLand(0.8375, 0.9, 0.95);
		const PartManager::KicadDrawing moved = chipLand(0.8375, 0.9, 0.95, 12.7, -5.08);

		const PartManager::FootprintComparison result =
			PartManager::FootprintCompatibility::compare(atOrigin, moved);
		TEST_ASSERT_M(result.compatible, result.summary);
		TEST_ASSERT_M(result.maxPositionOffsetMm < 1e-9, result.summary);
		TEST_ASSERT_M(std::abs(result.score - 1.0) < 1e-9, result.summary);
		// ...and it says how far it had to move, because "these are the same" is a claim the
		// user may want to check against the two files.
		TEST_ASSERT_M(std::abs(result.alignmentXMm - 12.7) < 1e-9,
			std::to_string(result.alignmentXMm));
		TEST_ASSERT_M(std::abs(result.alignmentYMm + 5.08) < 1e-9,
			std::to_string(result.alignmentYMm));
	}

	// One needs a hole drilled and the other must not have one. No tolerance makes that fit.
	TEST_FUNCTION(throughHoleAndSurfaceMountAreNeverCompatible)
	{
		TEST_START;

		const PartManager::KicadDrawing smd = chipLand(1.27, 1.5, 1.5);
		const PartManager::KicadDrawing tht = throughHoleLand(1.27, 1.5, 0.8);

		const PartManager::FootprintComparison result =
			PartManager::FootprintCompatibility::compare(smd, tht);
		TEST_ASSERT_M(!result.compatible, result.summary);
		TEST_ASSERT(result.mountingMismatch);
		// Scored zero, not merely below the bar: it must never be suggested, however well its
		// pads happen to line up — and in this fixture they line up exactly.
		TEST_ASSERT_M(result.score == 0.0, result.summary);
		TEST_ASSERT_M(result.maxPositionOffsetMm < 1e-9, result.summary);
	}

	TEST_FUNCTION(padCountIsTheFirstThingChecked)
	{
		TEST_START;

		PartManager::KicadDrawing three = chipLand(0.8375, 0.9, 0.95);
		PartManager::KicadShape extra;
		extra.kind = PartManager::KicadShapeKind::Pad;
		extra.points.push_back({ 0.0, 1.5 });
		extra.sizeX = 0.9;
		extra.sizeY = 0.95;
		extra.label = "3";
		three.shapes.push_back(extra);

		const PartManager::FootprintComparison result =
			PartManager::FootprintCompatibility::compare(chipLand(0.8375, 0.9, 0.95), three);
		TEST_ASSERT_M(!result.compatible, result.summary);
		TEST_COMPARE(result.padsA, 2);
		TEST_COMPARE(result.padsB, 3);
		// A different number of pads is a different part, so it is not scored as a near miss.
		TEST_ASSERT_M(result.score == 0.0, result.summary);
		TEST_ASSERT_M(result.summary.find("different pad counts") != std::string::npos,
			result.summary);
	}

	TEST_FUNCTION(theVerdictDoesNotDependOnArgumentOrder)
	{
		TEST_START;

		const PartManager::KicadDrawing a = chipLand(0.8375, 0.9, 0.95);
		const PartManager::KicadDrawing b = chipLand(0.85, 0.95, 0.95, 3.0, 0.0);

		const PartManager::FootprintComparison forward =
			PartManager::FootprintCompatibility::compare(a, b);
		const PartManager::FootprintComparison backward =
			PartManager::FootprintCompatibility::compare(b, a);

		TEST_COMPARE(forward.compatible, backward.compatible);
		TEST_ASSERT_M(std::abs(forward.maxPositionOffsetMm - backward.maxPositionOffsetMm) < 1e-9,
			forward.summary + " / " + backward.summary);
		TEST_ASSERT_M(std::abs(forward.maxSizeDeltaMm - backward.maxSizeDeltaMm) < 1e-9,
			forward.summary + " / " + backward.summary);
		// The alignment is the one thing that flips, because it says which way the second one
		// had to move.
		TEST_ASSERT_M(std::abs(forward.alignmentXMm + backward.alignmentXMm) < 1e-9,
			std::to_string(forward.alignmentXMm) + " / " + std::to_string(backward.alignmentXMm));
	}

	// What the suggestion actually shows: the closest fit first, everything compatible before
	// anything that is not, and the same order every run.
	TEST_FUNCTION(rankingPutsTheBestFitFirst)
	{
		TEST_START;

		const PartManager::KicadDrawing downloaded = chipLand(0.8375, 0.9, 0.95);

		std::vector<PartManager::KicadDrawing> candidates;
		candidates.push_back(chipLand(0.48, 0.6, 0.6));            // 0: an 0402, incompatible
		candidates.push_back(chipLand(0.85, 0.95, 0.95));          // 1: a near 0603
		candidates.push_back(chipLand(0.8375, 0.9, 0.95, 5.0));    // 2: the same, moved
		candidates.push_back(throughHoleLand(0.8375, 0.9, 0.5));   // 3: through-hole

		const std::vector<PartManager::FootprintCandidate> ranked =
			PartManager::FootprintCompatibility::rank(downloaded, candidates);
		TEST_COMPARE(ranked.size(), static_cast<size_t>(4));
		// The exact match wins, the near one follows, and both beat everything rejected.
		TEST_COMPARE(ranked[0].index, static_cast<size_t>(2));
		TEST_ASSERT_M(ranked[0].comparison.compatible, ranked[0].comparison.summary);
		TEST_COMPARE(ranked[1].index, static_cast<size_t>(1));
		TEST_ASSERT_M(ranked[1].comparison.compatible, ranked[1].comparison.summary);
		TEST_ASSERT_M(!ranked[2].comparison.compatible, ranked[2].comparison.summary);
		TEST_ASSERT_M(!ranked[3].comparison.compatible, ranked[3].comparison.summary);

		// Stable: the same input gives the same order, so a suggestion can be checked twice.
		const std::vector<PartManager::FootprintCandidate> again =
			PartManager::FootprintCompatibility::rank(downloaded, candidates);
		for (size_t i = 0; i < ranked.size(); ++i)
		{
			TEST_COMPARE(again[i].index, ranked[i].index);
		}
	}

	// `compatible` is a badge, not a sort key. On the user's real library the margins are
	// hundredths of a millimetre — 0.14 mm accepted against a 0.15 mm threshold, 0.27 mm
	// rejected against 0.25 mm — so putting the boolean in front of the score would claim a
	// difference the copper does not have, and would bury a deliberately denser land pattern
	// under everything that happened to scrape past.
	TEST_FUNCTION(aCloseMissOutranksABarelyPassingFit)
	{
		TEST_START;

		const PartManager::KicadDrawing downloaded = chipLand(0.8375, 0.9, 0.95);

		std::vector<PartManager::KicadDrawing> candidates;
		// 0: inside both tolerances, but only just — 0.14 mm off and 0.24 mm bigger.
		candidates.push_back(chipLand(0.8375 + 0.14, 1.14, 0.95));
		// 1: fails position by 0.01 mm and matches every size exactly.
		candidates.push_back(chipLand(0.8375 + 0.16, 0.9, 0.95));

		const PartManager::FootprintComparison passing =
			PartManager::FootprintCompatibility::compare(downloaded, candidates[0]);
		const PartManager::FootprintComparison failing =
			PartManager::FootprintCompatibility::compare(downloaded, candidates[1]);
		TEST_ASSERT_M(passing.compatible, passing.summary);
		TEST_ASSERT_M(!failing.compatible, failing.summary);

		const std::vector<PartManager::FootprintCandidate> ranked =
			PartManager::FootprintCompatibility::rank(downloaded, candidates);
		// The one that missed comes first, because it measures better. The badge still says it
		// missed, and the numbers are there for the user to judge with the overlay.
		TEST_COMPARE(ranked[0].index, static_cast<size_t>(1));
		TEST_ASSERT_M(!ranked[0].comparison.compatible, ranked[0].comparison.summary);
		TEST_COMPARE(ranked[1].index, static_cast<size_t>(0));
		TEST_ASSERT_M(ranked[1].comparison.compatible, ranked[1].comparison.summary);
		TEST_ASSERT_M(ranked[0].comparison.score > ranked[1].comparison.score,
			ranked[0].comparison.summary + " / " + ranked[1].comparison.summary);

		// ...and nothing is filtered away: both are still offerable.
		TEST_COMPARE(ranked.size(), static_cast<size_t>(2));
	}
};

TEST_INSTANTIATE(TST_FootprintCompatibility);
