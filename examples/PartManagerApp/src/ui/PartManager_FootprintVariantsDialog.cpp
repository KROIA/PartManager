#include "ui/PartManager_FootprintVariantsDialog.h"
#include "ui_PartManager_FootprintVariantsDialog.h"

#include "widgets/PartManager_KicadVariantView.h"
#include "filestore/PartManager_FileStore.h"
#include "kicad/PartManager_KicadGeometry.h"

#include <fstream>
#include <iterator>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace PartManager
{
	namespace
	{
		// Item roles: which group a row belongs to, and which variant of it.
		constexpr int GroupIndexRole = Qt::UserRole;
		constexpr int VariantKeyRole = Qt::UserRole + 1;

		std::string readWholeFile(const std::string& path)
		{
			std::ifstream stream(path, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>());
		}

		QString shortHash(const std::string& hash)
		{
			return QString::fromStdString(hash).left(8);
		}
	}

	FootprintVariantsDialog::FootprintVariantsDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_handle(handle)
		, m_ui(new Ui::FootprintVariantsDialog)
	{
		m_ui->setupUi(this);
		// The splitter takes the height; the two wrapping labels around it would otherwise grow
		// into it and leave the tree and the drawing sharing a strip in the middle.
		m_ui->mainLayout->setStretch(2, 1);

		m_view = new KicadVariantView(m_ui->viewHost);
		m_ui->viewLayout->addWidget(m_view);

		m_ui->modeCombo->addItem(tr("Overlaid and side by side"),
			static_cast<int>(KicadVariantView::Mode::Both));
		m_ui->modeCombo->addItem(tr("Overlaid"),
			static_cast<int>(KicadVariantView::Mode::Overlay));
		m_ui->modeCombo->addItem(tr("Side by side"),
			static_cast<int>(KicadVariantView::Mode::SideBySide));

		m_ui->variantTree->header()->setStretchLastSection(false);
		m_ui->variantTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
		m_ui->variantTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		// The whole point of the screen: the pointer moving over a row brings that variant to
		// the front. entered() is only emitted with mouse tracking on, which the .ui sets.
		m_ui->variantTree->viewport()->setMouseTracking(true);
		m_ui->variantTree->viewport()->installEventFilter(this);

		connect(m_ui->variantTree, &QTreeWidget::currentItemChanged, this,
			[this](QTreeWidgetItem* current, QTreeWidgetItem*) { onCurrentItemChanged(current); });
		connect(m_ui->variantTree, &QTreeWidget::itemEntered,
			this, &FootprintVariantsDialog::onItemEntered);
		connect(m_ui->modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &FootprintVariantsDialog::onModeChanged);
		connect(m_ui->onlyClashesCheck, &QCheckBox::toggled, this,
			&FootprintVariantsDialog::reload);
		connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

		reload();
	}

	FootprintVariantsDialog::~FootprintVariantsDialog()
	{
		delete m_ui;
	}

	QColor FootprintVariantsDialog::colourForIndex(int index)
	{
		// Distinguishable on the dark board background and from each other — the question the
		// colour answers is "which of these is which", so neighbouring hues would defeat it.
		static const QColor palette[] = {
			QColor(0x4F, 0xC3, 0xF7),   // blue
			QColor(0xFF, 0xB3, 0x4D),   // amber
			QColor(0x81, 0xC7, 0x84),   // green
			QColor(0xE5, 0x73, 0x73),   // red
			QColor(0xBA, 0x9C, 0xE8),   // violet
			QColor(0xF0, 0xE6, 0x8C),   // khaki
		};
		constexpr int count = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
		return palette[((index % count) + count) % count];
	}

	void FootprintVariantsDialog::reload()
	{
		m_ui->variantTree->clear();
		m_view->clear();
		m_shownGroup = -1;
		m_selectedVariant.clear();
		m_groups.clear();
		if (m_handle == nullptr || !m_handle->isOpen())
		{
			m_ui->summaryLabel->setText(tr("No database is open."));
			return;
		}

		const std::vector<FootprintPackageGroup> all =
			FootprintVariants::group(FootprintVariants::collect(m_handle->connection()));
		int clashing = 0;
		int variants = 0;
		for (const FootprintPackageGroup& group : all)
		{
			variants += static_cast<int>(group.variants.size());
			if (!group.consistent()) { ++clashing; }
		}
		m_groups = m_ui->onlyClashesCheck->isChecked()
			? FootprintVariants::inconsistentOnly(all) : all;

		// Counted over everything, not over what the filter left: the number of packages that
		// still disagree is the progress bar for this job, and hiding the settled ones must not
		// change it.
		m_ui->summaryLabel->setText(tr("%n package(s) with a footprint", "", static_cast<int>(all.size()))
			+ tr(", %n of them claimed by different pad layouts", "", clashing)
			+ tr(" — %n distinct footprint(s) in total.", "", variants));

		for (size_t index = 0; index < m_groups.size(); ++index)
		{
			const FootprintPackageGroup& group = m_groups[index];
			QTreeWidgetItem* packageItem = new QTreeWidgetItem(m_ui->variantTree);
			packageItem->setText(0, group.package.empty()
				? tr("(no package)")
				: QString::fromStdString(group.package));   // user data
			packageItem->setText(1, QString::number(group.partCount()));
			packageItem->setData(0, GroupIndexRole, static_cast<int>(index));
			// Said in words rather than by an icon: "consistent" is the state most packages are
			// in and the user is scanning for the exception.
			packageItem->setToolTip(0, group.package.empty()
				? tr("These parts have no package, so each writes a footprint file named after "
					"itself. Nothing here is in conflict.")
				: (group.consistent()
					? tr("Every part with this package uses the same footprint.")
					: tr("%n different footprint(s) under one package.", "",
						static_cast<int>(group.variants.size()))));

			for (size_t v = 0; v < group.variants.size(); ++v)
			{
				const FootprintVariant& variant = group.variants[v];
				QTreeWidgetItem* variantItem = new QTreeWidgetItem(packageItem);
				variantItem->setText(0, tr("Variant %1").arg(shortHash(variant.contentHash)));
				variantItem->setText(1, QString::number(variant.parts.size()));
				variantItem->setData(0, GroupIndexRole, static_cast<int>(index));
				variantItem->setData(0, VariantKeyRole,
					QString::fromStdString(variant.contentHash));
				// The same colour the overlay draws it in — without this the tree and the
				// picture are two lists the user has to correlate by counting.
				QPixmap swatch(12, 12);
				swatch.fill(colourForIndex(static_cast<int>(v)));
				variantItem->setIcon(0, QIcon(swatch));

				for (const FootprintPartRef& part : variant.parts)
				{
					QTreeWidgetItem* partItem = new QTreeWidgetItem(variantItem);
					partItem->setText(0, QString::fromStdString(part.partName));   // user data
					partItem->setData(0, GroupIndexRole, static_cast<int>(index));
					partItem->setData(0, VariantKeyRole,
						QString::fromStdString(variant.contentHash));
				}
			}
			// Expanded for a package that needs a decision, collapsed for one that does not:
			// the tree then opens on exactly the work list.
			packageItem->setExpanded(!group.consistent());
		}

		if (m_ui->variantTree->topLevelItemCount() > 0)
		{
			m_ui->variantTree->setCurrentItem(m_ui->variantTree->topLevelItem(0));
		}
	}

	void FootprintVariantsDialog::showPackage(int groupIndex, const QString& variantKey)
	{
		if (groupIndex < 0 || groupIndex >= static_cast<int>(m_groups.size()))
		{
			m_view->clear();
			m_shownGroup = -1;
			return;
		}
		if (groupIndex == m_shownGroup)
		{
			highlight(variantKey);
			return;
		}

		const FootprintPackageGroup& group = m_groups[static_cast<size_t>(groupIndex)];
		const FileStore store(m_handle->filestorePath());

		std::vector<KicadVariantView::Entry> entries;
		for (size_t v = 0; v < group.variants.size(); ++v)
		{
			const FootprintVariant& variant = group.variants[v];
			KicadVariantView::Entry entry;
			entry.key = QString::fromStdString(variant.contentHash);
			entry.colour = colourForIndex(static_cast<int>(v));
			entry.label = tr("%1 · %n part(s)", "", static_cast<int>(variant.parts.size()))
				.arg(shortHash(variant.contentHash));
			// Read straight from the store rather than from `kicad_libs/`: the generated bundle
			// only holds the footprints that won a name, and this screen is about the ones that
			// did not. The attachment is where every variant exists.
			const std::string path = store.absolutePath(variant.relativePath);
			if (!path.empty())
			{
				entry.drawing = KicadGeometry::footprint(readWholeFile(path));
			}
			entries.push_back(entry);
		}
		m_view->setEntries(entries);
		m_shownGroup = groupIndex;
		highlight(variantKey.isEmpty() && !entries.empty() ? entries.front().key : variantKey);
	}

	void FootprintVariantsDialog::highlight(const QString& variantKey)
	{
		m_view->setHighlighted(variantKey);
		if (variantKey.isEmpty() || m_shownGroup < 0)
		{
			m_ui->statusLabel->clear();
			return;
		}
		const FootprintPackageGroup& group = m_groups[static_cast<size_t>(m_shownGroup)];
		for (const FootprintVariant& variant : group.variants)
		{
			if (QString::fromStdString(variant.contentHash) != variantKey) { continue; }
			QStringList names;
			for (const FootprintPartRef& part : variant.parts)
			{
				names << QString::fromStdString(part.partName);   // user data
			}
			// Named, not counted: these are the parts a reassignment would move, and the user
			// is deciding whether that is the right set before any such gesture exists.
			m_ui->statusLabel->setText(tr("Variant %1: %2")
				.arg(shortHash(variant.contentHash), names.join(QStringLiteral(", "))));
			return;
		}
	}

	void FootprintVariantsDialog::onCurrentItemChanged(QTreeWidgetItem* current)
	{
		if (current == nullptr)
		{
			m_selectedVariant.clear();
			return;
		}
		m_selectedVariant = current->data(0, VariantKeyRole).toString();
		showPackage(current->data(0, GroupIndexRole).toInt(), m_selectedVariant);
	}

	void FootprintVariantsDialog::onItemEntered(QTreeWidgetItem* item, int column)
	{
		Q_UNUSED(column);
		if (item == nullptr) { return; }
		const int groupIndex = item->data(0, GroupIndexRole).toInt();
		const QString key = item->data(0, VariantKeyRole).toString();
		// Hovering a package row loads that package but highlights nothing of its own — the
		// selection below puts one back in front, so passing over a header does not blank the
		// view on the way to the rows under it.
		showPackage(groupIndex, key.isEmpty() ? m_selectedVariant : key);
	}

	void FootprintVariantsDialog::onModeChanged()
	{
		m_view->setMode(static_cast<KicadVariantView::Mode>(
			m_ui->modeCombo->currentData().toInt()));
	}

	bool FootprintVariantsDialog::eventFilter(QObject* watched, QEvent* event)
	{
		if (watched == m_ui->variantTree->viewport() && event->type() == QEvent::Leave)
		{
			// Back to the selected row. Falling back to *nothing* would leave the overlay with
			// every variant dimmed every time the pointer crossed the gap to the picture.
			highlight(m_selectedVariant);
		}
		return QDialog::eventFilter(watched, event);
	}

}
