#include "ui/PartManager_PartlistEditorDialog.h"
#include "ui_PartManager_PartlistEditorDialog.h"

#include "ui/PartManager_OrderManagerDialog.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QHeaderView>
#include <QMessageBox>
#include <QSpinBox>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>

namespace PartManager
{
	namespace
	{
		// Same debounce the part editor uses, for the same reason: typing a name should not be
		// one UPDATE per keystroke.
		constexpr int HeaderSaveDebounceMs = 400;

		// §4 paints an unresolved or short row orange. Kept light enough that the black text on
		// top stays readable in both Qt themes this ships with.
		const QColor UnresolvedRowColor(0xFB, 0xE1, 0x8F);
		const QColor ShortfallRowColor(0xF7, 0xD3, 0xC4);

		enum Column
		{
			ColumnDesignators = 0,
			ColumnPart,
			ColumnQtyPerUnit,
			ColumnQtyTotal,
			ColumnInStock,
			ColumnShortfall,
			ColumnCount
		};
	}

	PartlistEditorDialog::PartlistEditorDialog(const PartlistController& controller, int partlistId,
		QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::PartlistEditorDialog)
		, m_controller(controller)
		, m_saveTimer(new QTimer(this))
	{
		m_ui->setupUi(this);
		m_partlist.id = partlistId;

		m_saveTimer->setSingleShot(true);
		m_saveTimer->setInterval(HeaderSaveDebounceMs);
		connect(m_saveTimer, &QTimer::timeout, this, &PartlistEditorDialog::autosaveHeader);

		m_ui->itemTable->setColumnCount(ColumnCount);
		m_ui->itemTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Designators") << tr("Part") << tr("Qty/unit") << tr("Qty total")
			<< tr("In stock") << tr("Still needed"));
		m_ui->itemTable->horizontalHeader()->setSectionResizeMode(ColumnPart, QHeaderView::Stretch);

		connect(m_ui->nameEdit, &QLineEdit::textChanged, this, &PartlistEditorDialog::scheduleHeaderSave);
		connect(m_ui->projectLinkEdit, &QLineEdit::textChanged, this, &PartlistEditorDialog::scheduleHeaderSave);
		connect(m_ui->descriptionEdit, &QPlainTextEdit::textChanged, this, &PartlistEditorDialog::scheduleHeaderSave);
		// The multiplier changes every row's needed quantity, so it saves immediately and reloads
		// rather than waiting out the debounce with stale numbers on screen.
		connect(m_ui->multiplierSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &PartlistEditorDialog::autosaveHeader);
		connect(m_ui->openProjectLinkButton, &QPushButton::clicked,
			this, &PartlistEditorDialog::openProjectLink);

		connect(m_ui->itemTable, &QTableWidget::itemChanged, this, &PartlistEditorDialog::onCellChanged);
		connect(m_ui->itemTable, &QTableWidget::itemSelectionChanged,
			this, &PartlistEditorDialog::updateButtons);
		connect(m_ui->addLineButton, &QPushButton::clicked, this, &PartlistEditorDialog::addLine);
		connect(m_ui->removeLineButton, &QPushButton::clicked, this, &PartlistEditorDialog::removeLine);
		connect(m_ui->orderShortfallButton, &QPushButton::clicked,
			this, &PartlistEditorDialog::orderShortfall);
		connect(m_ui->closeButton, &QPushButton::clicked, this, &PartlistEditorDialog::accept);

