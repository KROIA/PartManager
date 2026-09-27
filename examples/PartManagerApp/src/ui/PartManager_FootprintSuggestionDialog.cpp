#include "ui/PartManager_FootprintSuggestionDialog.h"
#include "ui_PartManager_FootprintSuggestionDialog.h"

#include "widgets/PartManager_KicadVariantView.h"
#include "filestore/PartManager_FileStore.h"
#include "kicad/PartManager_FootprintVariants.h"
#include "kicad/PartManager_KicadGeometry.h"
#include "persistence/PartManager_PartRepository.h"

#include <fstream>
#include <iterator>

#include <QDialogButtonBox>
#include <QEvent>
#include <QFont>
#include <QKeyEvent>
#include <QMessageBox>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <QVariant>
#include <QVBoxLayout>

namespace PartManager
{
	namespace
	{
		constexpr int CandidateIndexRole = Qt::UserRole;

		// The reference and the candidate get one colour each, and they keep it in the tree, the
		// legend and the overlay — "which of these is which" has to be answerable at a glance.
		const QColor ReferenceColour(0xFF, 0xB3, 0x4D);    // amber
		const QColor ExistingColour(0x4F, 0xC3, 0xF7);     // blue

		// The reference's key in the view. It is a fixed string rather than a content hash: the
		// reference is not a variant the tree can address, it is the thing everything else is
		// measured against, and it is the entry drawn on top in both states of the panel.
		const QString ReferenceKey = QStringLiteral("reference");

		// A row the user can choose. Group headers and package nodes carry no `CandidateIndexRole`
		// — testing for it is the same question as "is this a leaf", asked of the data rather
		// than of `childCount()`, which would call an empty group a choice.
		bool isCandidateItem(const QTreeWidgetItem* item)
		{
			return item != nullptr && item->data(0, CandidateIndexRole).isValid();
		}

		// Sibling navigation that works at the top level too, where `parent()` is null and the
		// tree itself is the container.
		QTreeWidgetItem* siblingOf(QTreeWidget* tree, QTreeWidgetItem* item, int offset)
		{
			if (QTreeWidgetItem* parent = item->parent())
			{
				const int row = parent->indexOfChild(item) + offset;
				return row >= 0 && row < parent->childCount() ? parent->child(row) : nullptr;
			}
			const int row = tree->indexOfTopLevelItem(item) + offset;
			return row >= 0 && row < tree->topLevelItemCount() ? tree->topLevelItem(row) : nullptr;
		}

		// The last row `item` currently shows: its last child's last child, as deep as the
		// expansions go. Where a step *upwards* lands when it passes a whole group.
		QTreeWidgetItem* lastVisibleDescendant(QTreeWidgetItem* item)
		{
			while (item->isExpanded() && item->childCount() > 0)
			{
				item = item->child(item->childCount() - 1);
			}
			return item;
		}

		std::string readWholeFile(const std::string& path)
		{
			std::ifstream stream(path, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>());
		}

		QString millimetres(double value)
		{
			return QString::number(value, 'f', 2);
		}
	}

