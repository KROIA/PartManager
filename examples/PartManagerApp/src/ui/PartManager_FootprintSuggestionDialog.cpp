#include "ui/PartManager_FootprintSuggestionDialog.h"
#include "ui_PartManager_FootprintSuggestionDialog.h"

#include "widgets/PartManager_KicadVariantView.h"
#include "filestore/PartManager_FileStore.h"
#include "kicad/PartManager_FootprintVariants.h"
#include "kicad/PartManager_KicadGeometry.h"

#include <fstream>
#include <iterator>

#include <QDialogButtonBox>
#include <QMessageBox>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace PartManager
{
	namespace
	{
		constexpr int CandidateIndexRole = Qt::UserRole;

		// The download and the candidate get one colour each, and they keep it in the tree, the
		// legend and the overlay — "which of these is which" has to be answerable at a glance.
		const QColor DownloadedColour(0xFF, 0xB3, 0x4D);   // amber
		const QColor ExistingColour(0x4F, 0xC3, 0xF7);     // blue

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
		const QByteArray& downloaded, const QString& downloadedName, QWidget* parent)
		: QDialog(parent)
		, m_handle(handle)
		, m_partId(partId)
		, m_downloaded(downloaded)
		, m_downloadedName(downloadedName)
		, m_ui(new Ui::FootprintSuggestionDialog)
	{
		m_ui->setupUi(this);
		m_ui->mainLayout->setStretch(1, 1);

		m_view = new KicadVariantView(m_ui->viewHost);
		// Overlay only: two footprints, and the question is where they differ. The side-by-side
		// tiles answer "what are they", which the user already knows — one is the download.
		m_view->setMode(KicadVariantView::Mode::Overlay);
		m_ui->viewLayout->addWidget(m_view);

		m_ui->candidateTree->header()->setStretchLastSection(false);
		m_ui->candidateTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
		m_ui->candidateTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		m_ui->candidateTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

		QPushButton* useExisting = m_ui->buttonBox->addButton(tr("Use this footprint"),
			QDialogButtonBox::AcceptRole);
		m_ui->buttonBox->addButton(tr("Keep the downloaded one"), QDialogButtonBox::RejectRole);
		connect(useExisting, &QPushButton::clicked, this, &FootprintSuggestionDialog::onAccept);
		connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

		m_downloadedDrawing = KicadGeometry::footprint(
			std::string(m_downloaded.constData(), static_cast<size_t>(m_downloaded.size())));
		buildCandidates();

		connect(m_ui->candidateTree, &QTreeWidget::currentItemChanged, this,
			[this](QTreeWidgetItem* current, QTreeWidgetItem*) { onSelectionChanged(current); });

		if (!m_candidates.empty())
		{
			// The best fit is pre-selected — unless it scored zero, which means "never suggest
			// this" (a through-hole land pattern against a surface-mount one). Bottom of the
			// list is a fair place for those; "best fit" is not.
			const int preselect = m_candidates.front().comparison.score > 0.0 ? 0 : -1;
			if (preselect >= 0)
			{
				m_ui->candidateTree->setCurrentItem(m_ui->candidateTree->topLevelItem(0));
			}
			else
			{
				useExisting->setEnabled(false);
			}
		}

		m_ui->headerLabel->setText(
			tr("“%1” looks like %n footprint(s) this database already has. Using one of them means "
				"both parts share a single file — check the overlay first: the download is drawn "
				"in amber, the one you are looking at in blue.", "",
				static_cast<int>(m_candidates.size())).arg(m_downloadedName));
	}

	FootprintSuggestionDialog::~FootprintSuggestionDialog()
	{
		delete m_ui;
	}

	int FootprintSuggestionDialog::chosenSourcePartId() const
	{
		if (!m_tookExisting || m_chosen < 0)
		{
			return 0;
		}
		return m_candidates[static_cast<size_t>(m_chosen)].examplePartId;
	}

	void FootprintSuggestionDialog::buildCandidates()
	{
		if (m_handle == nullptr || !m_handle->isOpen() || m_downloadedDrawing.empty())
		{
			return;
		}

		const FileStore store(m_handle->filestorePath());
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
				// Byte-identical to the download: the content-addressed store will share it on
				// its own the moment these bytes are attached, so there is nothing to ask.
				if (QByteArray(text.data(), static_cast<int>(text.size())) == m_downloaded)
				{
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
			return;
		}

		const std::vector<FootprintCandidate> ranked =
			FootprintCompatibility::rank(m_downloadedDrawing, drawings);
		for (const FootprintCandidate& entry : ranked)
		{
			Candidate candidate = candidates[entry.index];
			candidate.comparison = entry.comparison;
			m_candidates.push_back(candidate);
		}

		for (size_t i = 0; i < m_candidates.size(); ++i)
		{
			const Candidate& candidate = m_candidates[i];
			QTreeWidgetItem* item = new QTreeWidgetItem(m_ui->candidateTree);
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
			item->setText(2, candidate.comparison.padsA == candidate.comparison.padsB
				? tr("offset %1 mm · size %2 mm")
					.arg(millimetres(candidate.comparison.maxPositionOffsetMm),
						millimetres(candidate.comparison.maxSizeDeltaMm))
				: tr("%1 pads vs %2")
					.arg(candidate.comparison.padsB).arg(candidate.comparison.padsA));
			item->setData(0, CandidateIndexRole, static_cast<int>(i));
			QPixmap swatch(12, 12);
			swatch.fill(ExistingColour);
			item->setIcon(0, QIcon(swatch));
		}
	}

	void FootprintSuggestionDialog::showCandidate(int index)
	{
		if (index < 0 || index >= static_cast<int>(m_candidates.size()))
		{
			m_view->clear();
			return;
		}
		const Candidate& candidate = m_candidates[static_cast<size_t>(index)];

		std::vector<KicadVariantView::Entry> entries;
		KicadVariantView::Entry downloaded;
		downloaded.key = QStringLiteral("downloaded");
		downloaded.label = m_downloadedName;
		downloaded.colour = DownloadedColour;
		downloaded.drawing = m_downloadedDrawing;
		entries.push_back(downloaded);

		KicadVariantView::Entry existing;
		existing.key = candidate.contentHash;
		existing.label = candidate.usedBy;
		existing.colour = ExistingColour;
		existing.drawing = candidate.drawing;
		entries.push_back(existing);

		m_view->setEntries(entries);
		// The candidate is the one being judged, so it goes on top; the download stays behind it
		// as the constant reference.
		m_view->setHighlighted(candidate.contentHash);

		// Built from the fields, **not** from `FootprintComparison::summary`. That string lives
		// in core/, which has no tr(), so showing it would leave the one line that explains the
		// verdict in English on a German screen — which is exactly what the metric's header
		// says not to do, and what this line did until it was seen in the app.
		const FootprintComparison& fit = candidate.comparison;
		QString text;
		if (fit.padsA != fit.padsB)
		{
			text = tr("%1 pads against %2 — a different number of pads is a different part.")
				.arg(fit.padsB).arg(fit.padsA);
		}
		else
		{
			text = tr("Same %n pad(s). Largest position difference %1 mm, largest size "
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
		}
		m_ui->statusLabel->setText(text);
	}

	void FootprintSuggestionDialog::onSelectionChanged(QTreeWidgetItem* current)
	{
		m_chosen = current == nullptr ? -1 : current->data(0, CandidateIndexRole).toInt();
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

	bool FootprintSuggestionDialog::offer(QWidget* parent, DatabaseHandle* handle, int partId,
		const QByteArray& downloaded, const QString& downloadedName)
	{
		if (handle == nullptr || partId == 0 || downloaded.isEmpty())
		{
			return false;
		}
		FootprintSuggestionDialog dialog(handle, partId, downloaded, downloadedName, parent);
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

		// Re-point, never re-import. `useStoredFile()` updates this part's row to name the file
		// the other part already uses and deletes nothing — see its header for the 139-to-138
		// measurement that made it exist.
		SQLiteWrapper::SQLite& db = handle->connection();
		PartFile stored;
		if (!FileStore::roleFile(db, dialog.chosenSourcePartId(),
			PartFileRole::KicadFootprint, stored))
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

}