		reload();
	}

	PartlistEditorDialog::~PartlistEditorDialog()
	{
		delete m_ui;
	}

	void PartlistEditorDialog::done(int result)
	{
		if (m_saveTimer->isActive())
		{
			m_saveTimer->stop();
			autosaveHeader();
		}
		QDialog::done(result);
	}

	void PartlistEditorDialog::reload()
	{
		m_loading = true;
		if (!m_controller.load(m_partlist.id, m_partlist))
		{
			m_ui->headerGroup->setEnabled(false);
			m_ui->itemTable->setEnabled(false);
			m_ui->addLineButton->setEnabled(false);
			m_ui->statusLabel->setText(tr("This partlist no longer exists."));
			m_loading = false;
			return;
		}

		setWindowTitle(tr("Partlist — %1").arg(QString::fromStdString(m_partlist.name)));
		m_ui->nameEdit->setText(QString::fromStdString(m_partlist.name));
		m_ui->projectLinkEdit->setText(QString::fromStdString(m_partlist.projectLinkUrl));
		m_ui->descriptionEdit->setPlainText(QString::fromStdString(m_partlist.description));
		m_ui->multiplierSpin->setValue(m_partlist.multiplier);
		m_ui->sourceLabel->setText(partlistSourceLabel(m_partlist.source));
		m_ui->openProjectLinkButton->setEnabled(!m_partlist.projectLinkUrl.empty());

		const std::vector<PartlistLine> lines = m_controller.lines(m_partlist.id);
		m_items.clear();
		for (const PartlistLine& line : lines)
		{
			m_items.push_back(line.item);
		}
		showLines(lines);
		m_loading = false;
	}

	void PartlistEditorDialog::showLines(const std::vector<PartlistLine>& lines)
	{
		const bool wasLoading = m_loading;
		m_loading = true;

		m_ui->itemTable->clearContents();
		m_ui->itemTable->setRowCount(static_cast<int>(lines.size()));

		const std::vector<Part> parts = m_controller.allParts();
		for (size_t i = 0; i < lines.size(); ++i)
		{
			const int row = static_cast<int>(i);
			const PartlistItem& item = lines[i].item;

			m_ui->itemTable->setItem(row, ColumnDesignators,
				new QTableWidgetItem(QString::fromStdString(item.designators)));

			QComboBox* picker = new QComboBox(m_ui->itemTable);
			// §4's unresolved state is a real choice, not an absence — it has to be selectable so
			// a mis-matched import row can be put back.
			picker->addItem(tr("— not matched —"), NoPartId);
			for (const Part& part : parts)
			{
				picker->addItem(partPickerLabel(part), part.id);
			}
			const int index = picker->findData(item.partId);
			picker->setCurrentIndex(index >= 0 ? index : 0);
			connect(picker, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[this, row, picker](int)
				{
					if (m_loading || row >= static_cast<int>(m_items.size()))
					{
						return;
					}
					m_items[static_cast<size_t>(row)].partId = picker->currentData().toInt();
					saveLines();
				});
			m_ui->itemTable->setCellWidget(row, ColumnPart, picker);

			QSpinBox* quantity = new QSpinBox(m_ui->itemTable);
			quantity->setRange(1, 1000000);
			quantity->setValue(item.quantityPerUnit < 1 ? 1 : item.quantityPerUnit);
			connect(quantity, QOverload<int>::of(&QSpinBox::valueChanged), this,
				[this, row](int value)
				{
					if (m_loading || row >= static_cast<int>(m_items.size()))
					{
						return;
					}
					m_items[static_cast<size_t>(row)].quantityPerUnit = value;
					saveLines();
				});
			m_ui->itemTable->setCellWidget(row, ColumnQtyPerUnit, quantity);

			for (int column = ColumnQtyTotal; column < ColumnCount; ++column)
			{
				QTableWidgetItem* cell = new QTableWidgetItem();
				cell->setFlags(cell->flags() & ~Qt::ItemIsEditable);
				m_ui->itemTable->setItem(row, column, cell);
			}
		}

		m_loading = wasLoading;
		refreshComputedColumns(lines);
		m_ui->itemTable->resizeColumnsToContents();
		m_ui->itemTable->horizontalHeader()->setSectionResizeMode(ColumnPart, QHeaderView::Stretch);
		updateButtons();
	}

	void PartlistEditorDialog::refreshComputedColumns(const std::vector<PartlistLine>& lines)
	{
		const bool wasLoading = m_loading;
		m_loading = true;

		for (size_t i = 0; i < lines.size() && static_cast<int>(i) < m_ui->itemTable->rowCount(); ++i)
		{
			const PartlistLine& line = lines[i];
			const int row = static_cast<int>(i);

			// An unresolved row has nothing to compare against, so it shows dashes rather than a
			// confident "0 in stock, 0 short" that would read as "nothing to do here".
			const QString total = QString::number(line.neededQty);
			const QString stock = line.resolved ? QString::number(line.stockQty) : tr("—");
			const QString shortfall = line.resolved ? QString::number(line.shortfallQty) : tr("—");
			const QString values[3] = { total, stock, shortfall };
			for (int offset = 0; offset < 3; ++offset)
			{
				QTableWidgetItem* cell = m_ui->itemTable->item(row, ColumnQtyTotal + offset);
				if (cell != nullptr)
				{
					cell->setText(values[offset]);
				}
			}

			const bool paintUnresolved = !line.resolved;
			const bool paintShortfall = line.resolved && line.shortfallQty > 0;
			for (int column = 0; column < ColumnCount; ++column)
			{
				QTableWidgetItem* cell = m_ui->itemTable->item(row, column);
				if (cell == nullptr)
				{
					continue;   // the part picker and the spin box are widgets, not items
				}
				if (paintUnresolved)
				{
					cell->setBackground(UnresolvedRowColor);
				}
				else if (paintShortfall)
				{
					cell->setBackground(ShortfallRowColor);
				}
				else
				{
					cell->setBackground(QBrush());
				}
			}
		}

		m_loading = wasLoading;
		m_ui->statusLabel->setText(partlistStatusSummary(lines));
	}

	void PartlistEditorDialog::scheduleHeaderSave()
	{
		if (m_loading)
		{
			return;
		}
		m_saveTimer->start();
	}

	void PartlistEditorDialog::autosaveHeader()
	{
		if (m_loading || m_partlist.id == NoPartlistId)
		{
			return;
		}
		m_saveTimer->stop();

		const int previousMultiplier = m_partlist.multiplier;
		m_partlist.name = m_ui->nameEdit->text().trimmed().toStdString();
		m_partlist.projectLinkUrl = m_ui->projectLinkEdit->text().trimmed().toStdString();
		m_partlist.description = m_ui->descriptionEdit->toPlainText().toStdString();
		m_partlist.multiplier = m_ui->multiplierSpin->value();
		m_controller.save(m_partlist);

		setWindowTitle(tr("Partlist — %1").arg(QString::fromStdString(m_partlist.name)));
		m_ui->openProjectLinkButton->setEnabled(!m_partlist.projectLinkUrl.empty());

		if (m_partlist.multiplier != previousMultiplier)
		{
			// Every row's needed quantity just changed; the picker widgets did not, so only the
			// computed columns are rewritten and whatever had focus keeps it.
			refreshComputedColumns(m_controller.lines(m_partlist.id));
		}
	}

	void PartlistEditorDialog::saveLines()
	{
		if (m_loading || m_partlist.id == NoPartlistId)
		{
			return;
		}
		m_controller.saveItems(m_partlist.id, m_items);

		// saveItems() replaces the rows, so the ids the grid holds are stale — re-read them along
		// with the recomputed quantities, keeping the widgets in place.
		const std::vector<PartlistLine> lines = m_controller.lines(m_partlist.id);
		m_items.clear();
		for (const PartlistLine& line : lines)
		{
			m_items.push_back(line.item);
		}
		refreshComputedColumns(lines);
	}

	void PartlistEditorDialog::onCellChanged(QTableWidgetItem* item)
	{
		if (m_loading || item == nullptr || item->column() != ColumnDesignators)
		{
			return;
		}
		const size_t row = static_cast<size_t>(item->row());
		if (row >= m_items.size())
		{
			return;
		}
		// Designators are the user's own text (`R1,R2,R5`), stored verbatim — core never parses them.
		m_items[row].designators = item->text().toStdString();
		saveLines();
	}

	void PartlistEditorDialog::addLine()
	{
		if (m_partlist.id == NoPartlistId)
		{
			return;
		}
		PartlistItem item;
		item.partlistId = m_partlist.id;
		item.quantityPerUnit = 1;
		m_items.push_back(item);
		m_controller.saveItems(m_partlist.id, m_items);
		reload();
		m_ui->itemTable->selectRow(m_ui->itemTable->rowCount() - 1);
	}

	void PartlistEditorDialog::removeLine()
	{
		const int row = m_ui->itemTable->currentRow();
		if (row < 0 || row >= static_cast<int>(m_items.size()))
		{
			return;
		}
		// No confirmation: one line is cheap to re-add, and Add Line is right next to the button.
		m_items.erase(m_items.begin() + row);
		m_controller.saveItems(m_partlist.id, m_items);
		reload();
	}

	void PartlistEditorDialog::updateButtons()
	{
		m_ui->removeLineButton->setEnabled(m_ui->itemTable->currentRow() >= 0
			&& !m_items.empty());
	}

	void PartlistEditorDialog::openProjectLink()
	{
		const QString link = m_ui->projectLinkEdit->text().trimmed();
		if (link.isEmpty())
		{
			return;
		}
		// A link typed as "github.com/…" is not a URL QDesktopServices can open; assume https
		// rather than silently doing nothing.
		QUrl url(link);
		if (url.scheme().isEmpty())
		{
			url = QUrl(QStringLiteral("https://") + link);
		}
		QDesktopServices::openUrl(url);
	}

	void PartlistEditorDialog::orderShortfall()
	{
		// The grid's edits are written on the spot, but the header debounce may still be pending
		// and the multiplier drives every needed quantity — flush it before measuring anything.
		autosaveHeader();

		OrderController orders(m_controller.handle());
		const OrderDraftPreview preview = orders.previewFromPartlist(m_partlist.id);

		if (preview.lines.empty())
		{
			QMessageBox::information(this, tr("Nothing to order"),
				preview.skippedUnresolved > 0
					? tr("Everything that is matched to a part is already in stock. "
						 "%n line(s) still point at no part and could not be checked.",
						 "", preview.skippedUnresolved)
					: tr("Every line on this list is already covered by what is in stock."));
			return;
		}

		QStringList summary;
		for (const OrderLine& line : preview.lines)
		{
			summary.append(tr("%1 × %2%3")
				.arg(line.item.quantityOrdered)
				.arg(QString::fromStdString(line.partName))
				.arg(line.stageable ? QString() : tr("  (no Mouser part number)")));
		}
		if (preview.skippedUnresolved > 0)
		{
			// Named, not dropped: an unresolved row is the one thing that silently under-orders
			// a build, and the user is the only one who can fix it.
			summary.append(tr("Not included: %n line(s) that point at no part yet.",
				"", preview.skippedUnresolved));
		}

		if (QMessageBox::question(this, tr("Raise a draft order?"),
			tr("This list is short of:\n\n%1\n\nCreate a draft order for it?")
				.arg(summary.join(QStringLiteral("\n"))),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		{
			return;
		}

		const int orderId = orders.createDraftFromPartlist(m_partlist.id);
		if (orderId == NoOrderId)
		{
			QMessageBox::warning(this, tr("Could not create the order"),
				tr("The database rejected the new order."));
			return;
		}

		OrderManagerDialog dialog(m_controller.handle(), this);
		dialog.selectOrder(orderId);
		dialog.exec();
		// Confirming an arrival in there restocks, which moves every shortfall on this screen.
		reload();
	}

}