	FootprintSuggestionDialog::FootprintSuggestionDialog(DatabaseHandle* handle, int partId,
		const QByteArray& reference, const QString& referenceName, Trigger trigger,
		QWidget* parent)
		: QDialog(parent)
		, m_handle(handle)
		, m_partId(partId)
		, m_trigger(trigger)
		, m_reference(reference)
		, m_referenceName(referenceName)
		, m_ui(new Ui::FootprintSuggestionDialog)
	{
		m_ui->setupUi(this);
		m_ui->mainLayout->setStretch(1, 1);

		m_view = new KicadVariantView(m_ui->viewHost);
		// Overlay only: two footprints, and the question is where they differ. The side-by-side
		// tiles answer "what are they", which the user already knows — one is the download.
		m_view->setMode(KicadVariantView::Mode::Overlay);
		// **Peers, not a highlight.** The view's default draws the top entry opaque, which on a
		// two-footprint comparison paints the amber reference straight over the blue candidate
		// wherever their pads agree — hiding exactly the copper the user is here to judge. Both
		// translucent means a shared pad comes out a third colour and a pad that is a fraction
		// of a millimetre wider in one of them shows as a fringe in that one's own colour. The
		// variant browser keeps the default: it has *N* entries, and blending N is unreadable.
		m_view->setOverlayBlend(KicadVariantView::Blend::Peers);
		m_ui->viewLayout->addWidget(m_view);

		m_ui->candidateTree->header()->setStretchLastSection(false);
		m_ui->candidateTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
		m_ui->candidateTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		m_ui->candidateTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		// Key events go to the tree itself, not to its viewport — the viewport never takes focus.
		m_ui->candidateTree->installEventFilter(this);

		m_useExisting = m_ui->buttonBox->addButton(tr("Use this footprint"),
			QDialogButtonBox::AcceptRole);
		// "Keep the downloaded one" is a lie on the button path — nothing was downloaded, the
		// part has had this footprint for as long as it has existed.
		m_ui->buttonBox->addButton(m_trigger == Trigger::UserRequested
			? tr("Keep the current one")
			: tr("Keep the downloaded one"), QDialogButtonBox::RejectRole);
		connect(m_useExisting, &QPushButton::clicked, this, &FootprintSuggestionDialog::onAccept);
		connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
		// Nothing is current yet, and the tree now also holds rows that are not a choice.
		// `onSelectionChanged()` owns this from here on.
		m_useExisting->setEnabled(false);

		if (m_handle != nullptr && m_handle->isOpen())
		{
			Part part;
			if (PartRepository::findPart(m_handle->connection(), m_partId, part))
			{
				m_partPackage = QString::fromStdString(part.package);   // user data
			}
		}

		m_referenceDrawing = KicadGeometry::footprint(
			std::string(m_reference.constData(), static_cast<size_t>(m_reference.size())));
		buildCandidates();

		connect(m_ui->candidateTree, &QTreeWidget::currentItemChanged, this,
			[this](QTreeWidgetItem* current, QTreeWidgetItem*) { onSelectionChanged(current); });

		if (QTreeWidgetItem* first = firstCandidateItem())
		{
			// The top row is pre-selected — since the grouping, that is the best fit *among the
			// favourites* whenever there are any, because a favourite the user has to scroll past
			// a better-measuring stranger to reach is not being offered as one. Unless it scored
			// zero, which means "never suggest this" (a through-hole land pattern against a
			// surface-mount one): still selectable by hand, never proposed.
			const int index = first->data(0, CandidateIndexRole).toInt();
			if (m_candidates[static_cast<size_t>(index)].comparison.score > 0.0)
			{
				m_ui->candidateTree->setCurrentItem(first);
			}
		}
		if (m_ui->candidateTree->currentItem() == nullptr)
		{
			// Nothing was preselected — every row scored zero, so none is worth proposing. The
			// panel still opens on the part's own footprint rather than on a blank: `currentItem`
			// never changed, so nothing would have called `showCandidate()` at all.
			showCandidate(-1);
		}

		// Two wordings, because on the button path there is no download to speak of and the
		// reference is what the part has been using all along. Same sentence otherwise — the
		// colours mean the same thing and the choice has the same consequence.
		m_ui->headerLabel->setText(m_trigger == Trigger::UserRequested
			? tr("This part uses “%1”. %n footprint(s) other parts already have could take its "
				"place — using one means both parts share a single file. Check the overlay "
				"first: the footprint in use now is drawn in amber, the one you are looking at "
				"in blue.", "", static_cast<int>(m_candidates.size())).arg(m_referenceName)
			: tr("“%1” looks like %n footprint(s) this database already has. Using one of them "
				"means both parts share a single file — check the overlay first: the download "
				"is drawn in amber, the one you are looking at in blue.", "",
				static_cast<int>(m_candidates.size())).arg(m_referenceName));
	}

