#include "ui/PartManager_CategoryExportDialog.h"
#include "ui_PartManager_CategoryExportDialog.h"

#include "controllers/PartManager_MainWindowController.h"   // buildCategoryTree
#include "controllers/PartManager_PartEditorController.h"   // types()
#include "domain/PartManager_TypeIcon.h"
#include "import/PartManager_PartTypeTransfer.h"
#include "widgets/PartManager_TypeIconPainter.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QSaveFile>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace PartManager
{
	namespace
	{
		constexpr int TypeIdRole = Qt::UserRole;

		// The extension §2's bundles carry. One place, because the filter, the default file name
		// and the suffix appended to a name typed without one all have to agree.
		const QString& bundleSuffix()
		{
			static const QString suffix = QStringLiteral(".pmcat");
			return suffix;
		}
	}

	CategoryExportDialog::CategoryExportDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::CategoryExportDialog)
		, m_handle(handle)
		, m_types(PartEditorController(handle).types())
	{
		m_ui->setupUi(this);

		// Said once here rather than per item: the glyphs are drawn at exactly this size and a view
		// whose icon size disagrees scales them soft — the same reason the main window's tree and
		// the type picker both set it.
		m_ui->categoryTree->setIconSize(QSize(CategoryGlyphSize, CategoryGlyphSize));

		m_exportButton = m_ui->buttonBox->addButton(tr("Export…"), QDialogButtonBox::AcceptRole);

		m_updating = true;
		for (const CategoryNode& root : buildCategoryTree(m_types, std::map<int, int>()))
		{
			addTypeItem(root, nullptr);
		}
		m_updating = false;
		m_ui->categoryTree->expandToDepth(0);

		connect(m_ui->categoryTree, &QTreeWidget::itemChanged,
			this, &CategoryExportDialog::onItemChanged);
		connect(m_exportButton, &QPushButton::clicked, this, &CategoryExportDialog::onExport);
		// Close, not accept: this dialog produces a file, not a result the caller reads back, and
		// its own button box has no OK to be mistaken for one.
		connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

		auto setAll = [this](Qt::CheckState state)
		{
			m_updating = true;
			for (int i = 0; i < m_ui->categoryTree->topLevelItemCount(); ++i)
			{
				QTreeWidgetItem* root = m_ui->categoryTree->topLevelItem(i);
				root->setCheckState(0, state);
				std::vector<QTreeWidgetItem*> stack(1, root);
				while (!stack.empty())
				{
					QTreeWidgetItem* item = stack.back();
					stack.pop_back();
					item->setCheckState(0, state);
					for (int child = 0; child < item->childCount(); ++child)
					{
						stack.push_back(item->child(child));
					}
				}
			}
			m_updating = false;
			updateExportState();
		};
		connect(m_ui->selectAllButton, &QPushButton::clicked, this,
			[setAll]() { setAll(Qt::Checked); });
		connect(m_ui->selectNoneButton, &QPushButton::clicked, this,
			[setAll]() { setAll(Qt::Unchecked); });

		updateExportState();
	}

	CategoryExportDialog::~CategoryExportDialog()
	{
		delete m_ui;
	}

	int CategoryExportDialog::exportedCount() const
	{
		return m_exportedCount;
	}

	const QString& CategoryExportDialog::exportedPath() const
	{
		return m_exportedPath;
	}

	void CategoryExportDialog::addTypeItem(const CategoryNode& node, QTreeWidgetItem* parent)
	{
		QTreeWidgetItem* item = parent != nullptr
			? new QTreeWidgetItem(parent)
			: new QTreeWidgetItem(m_ui->categoryTree);

		// The user's own category name — never tr()'d.
		item->setText(0, node.name);
		item->setData(0, TypeIdRole, node.typeId);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		// Everything ticked to start with: exporting the whole tree is the common case, and the
		// alternative is a user ticking a hundred boxes before anything happens.
		item->setCheckState(0, Qt::Checked);

		// The same rule the main window's tree paints by: a type that classifies as Generic gets no
		// glyph, since a column of look-alike initials says nothing the name does not.
		if (TypeIconStyle::forType(node.name.toStdString()).glyph != TypeGlyph::Generic)
		{
			item->setIcon(0, TypeIconPainter::icon(node.name, CategoryGlyphSize, devicePixelRatioF()));
		}

		for (const CategoryNode& child : node.children)
		{
			addTypeItem(child, item);
		}
	}

	void CategoryExportDialog::onItemChanged(QTreeWidgetItem* item, int column)
	{
		if (m_updating || item == nullptr || column != 0)
		{
			return;
		}

		// Every setCheckState() below re-enters this slot. The guard is what makes one user click
		// one cascade instead of a cascade per box the cascade touched.
		m_updating = true;
		const Qt::CheckState state = item->checkState(0);
		if (state != Qt::PartiallyChecked)
		{
			// Down: a category turned off takes its subtypes with it — they inherit from it and
			// would arrive parentless — and one turned on brings them along, which is what the box
			// looks like it promises.
			std::vector<QTreeWidgetItem*> stack(1, item);
			while (!stack.empty())
			{
				QTreeWidgetItem* current = stack.back();
				stack.pop_back();
				for (int child = 0; child < current->childCount(); ++child)
				{
					current->child(child)->setCheckState(0, state);
					stack.push_back(current->child(child));
				}
			}
		}
		refreshAncestors(item);
		m_updating = false;

		updateExportState();
	}

	void CategoryExportDialog::refreshAncestors(QTreeWidgetItem* item)
	{
		for (QTreeWidgetItem* ancestor = item->parent(); ancestor != nullptr;
			ancestor = ancestor->parent())
		{
			bool anyIn = false;
			bool allIn = true;
			for (int child = 0; child < ancestor->childCount(); ++child)
			{
				const Qt::CheckState childState = ancestor->child(child)->checkState(0);
				anyIn = anyIn || childState != Qt::Unchecked;
				allIn = allIn && childState == Qt::Checked;
			}
			// §2b: a child in the bundle drags its parent in, so "any child in" is already enough
			// to make the ancestor part of the export. Whether that shows as checked or partially
			// checked is only about how much of *its* subtree came along.
			ancestor->setCheckState(0, anyIn
				? (allIn ? Qt::Checked : Qt::PartiallyChecked)
				: Qt::Unchecked);
		}
	}

	void CategoryExportDialog::collectSelected(QTreeWidgetItem* item, std::vector<int>& out) const
	{
		if (item->checkState(0) != Qt::Unchecked)
		{
			out.push_back(item->data(0, TypeIdRole).toInt());
		}
		for (int child = 0; child < item->childCount(); ++child)
		{
			collectSelected(item->child(child), out);
		}
	}

	std::vector<int> CategoryExportDialog::selectedTypeIds() const
	{
		std::vector<int> ids;
		for (int i = 0; i < m_ui->categoryTree->topLevelItemCount(); ++i)
		{
			collectSelected(m_ui->categoryTree->topLevelItem(i), ids);
		}
		return ids;
	}

	void CategoryExportDialog::updateExportState()
	{
		const int count = static_cast<int>(selectedTypeIds().size());
		m_exportButton->setEnabled(count > 0);
		m_exportButton->setText(count > 0
			? tr("Export %n category(ies)…", "", count)
			: tr("Export…"));
	}

	void CategoryExportDialog::onExport()
	{
		const std::vector<int> ids = selectedTypeIds();
		if (ids.empty() || m_handle == nullptr || !m_handle->isOpen())
		{
			return;
		}

		// The database's own name in the suggested file name: a folder of exports from three
		// databases is otherwise three files called the same thing.
		const QString databaseName =
			QFileInfo(QString::fromStdString(m_handle->pmdbPath())).absoluteDir().dirName();
		const QString suggestion = QDir(QDir::homePath()).filePath(
			databaseName + QStringLiteral(" categories") + bundleSuffix());

		QString path = QFileDialog::getSaveFileName(this, tr("Export categories"), suggestion,
			tr("PartManager categories (*.pmcat)"));
		if (path.isEmpty())
		{
			return;
		}
		// A name typed without the extension still has to be one of ours — fromJson() and every
		// file dialog that reads these recognise the suffix, not the content.
		if (!path.endsWith(bundleSuffix(), Qt::CaseInsensitive))
		{
			path += bundleSuffix();
		}

		const PartTypeBundle bundle = bundleFromDatabase(m_handle->connection(), ids);
		const std::string json = toJson(bundle);

		// QSaveFile rather than QFile: a failure halfway through a write leaves the previous
		// export intact instead of a truncated file that looks like one.
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		{
			m_ui->statusLabel->setText(tr("Could not write %1 — %2.")
				.arg(QDir::toNativeSeparators(path), file.errorString()));
			return;
		}
		// UTF-8 explicitly: category names and attribute labels are the user's own text and are
		// routinely not ascii, and toJson() already produced UTF-8 bytes.
		const QByteArray payload = QByteArray::fromStdString(json);
		if (file.write(payload) != payload.size() || !file.commit())
		{
			m_ui->statusLabel->setText(tr("Could not write %1 — %2.")
				.arg(QDir::toNativeSeparators(path), file.errorString()));
			return;
		}

		// bundleFromDatabase() adds ancestors of its own accord, so the number written is its
		// answer and not the tick count — the two agree here, but only because the tree keeps
		// them in step, and reporting the bundle's own count cannot drift from the file.
		m_exportedCount = static_cast<int>(bundle.nodes.size());
		m_exportedPath = path;
		// A status line, not a modal: exporting a second selection is a normal next step and a
		// message box would stand between the user and the tree they would pick it from.
		m_ui->statusLabel->setText(tr("Exported %n category(ies) to %1.", "", m_exportedCount)
			.arg(QDir::toNativeSeparators(path)));
	}

}
