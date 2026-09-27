#include "kicad/PartManager_FootprintCompatibility.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace PartManager
{

	namespace
	{
		// Where the tolerances come from, so the next person can argue with the numbers rather
		// than with a mystery:
		//
		//   0402 nominal land: pads 0.6 x 0.6 mm, centres at ±0.48 mm
		//   0603 nominal land: pads 0.9 x 0.95 mm, centres at ±0.8375 mm
		//
		// One package size to the next moves a pad centre by 0.36 mm and its size by 0.3 mm.
		// The spread *within* one package — different vendors, IPC's three density levels — is
		// about a third of that. So position 0.15 mm and size 0.25 mm sit in the gap: generous
		// enough that two vendors' 0603 land patterns match, less than half the distance to the
		// neighbouring package in both measures. Measured against the user's own library on
		// 2026-09-27, where eight 0603 footprints differ from each other by ~0.05 mm of pad
		// width — well inside these, and genuinely interchangeable.

		struct Pad
		{
			double x = 0.0;
			double y = 0.0;
			double sizeX = 0.0;
			double sizeY = 0.0;
			double rotation = 0.0;
			double drill = 0.0;
			bool round = false;
			bool throughHole = false;
			std::string number;
		};

		std::vector<Pad> padsOf(const KicadDrawing& drawing)
		{
			std::vector<Pad> pads;
			for (const KicadShape& shape : KicadGeometry::padsOnly(drawing).shapes)
			{
				if (shape.points.empty())
				{
					continue;
				}
				Pad pad;
				pad.x = shape.points[0].x;
				pad.y = shape.points[0].y;
				pad.sizeX = shape.sizeX;
				pad.sizeY = shape.sizeY;
				pad.rotation = shape.rotationDegrees;
				pad.drill = shape.drillDiameter;
				pad.round = shape.roundPad;
				pad.throughHole = shape.throughHole;
				pad.number = shape.label;
				pads.push_back(pad);
			}
			// A stable order before any pairing, so the greedy fallback below cannot depend on
			// the order shapes happened to be parsed in.
			std::sort(pads.begin(), pads.end(), [](const Pad& a, const Pad& b)
				{
					if (a.x != b.x) { return a.x < b.x; }
					if (a.y != b.y) { return a.y < b.y; }
					return a.number < b.number;
				});
			return pads;
		}

		// Every pad numbered, and no number used twice. Anything less and the numbers are not a
		// key — a vendor file with blank labels would otherwise pair every pad with the first.
		bool hasUsableNumbers(const std::vector<Pad>& pads)
		{
			std::set<std::string> seen;
			for (const Pad& pad : pads)
			{
				if (pad.number.empty() || !seen.insert(pad.number).second)
				{
					return false;
				}
			}
			return true;
		}

		// Half a circle: a rectangle turned by 180° is the same rectangle, and vendors write
		// that both ways round routinely.
		double rotationDelta(double a, double b)
		{
			double delta = std::fmod(std::abs(a - b), 180.0);
			if (delta > 90.0)
			{
				delta = 180.0 - delta;
			}
			return delta;
		}

		std::string number(double value)
		{
			std::ostringstream out;
			out.precision(3);
			out << std::fixed << value;
			std::string text = out.str();
			// Trailing zeros make "0.050 mm" of a measurement that is really 0.05.
			while (text.size() > 1 && text.back() == '0') { text.pop_back(); }
			if (!text.empty() && text.back() == '.') { text.pop_back(); }
			return text;
		}
	}

	FootprintComparison FootprintCompatibility::compare(const KicadDrawing& a, const KicadDrawing& b,
		const FootprintTolerance& tolerance)
	{
		FootprintComparison result;
		const std::vector<Pad> padsA = padsOf(a);
		const std::vector<Pad> padsB = padsOf(b);
		result.padsA = static_cast<int>(padsA.size());
		result.padsB = static_cast<int>(padsB.size());

		if (padsA.empty() || padsB.empty())
		{
			result.summary = "one of the footprints has no pads to compare";
			return result;
		}
		if (padsA.size() != padsB.size())
		{
			// Not a near miss to be scored: a different number of pads is a different part.
			result.summary = "different pad counts (" + std::to_string(result.padsA) + " and "
				+ std::to_string(result.padsB) + ")";
			return result;
		}

		// Align on the centroid of the pad centres, which is what makes a whole-footprint
		// translation free. Not on the bounding box: one oversized pad would drag the box and
		// shift every other pad's measured offset with it.
		double centroidAx = 0.0, centroidAy = 0.0, centroidBx = 0.0, centroidBy = 0.0;
		for (const Pad& pad : padsA) { centroidAx += pad.x; centroidAy += pad.y; }
		for (const Pad& pad : padsB) { centroidBx += pad.x; centroidBy += pad.y; }
		const double count = static_cast<double>(padsA.size());
		centroidAx /= count; centroidAy /= count;
		centroidBx /= count; centroidBy /= count;
		result.alignmentXMm = centroidBx - centroidAx;
		result.alignmentYMm = centroidBy - centroidAy;

		// Pair by pad number when both sides have real numbers: pad 1 has to meet pad 1, or a
		// polarised part could be judged compatible with its own mirror image.
		std::vector<std::pair<const Pad*, const Pad*>> pairs;
		if (hasUsableNumbers(padsA) && hasUsableNumbers(padsB))
		{
			std::map<std::string, const Pad*> byNumber;
			for (const Pad& pad : padsB) { byNumber[pad.number] = &pad; }
			bool complete = true;
			for (const Pad& pad : padsA)
			{
				const auto found = byNumber.find(pad.number);
				if (found == byNumber.end()) { complete = false; break; }
				pairs.push_back({ &pad, found->second });
			}
			if (complete)
			{
				result.matchedByPadNumber = true;
			}
			else
			{
				// Same count, different numbering — fall through to position pairing rather
				// than refusing: "1,2" against "A,K" is a diode, and it is still comparable.
				pairs.clear();
			}
		}
		if (pairs.empty())
		{
			std::vector<bool> taken(padsB.size(), false);
			for (const Pad& pad : padsA)
			{
				size_t best = padsB.size();
				double bestDistance = 0.0;
				for (size_t i = 0; i < padsB.size(); ++i)
				{
					if (taken[i]) { continue; }
					const double dx = (pad.x + result.alignmentXMm) - padsB[i].x;
					const double dy = (pad.y + result.alignmentYMm) - padsB[i].y;
					const double distance = std::sqrt(dx * dx + dy * dy);
					if (best == padsB.size() || distance < bestDistance)
					{
						best = i;
						bestDistance = distance;
					}
				}
				if (best == padsB.size()) { break; }
				taken[best] = true;
				pairs.push_back({ &pad, &padsB[best] });
			}
		}

		for (const std::pair<const Pad*, const Pad*>& pair : pairs)
		{
			const Pad& left = *pair.first;
			const Pad& right = *pair.second;
			const double dx = (left.x + result.alignmentXMm) - right.x;
			const double dy = (left.y + result.alignmentYMm) - right.y;
			result.maxPositionOffsetMm =
				std::max(result.maxPositionOffsetMm, std::sqrt(dx * dx + dy * dy));
			result.maxSizeDeltaMm = std::max(result.maxSizeDeltaMm,
				std::max(std::abs(left.sizeX - right.sizeX), std::abs(left.sizeY - right.sizeY)));
			result.maxDrillDeltaMm =
				std::max(result.maxDrillDeltaMm, std::abs(left.drill - right.drill));
			result.maxRotationDeltaDegrees = std::max(result.maxRotationDeltaDegrees,
				rotationDelta(left.rotation, right.rotation));
			if (left.throughHole != right.throughHole) { result.mountingMismatch = true; }
			if (left.round != right.round) { result.shapeDiffers = true; }
		}

		result.compatible = !result.mountingMismatch
			&& result.maxPositionOffsetMm <= tolerance.positionMm
			&& result.maxSizeDeltaMm <= tolerance.sizeMm
			&& result.maxDrillDeltaMm <= tolerance.drillMm
			&& result.maxRotationDeltaDegrees <= tolerance.rotationDegrees;

		// Position is weighted above size because it is the one that decides whether the part
		// lands on its pads at all; an oversized pad still takes the same lead. A shape
		// difference costs a little so an exact match outranks an equally-placed oval one.
		const double positionScore =
			1.0 - std::min(1.0, result.maxPositionOffsetMm / std::max(tolerance.positionMm, 1e-9));
		const double sizeScore =
			1.0 - std::min(1.0, result.maxSizeDeltaMm / std::max(tolerance.sizeMm, 1e-9));
		result.score = 0.6 * positionScore + 0.4 * sizeScore;
		if (result.shapeDiffers) { result.score -= 0.05; }
		if (result.mountingMismatch) { result.score = 0.0; }
		result.score = std::max(0.0, std::min(1.0, result.score));

		std::string text = "same " + std::to_string(result.padsA) + " pad(s), max offset "
			+ number(result.maxPositionOffsetMm) + " mm, max size difference "
			+ number(result.maxSizeDeltaMm) + " mm";
		if (result.mountingMismatch) { text += "; one is through-hole and the other is not"; }
		if (result.shapeDiffers) { text += "; pad shapes differ"; }
		if (result.maxDrillDeltaMm > 0.0) { text += "; drill differs by "
			+ number(result.maxDrillDeltaMm) + " mm"; }
		if (result.maxRotationDeltaDegrees > 0.0) { text += "; pads turned by "
			+ number(result.maxRotationDeltaDegrees) + "°"; }
		if (!result.matchedByPadNumber) { text += "; paired by position, not by pad number"; }
		result.summary = text;
		return result;
	}

	std::vector<FootprintCandidate> FootprintCompatibility::rank(const KicadDrawing& downloaded,
		const std::vector<KicadDrawing>& candidates, const FootprintTolerance& tolerance)
	{
		std::vector<FootprintCandidate> ranked;
		ranked.reserve(candidates.size());
		for (size_t i = 0; i < candidates.size(); ++i)
		{
			FootprintCandidate candidate;
			candidate.index = i;
			candidate.comparison = compare(downloaded, candidates[i], tolerance);
			ranked.push_back(candidate);
		}
		std::stable_sort(ranked.begin(), ranked.end(),
			[](const FootprintCandidate& a, const FootprintCandidate& b)
			{
				// **Score only — `compatible` is a badge, never a sort key.** Sorting by the
				// flag first would put a visible gap between a candidate at 0.145 mm and one at
				// 0.16 mm, and physically those are the same answer: *very close, look at it*.
				// On the user's real library the margins are 0.01 mm and 0.02 mm, so a boolean
				// in front of the score introduces a discontinuity the measurement does not
				// have. It also buries the case that matters most — IPC publishes three density
				// levels for one package, and someone deliberately after the denser land
				// pattern has to find it in order rather than underneath everything that
				// happened to pass a threshold.
				//
				// stable_sort keeps the original index as the tie-break, so the order is
				// reproducible for equal scores.
				return a.comparison.score > b.comparison.score;
			});
		return ranked;
	}

}