	FootprintSuggestionDialog::~FootprintSuggestionDialog()
	{
		delete m_ui;
	}

	int FootprintSuggestionDialog::chosenSourcePartId() const
	{
		// `m_chosen` is -1 whenever the current row is a group header, so a header can never
		// answer this — it is not a footprint and there is nothing to share from it.
		if (!m_tookExisting || m_chosen < 0
			|| m_chosen >= static_cast<int>(m_candidates.size()))
		{
			return 0;
		}
		return m_candidates[static_cast<size_t>(m_chosen)].examplePartId;
	}

	void FootprintSuggestionDialog::buildCandidates()
	{
		if (m_handle == nullptr || !m_handle->isOpen())
		{
			return;
		}
		if (m_referenceDrawing.empty())
		{
			// Nothing to measure *against*, which is a different answer from "nothing matched"
			// and reads as a different sentence on the button path — the file the part carries
			// is there, it simply holds no pads this can compare.
			m_emptyReason = Outcome::ReferenceUnreadable;
			return;
		}

		const FileStore store(m_handle->filestorePath());
		// Only ever set on `UserRequested`, where an identical candidate is skipped instead of
		// ending the search. It is the difference between "you have nothing else" and "what you
		// have is already this very file", and the user is owed the second sentence.
		bool skippedIdentical = false;
		const std::vector<FootprintPartRef> refs =
			FootprintVariants::collect(m_handle->connection());

		// One entry per *distinct* footprint, with the parts that use it — the same grouping the
		// variant browser shows, because "used by" is what makes a candidate recognisable.
		std::vector<Candidate> candidates;
		std::vector<KicadDrawing> drawings;
		for (const FootprintPackageGroup& group : FootprintVariants::group(refs))
		{
			for (const FootprintVariant& variant : group.variants)
			{
				Candidate candidate;
				candidate.contentHash = QString::fromStdString(variant.contentHash);
				candidate.relativePath = QString::fromStdString(variant.relativePath);
				candidate.package = QString::fromStdString(group.package);   // user data
				QStringList names;
				for (const FootprintPartRef& part : variant.parts)
				{
					// The part being given a footprint is not a candidate for sharing with
					// itself — and on the vendor-archive path it is already holding the very
					// bytes being offered, which would make every suggestion look identical.
					if (part.partId == m_partId) { continue; }
					names << QString::fromStdString(part.partName);   // user data
					if (candidate.examplePartId == 0)
					{
						candidate.examplePartId = part.partId;
					}
				}
				if (candidate.examplePartId == 0)
				{
					// Nobody but this part uses that footprint, so there is nothing to share.
					continue;
				}
				candidate.usedBy = names.join(QStringLiteral(", "));

				const std::string text =
					readWholeFile(store.absolutePath(variant.relativePath));
				if (QByteArray(text.data(), static_cast<int>(text.size())) == m_reference)
				{
					if (m_trigger == Trigger::UserRequested)
					{
						// The user asked to see something *different*, and a byte-identical file
						// is the one candidate that provably is not — it is already the same
						// stored file. Drop the row, keep the list: ending the search here
						// would answer a deliberate button press with silence.
						skippedIdentical = true;
						continue;
					}
					// Byte-identical to the download: the content-addressed store will share it
					// on its own the moment these bytes are attached, so there is nothing to ask.
					return;
				}
				candidate.drawing = KicadGeometry::footprint(text);
				if (candidate.drawing.empty())
				{
					continue;
				}
				candidates.push_back(candidate);
				drawings.push_back(candidate.drawing);
			}
		}
		if (candidates.empty())
		{
			m_emptyReason = skippedIdentical ? Outcome::OnlyIdenticalOnes
				: Outcome::NoOtherFootprints;
			return;
		}
		// Something comparable existed, so from here on an empty list is the pad-count filter's
		// doing and nothing else.
		m_emptyReason = Outcome::AllFilteredOut;

		const std::vector<FootprintCandidate> ranked =
			FootprintCompatibility::rank(m_referenceDrawing, drawings);
		for (const FootprintCandidate& entry : ranked)
		{
			// A different number of pads is a different part, and no amount of looking at the
			// overlay changes that — so it never becomes a row. **This is not a retreat from
			// "ranked by score alone, never filtered to a pass list"** (see the header): that
			// rule is about the accept/reject tolerance, where the real margins are hundredths
			// of a millimetre and a badge the user can overrule beats a filter they cannot. A
			// pad count is not a margin, it is a count. Do not "restore" these rows.
			if (entry.comparison.padsA != entry.comparison.padsB)
			{
				continue;
			}
			Candidate candidate = candidates[entry.index];
			candidate.comparison = entry.comparison;
			m_candidates.push_back(candidate);
		}
		if (m_candidates.empty())
		{
			// Everything the database had was the wrong shape. `hasSuggestions()` reads this and
			// `offer()` returns without showing a dialog that would have nothing in it.
			return;
		}

		// Favourites first: a part being given a footprint nearly always wants one from its own
		// package, and ranking by score alone scattered those among strangers that happened to
		// measure closer. The score still orders the rows *inside* each group — the grouping
		// only decides which block they sit in.
		std::vector<size_t> favourites;
		std::vector<size_t> others;
		for (size_t i = 0; i < m_candidates.size(); ++i)
		{
			// An empty package is not a package to match on — `footprintNameFor()` falls back to
			// the part's own name for those, so they share nothing by package and there is no
			// favourites group at all.
			const bool favourite = !m_partPackage.isEmpty()
				&& m_candidates[i].package == m_partPackage;
			(favourite ? favourites : others).push_back(i);
		}
		if (!favourites.empty())
		{
			addCandidateGroup(tr("Same package (%1)",
				"footprint candidate group header; %1 is a package name such as 0603")
				.arg(m_partPackage), favourites);   // user data in %1
		}
		if (!others.empty())
		{
			addPackageSplitGroup(tr("Other packages", "footprint candidate group header"), others);
		}
	}

	void FootprintSuggestionDialog::addCandidateGroup(const QString& title,
		const std::vector<size_t>& indices)
	{
		QTreeWidgetItem* group = addHeading(nullptr, title);
		for (const size_t index : indices)
		{
			addCandidateRow(group, index);
		}
		// Expanded from the start: the groups are an ordering, not somewhere to put rows the
		// user then has to go looking for.
		group->setExpanded(true);
	}

	void FootprintSuggestionDialog::addPackageSplitGroup(const QString& title,
		const std::vector<size_t>& indices)
	{
		QTreeWidgetItem* group = addHeading(nullptr, title);
		// `indices` arrives in score order, which is what turns this into a single grouping pass
		// with no sort in it: a package is first seen at its own best-scoring candidate, so
		// keeping the nodes in first-seen order *is* ordering them by best fit. The rows inside
		// one keep the score order they came in with, exactly as the flat list did.
		std::vector<QString> packages;
		std::vector<QTreeWidgetItem*> nodes;
		for (const size_t index : indices)
		{
			const QString& package = m_candidates[index].package;   // user data
			size_t slot = 0;
			while (slot < packages.size() && packages[slot] != package) { ++slot; }
			if (slot == packages.size())
			{
				packages.push_back(package);
				// A part with no package still has a footprint worth offering, and it has to hang
				// somewhere — a node titled "" would read as a bug rather than as a fact about
				// the parts under it. Named the way the variant browser names the same case, and
				// ordered with the rest by its best score: an unnamed package is not a worse
				// match, and pushing it to the end would say that it is.
				nodes.push_back(addHeading(group, package.isEmpty()
					? tr("(no package)", "footprint candidate node for parts that have none")
					: package));
			}
			addCandidateRow(nodes[slot], index);
		}
		// Expanded after their rows exist, for the same reason the groups are expanded at all —
		// three levels of closed nodes would hide every choice behind two clicks.
		for (QTreeWidgetItem* node : nodes)
		{
			node->setExpanded(true);
		}
		group->setExpanded(true);
	}

	QTreeWidgetItem* FootprintSuggestionDialog::addHeading(QTreeWidgetItem* parent,
		const QString& title)
	{
		QTreeWidgetItem* item = parent == nullptr
			? new QTreeWidgetItem(m_ui->candidateTree)
			: new QTreeWidgetItem(parent);
		item->setText(0, title);
		item->setFirstColumnSpanned(true);
		QFont heading = item->font(0);
		heading.setBold(true);
		item->setFont(0, heading);
		// A heading is not a choice, so it is not selectable and the "Use this footprint" button
		// never sees it. It also carries no `CandidateIndexRole`: an int role that defaulted to 0
		// would quietly mean candidate zero. It stays **enabled** — disabling is the other way to
		// keep the keyboard off it, and it greys the row out until the tree reads as broken.
		// `eventFilter()` is what actually keeps Up/Down on the leaves.
		item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
		return item;
	}

	void FootprintSuggestionDialog::addCandidateRow(QTreeWidgetItem* parent, size_t index)
	{
		const Candidate& candidate = m_candidates[index];
		QTreeWidgetItem* item = new QTreeWidgetItem(parent);
		item->setText(0, candidate.usedBy);   // user data
		// The badge, never a filter: a footprint that missed the tolerance is still here and
		// still selectable, because the margin between the two verdicts is hundredths of a
		// millimetre and the user has the overlay in front of them.
		// Disambiguated: "Close" here means *nearly a match*, not *shut the window*, and
		// lupdate's same-text heuristic cheerfully offered "Schließen" for it.
		item->setText(1, candidate.comparison.compatible
			? tr("Compatible", "footprint fit verdict")
			: (candidate.comparison.score > 0.0
				? tr("Close", "footprint fit verdict: nearly compatible")
				: tr("No", "footprint fit verdict: not compatible at all")));
		// Pad counts always match here, so there is one measurement to report and no branch
		// on which reading applies — the mismatched ones never got this far.
		item->setText(2, tr("offset %1 mm · size %2 mm")
			.arg(millimetres(candidate.comparison.maxPositionOffsetMm),
				millimetres(candidate.comparison.maxSizeDeltaMm)));
		item->setData(0, CandidateIndexRole, static_cast<int>(index));
		QPixmap swatch(12, 12);
		swatch.fill(ExistingColour);
		item->setIcon(0, QIcon(swatch));
	}

	QTreeWidgetItem* FootprintSuggestionDialog::stepItem(QTreeWidgetItem* from, bool forward) const
	{
		QTreeWidget* tree = m_ui->candidateTree;
		if (forward)
		{
			if (from->isExpanded() && from->childCount() > 0) { return from->child(0); }
			// Out of this node and on to the next one that has a neighbour — from the last row of
			// "0805" that is the next package, and from the last row of the last package it is
			// whatever follows "Other packages", which is nothing.
			for (QTreeWidgetItem* at = from; at != nullptr; at = at->parent())
			{
				if (QTreeWidgetItem* next = siblingOf(tree, at, 1)) { return next; }
			}
			return nullptr;
		}
		if (QTreeWidgetItem* previous = siblingOf(tree, from, -1))
		{
			return lastVisibleDescendant(previous);
		}
		return from->parent();
	}

	QTreeWidgetItem* FootprintSuggestionDialog::nextCandidateItem(QTreeWidgetItem* from,
		bool forward) const
	{
		for (QTreeWidgetItem* at = stepItem(from, forward); at != nullptr;
			at = stepItem(at, forward))
		{
			if (isCandidateItem(at)) { return at; }
		}
		return nullptr;
	}

	QTreeWidgetItem* FootprintSuggestionDialog::firstCandidateItem() const
	{
		QTreeWidgetItem* first = m_ui->candidateTree->topLevelItem(0);
		if (first == nullptr) { return nullptr; }
		return isCandidateItem(first) ? first : nextCandidateItem(first, true);
	}

	QTreeWidgetItem* FootprintSuggestionDialog::lastCandidateItem() const
	{
		const int count = m_ui->candidateTree->topLevelItemCount();
		if (count == 0) { return nullptr; }
		QTreeWidgetItem* last =
			lastVisibleDescendant(m_ui->candidateTree->topLevelItem(count - 1));
		return isCandidateItem(last) ? last : nextCandidateItem(last, false);
	}

	void FootprintSuggestionDialog::moveToCandidate(bool forward)
	{
		QTreeWidgetItem* current = m_ui->candidateTree->currentItem();
		// No current row yet — which happens when every candidate scored zero and none was
		// proposed, and after a click on a heading. Either end of the list is then one press away.
		QTreeWidgetItem* target = current == nullptr
			? (forward ? firstCandidateItem() : lastCandidateItem())
			: nextCandidateItem(current, forward);
		// Nothing in that direction: stay put rather than clear the current row. Running off the
		// end of a list should feel like a wall, not like a deselect.
		if (target != nullptr)
		{
			m_ui->candidateTree->setCurrentItem(target);
		}
	}

	bool FootprintSuggestionDialog::eventFilter(QObject* watched, QEvent* event)
	{
		// **Clearing `ItemIsSelectable` is not enough and the user reported exactly that.** Qt
		// keeps *current item* and *selection* apart: `QTreeView::moveCursor()` steps to the next
		// visible row whatever its flags say, and only the selecting half of the key press
		// consults them — so Up/Down landed on the group headers with nothing selected, which is
		// the state that empties the panel. Disabling the headers would make Qt skip them, at the
		// price of greying out the one text that says what the block below it is. So the keys are
		// answered here instead, and the flags keep doing the job they are right for.
		//
		// Only Up and Down. Left and Right fall through untouched, which is what still collapses
		// and expands a group — and moving current onto a heading with Left is how the keyboard
		// reaches one to collapse it at all.
		if (watched == m_ui->candidateTree && event->type() == QEvent::KeyPress)
		{
			const int key = static_cast<QKeyEvent*>(event)->key();
			if (key == Qt::Key_Up || key == Qt::Key_Down)
			{
				moveToCandidate(key == Qt::Key_Down);
				return true;
			}
		}
		return QDialog::eventFilter(watched, event);
	}

	QString FootprintSuggestionDialog::referenceLegend() const
	{
		return m_trigger == Trigger::UserRequested
			? tr("Already assigned to this part", "footprint overlay legend, amber entry")
			: tr("The footprint just downloaded", "footprint overlay legend, amber entry");
	}

	void FootprintSuggestionDialog::showCandidate(int index)
	{
		std::vector<KicadVariantView::Entry> entries;
		KicadVariantView::Entry reference;
		reference.key = ReferenceKey;
		reference.label = m_referenceName;
		reference.legend = referenceLegend();
		reference.colour = ReferenceColour;
		reference.drawing = m_referenceDrawing;
		// Skipped only when there is nothing in it to draw, which leaves the view on its
		// placeholder rather than on a panel that is blank for no stated reason.
		if (!m_referenceDrawing.empty())
		{
			entries.push_back(reference);
		}

		if (index < 0 || index >= static_cast<int>(m_candidates.size()))
		{
			// **The reference stays on screen with nothing selected.** This used to `clear()` the
			// view, so deselecting — or landing on a heading — replaced the footprint the part is
			// actually carrying with an empty panel. The sentence goes *over* it now, which is
			// why the view draws it on a plate: it is competing with pads, not with a blank.
			m_view->setEntries(entries);
			m_view->setHighlighted(ReferenceKey);
			m_view->setHintText(tr("Pick a row on the left to see it against this one."));
			// The status line describes the current row, so it goes with it — a group header
			// left the last candidate's measurements standing under an unrelated overlay.
			m_ui->statusLabel->clear();
			return;
		}
		const Candidate& candidate = m_candidates[static_cast<size_t>(index)];

		KicadVariantView::Entry existing;
		existing.key = candidate.contentHash;
		existing.label = candidate.usedBy;
		existing.legend = tr("Selected in the list", "footprint overlay legend, blue entry");
		existing.colour = ExistingColour;
		existing.drawing = candidate.drawing;
		entries.push_back(existing);

		m_view->setEntries(entries);
		m_view->setHintText(QString());
		// **The reference is the constant, so it is the one drawn bright and on top.** It is the
		// only thing that does not change as the user walks the list; the candidates sweep
		// underneath it in their own colour and the eye compares each against a shape that
		// has not moved. Highlighting the candidate instead — which this did until the user ran
		// it on real data — redraws the reference behind every row and leaves nothing fixed to
		// judge against.
		m_view->setHighlighted(reference.key);

		// Built from the fields, **not** from `FootprintComparison::summary`. That string lives
		// in core/, which has no tr(), so showing it would leave the one line that explains the
		// verdict in English on a German screen — which is exactly what the metric's header
		// says not to do, and what this line did until it was seen in the app.
		//
		// Pad counts always match here — `buildCandidates()` drops the rest — so this is one
		// sentence plus its qualifiers, not a branch on whether the two are the same part.
		const FootprintComparison& fit = candidate.comparison;
		QString text = tr("Same %n pad(s). Largest position difference %1 mm, largest size "
			"difference %2 mm.", "", fit.padsA)
			.arg(millimetres(fit.maxPositionOffsetMm), millimetres(fit.maxSizeDeltaMm));
		if (fit.mountingMismatch)
		{
			text += QLatin1Char(' ') + tr("One is through-hole and the other is not, so they "
				"cannot replace each other.");
		}
		if (fit.shapeDiffers)
		{
			text += QLatin1Char(' ') + tr("Pad shapes differ (round against rectangular), "
				"which solders the same but looks different.");
		}
		if (!fit.matchedByPadNumber)
		{
			text += QLatin1Char(' ') + tr("Pads were paired by position because the two "
				"files do not use the same pad numbers.");
		}
		m_ui->statusLabel->setText(text);
	}

	void FootprintSuggestionDialog::onSelectionChanged(QTreeWidgetItem* current)
	{
		// A group header carries no `CandidateIndexRole`, and `toInt()` on an absent variant is 0
		// — which would hand the user candidate zero every time the current row was a header.
		// Test the variant, not the number it converts to.
		const QVariant index = current == nullptr ? QVariant()
			: current->data(0, CandidateIndexRole);
		m_chosen = index.isValid() ? index.toInt() : -1;
		m_useExisting->setEnabled(m_chosen >= 0);
		showCandidate(m_chosen);
	}

	void FootprintSuggestionDialog::onAccept()
	{
		if (m_chosen < 0)
		{
			reject();
			return;
		}
		m_tookExisting = true;
		accept();
	}

	bool FootprintSuggestionDialog::applyChosen(QWidget* parent, DatabaseHandle* handle,
		int partId, int sourcePartId)
	{
		// Re-point, never re-import. `useStoredFile()` updates this part's row to name the file
		// the other part already uses and deletes nothing — see its header for the 139-to-138
		// measurement that made it exist. It is also the call that makes the button path safe:
		// the row it rewrites may already name a file, which is the case it was written for, so
		// the footprint the part had before stays on disk as an ordinary orphan.
		SQLiteWrapper::SQLite& db = handle->connection();
		PartFile stored;
		if (!FileStore::roleFile(db, sourcePartId, PartFileRole::KicadFootprint, stored))
		{
			return false;
		}
		FileStore store(handle->filestorePath());
		std::string error;
		if (store.useStoredFile(db, partId, PartFileRole::KicadFootprint, stored, &error) == 0)
		{
			QMessageBox::warning(parent, tr("Could not use that footprint"),
				error.empty() ? tr("The database rejected the change.")
					: QString::fromStdString(error));
			return false;
		}
		return true;
	}

	bool FootprintSuggestionDialog::readAttachedFootprint(const PartEditorController& controller,
		int partId, QByteArray& outBytes, QString& outName)
	{
		PartFile attached;
		if (!controller.roleFile(partId, PartFileRole::KicadFootprint, attached))
		{
			return false;
		}
		// Read back from the store rather than kept from whatever put it there: an import's
		// entry went through the filestore, and what the part actually carries now is the only
		// thing worth comparing on either path.
		const std::string bytes =
			readWholeFile(controller.roleFilePath(partId, PartFileRole::KicadFootprint));
		if (bytes.empty())
		{
			return false;
		}
		outBytes = QByteArray(bytes.data(), static_cast<int>(bytes.size()));
		outName = QString::fromStdString(attached.originalFilename);   // user data
		return true;
	}

	bool FootprintSuggestionDialog::offer(QWidget* parent, DatabaseHandle* handle, int partId,
		const QByteArray& downloaded, const QString& downloadedName)
	{
		if (handle == nullptr || partId == 0 || downloaded.isEmpty())
		{
			return false;
		}
		FootprintSuggestionDialog dialog(handle, partId, downloaded, downloadedName,
			Trigger::AfterImport, parent);
		if (!dialog.hasSuggestions())
		{
			// Nothing comparable in the database, or it is already there byte for byte. Either
			// way the attach that follows is the right thing and the user is not interrupted.
			return false;
		}
		if (dialog.exec() != QDialog::Accepted || dialog.chosenSourcePartId() == 0)
		{
			return false;
		}
		return applyChosen(parent, handle, partId, dialog.chosenSourcePartId());
	}

	void FootprintSuggestionDialog::offerAfterArchiveImport(QWidget* parent,
		const PartEditorController& controller, int partId, bool footprintAttached)
	{
		if (!footprintAttached)
		{
			return;
		}
		QByteArray bytes;
		QString name;
		if (!readAttachedFootprint(controller, partId, bytes, name))
		{
			return;
		}
		// The result is ignored on purpose — see the header. `offer()` has already done the
		// re-point when it returns true, and there is no pending attach for false to release.
		offer(parent, controller.handle(), partId, bytes, name);
	}

	FootprintSuggestionDialog::Outcome FootprintSuggestionDialog::offerReplacement(QWidget* parent,
		const PartEditorController& controller, int partId)
	{
		DatabaseHandle* handle = controller.handle();
		if (handle == nullptr || partId == 0)
		{
			return Outcome::NoFootprintAttached;
		}
		QByteArray bytes;
		QString name;
		if (!readAttachedFootprint(controller, partId, bytes, name))
		{
			return Outcome::NoFootprintAttached;
		}

		FootprintSuggestionDialog dialog(handle, partId, bytes, name, Trigger::UserRequested,
			parent);
		if (!dialog.hasSuggestions())
		{
			// Handed straight back rather than reported here: the dialog says nothing when it
			// has nothing to say, and which sentence the user gets is the call site's business.
			return dialog.emptyReason();
		}
		if (dialog.exec() != QDialog::Accepted || dialog.chosenSourcePartId() == 0)
		{
			return Outcome::Declined;
		}
		// A refused re-point has already shown its own warning and changed nothing, which is
		// what `Declined` means to the caller — it must not report a second time.
		return applyChosen(parent, handle, partId, dialog.chosenSourcePartId())
			? Outcome::Replaced : Outcome::Declined;
	}

}
