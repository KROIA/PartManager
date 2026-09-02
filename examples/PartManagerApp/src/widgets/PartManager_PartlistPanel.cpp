#include "widgets/PartManager_PartlistPanel.h"
#include "ui_PartManager_PartlistPanel.h"

#include "ui/PartManager_OrderManagerDialog.h"
#include "ui/PartManager_PartlistImportDialog.h"

#include <QComboBox>
#include <QDataStream>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QMessageBox>
#include <QMimeData>
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

	const char* const PartlistPanel::PartMimeType = "application/x-qabstractitemmodeldatalist";

	PartlistPanel::PartlistPanel(DatabaseHandle* handle, QWidget* parent)
		: QWidget(parent)
		, m_ui(new Ui::PartlistPanel)
		, m_controller(handle)
		, m_saveTimer(new QTimer(this))
	{
		m_ui->setupUi(this);
		setAcceptDrops(true);

		m_saveTimer->setSingleShot(true);
		m_saveTimer->setInterval(HeaderSaveDebounceMs);
		connect(m_saveTimer, &QTimer::timeout, this, &PartlistPanel::autosaveHeader);

		m_ui->itemTable->setColumnCount(ColumnCount);
		m_ui->itemTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Designators") << tr("Part") << tr("Qty/unit") << tr("Qty total")
			<< tr("In stock") << tr("Still needed"));
		m_ui->itemTable->horizontalHeader()->setSectionResizeMode(ColumnPart, QHeaderView::Stretch);

		connect(m_ui->nameEdit, &QLineEdit::textChanged, this, &PartlistPanel::scheduleHeaderSave);
		connect(m_ui->projectLinkEdit, &QLineEdit::textChanged, this, &PartlistPanel::scheduleHeaderSave);
		connect(m_ui->descriptionEdit, &QPlainTextEdit::textChanged, this, &PartlistPanel::scheduleHeaderSave);
		// The multiplier changes every row's needed quantity, so it saves immediately and reloads
		// rather than waiting out the debounce with stale numbers on screen.
		connect(m_ui->multiplierSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &PartlistPanel::autosaveHeader);
		connect(m_ui->openProjectLinkButton, &QPushButton::clicked,
			this, &PartlistPanel::openProjectLink);

		connect(m_ui->itemTable, &QTableWidget::itemChanged, this, &PartlistPanel::onCellChanged);
		connect(m_ui->itemTable, &QTableWidget::itemSelectionChanged,
			this, &PartlistPanel::updateButtons);
		connect(m_ui->addLineButton, &QPushButton::clicked, this, &PartlistPanel::addLine);
		connect(m_ui->removeLineButton, &QPushButton::clicked, this, &PartlistPanel::removeLine);
		connect(m_ui->orderShortfallButton, &QPushButton::clicked,
			this, &PartlistPanel::orderShortfall);

		connect(m_ui->partlistCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, [this](int)
			{
				if (m_loading)
				{
					return;
				}
				// Switching lists mid-word must not lose the word.
				flushPendingEdits();
				m_partlist.id = m_ui->partlistCombo->currentData().toInt();
				reload();
			});
		connect(m_ui->newListButton, &QPushButton::clicked, this, [this]() { createPartlist(); });
		connect(m_ui->importListButton, &QPushButton::clicked, this, &PartlistPanel::importPartlist);
		connect(m_ui->deleteListButton, &QPushButton::clicked, this, &PartlistPanel::deletePartlist);
		connect(m_ui->hideButton, &QPushButton::clicked, this, &PartlistPanel::hideRequested);

		reloadPartlists();
	}

	PartlistPanel::~PartlistPanel()
	{
		delete m_ui;
	}

	void PartlistPanel::reloadPartlists()
	{
		const int wanted = m_partlist.id;

		const bool wasLoading = m_loading;
		m_loading = true;
		m_ui->partlistCombo->clear();
		for (const Partlist& list : m_controller.partlists())
		{
			// The list's name is the user's own data; the item count beside it is chrome.
			m_ui->partlistCombo->addItem(tr("%1  (%n line(s))", "", m_controller.itemCount(list.id))
				.arg(QString::fromStdString(list.name)), list.id);
		}
		const int index = m_ui->partlistCombo->findData(wanted);
		m_ui->partlistCombo->setCurrentIndex(index >= 0 ? index : 0);
		m_partlist.id = m_ui->partlistCombo->currentData().toInt();
		m_loading = wasLoading;

		const bool any = m_ui->partlistCombo->count() > 0;
		m_ui->bodyWidget->setVisible(any);
		m_ui->emptyLabel->setVisible(!any);
		m_ui->deleteListButton->setEnabled(any);
		m_ui->partlistCombo->setEnabled(any);
		m_ui->dropHintLabel->setVisible(any);
		if (any)
		{
			reload();
		}
	}

	void PartlistPanel::showPartlist(int partlistId)
	{
		m_partlist.id = partlistId;
		reloadPartlists();
	}

	int PartlistPanel::createPartlist()
	{
		Partlist partlist;
		partlist.name = tr("New partlist").toStdString();
		partlist.source = PartlistSource::Manual;
		const int id = m_controller.create(partlist);
		if (id == NoPartlistId)
		{
			QMessageBox::warning(this, tr("Could not create the partlist"),
				tr("The database rejected the new partlist."));
			return NoPartlistId;
		}
		showPartlist(id);
		// The name is a placeholder, so the first thing to do with it is replace it.
		m_ui->nameEdit->setFocus();
		m_ui->nameEdit->selectAll();
		return id;
	}

	void PartlistPanel::importPartlist()
	{
		PartlistImportDialog import(m_controller, this);
		if (import.exec() != QDialog::Accepted)
		{
			return;
		}
		// Straight into the grid: an import that left rows unresolved is exactly what the user
		// has to look at next.
		showPartlist(import.createdPartlistId());
	}

	void PartlistPanel::deletePartlist()
	{
		if (m_partlist.id == NoPartlistId)
		{
			return;
		}
		if (QMessageBox::question(this, tr("Delete this partlist?"),
			tr("“%1” and its %n line(s) will be removed. The parts themselves are not touched.",
				"", static_cast<int>(m_items.size()))
				.arg(QString::fromStdString(m_partlist.name)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}
		// A pending header write would recreate the row it is being deleted from under it.
		m_saveTimer->stop();
		m_controller.remove(m_partlist.id);
		m_partlist = Partlist();
		reloadPartlists();
	}

	void PartlistPanel::flushPendingEdits()
	{
		if (m_saveTimer->isActive())
		{
			m_saveTimer->stop();
			autosaveHeader();
		}
	}

	void PartlistPanel::hideEvent(QHideEvent* event)
	{
		flushPendingEdits();
		QWidget::hideEvent(event);
	}

	void PartlistPanel::closeEvent(QCloseEvent* event)
	{
		flushPendingEdits();
		QWidget::closeEvent(event);
	}

	int PartlistPanel::droppedPartId(const QMimeData* mime)
	{
		if (mime == nullptr || !mime->hasFormat(PartMimeType))
		{
			return 0;
		}
		// Qt's own item-view payload: one (row, column, roles) triple per dragged index. The
		// part table hangs the part id off Qt::UserRole of column 0, which is the cell the
		// selection and the tag chips already ride on.
		QByteArray encoded = mime->data(PartMimeType);
		QDataStream stream(&encoded, QIODevice::ReadOnly);
		while (!stream.atEnd())
		{
			int row = 0;
			int column = 0;
			QMap<int, QVariant> roles;
			stream >> row >> column >> roles;
			const int partId = roles.value(Qt::UserRole).toInt();
			if (partId != 0)
			{
				return partId;
			}
		}
		return 0;
	}

	void PartlistPanel::dragEnterEvent(QDragEnterEvent* event)
	{
		// Refused while no list is open: there would be nowhere to put the line, and a drop that
		// silently does nothing is worse than a cursor that says no.
		if (m_partlist.id != NoPartlistId && droppedPartId(event->mimeData()) != 0)
		{
			event->acceptProposedAction();
		}
	}

	void PartlistPanel::dragMoveEvent(QDragMoveEvent* event)
	{
		if (m_partlist.id != NoPartlistId && droppedPartId(event->mimeData()) != 0)
		{
			event->acceptProposedAction();
		}
	}

	void PartlistPanel::dropEvent(QDropEvent* event)
	{
		const int partId = droppedPartId(event->mimeData());
		if (partId == 0 || m_partlist.id == NoPartlistId)
		{
			return;
		}
		event->acceptProposedAction();
		addPart(partId);
	}

	void PartlistPanel::addPart(int partId)
	{
		// A part the list already carries gets one more of itself rather than a second row.
		// Two rows for the same part are legal (a BOM really does list one resistor on several
		// lines) but they are never what a drag means, and each row would then measure its own
		// shortfall against the same untouched stock.
		for (size_t i = 0; i < m_items.size(); ++i)
		{
			if (m_items[i].partId == partId)
			{
				m_items[i].quantityPerUnit += 1;
				m_controller.saveItems(m_partlist.id, m_items);
				reload();
				m_ui->itemTable->selectRow(static_cast<int>(i));
				return;
			}
		}

		PartlistItem item;
		item.partlistId = m_partlist.id;
		item.partId = partId;
		item.quantityPerUnit = 1;
		m_items.push_back(item);
		m_controller.saveItems(m_partlist.id, m_items);
		reload();
		m_ui->itemTable->selectRow(m_ui->itemTable->rowCount() - 1);
	}

	void PartlistPanel::reload()
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
		m_ui->headerGroup->setEnabled(true);
		m_ui->itemTable->setEnabled(true);
		m_ui->addLineButton->setEnabled(true);

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

	void PartlistPanel::showLines(const std::vector<PartlistLine>& lines)
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
			// The combo is still here for keyboard use and for repairing an import, but it is no
			// longer the only way in — dragging a row out of the part table above is.
			picker->setToolTip(tr("Or drag the part straight out of the table above."));
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

	void PartlistPanel::refreshComputedColumns(const std::vector<PartlistLine>& lines)
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
					cell->setToolTip(tr("This line points at no part yet. Drag one out of the table "
						"above, or pick it in the Part column — until then it cannot be ordered and "
						"is left out of “Order Missing Parts”."));
				}
				else if (paintShortfall)
				{
					cell->setBackground(ShortfallRowColor);
					cell->setToolTip(tr("Short by %n unit(s): the list needs %1 and stock holds %2.",
						"", line.shortfallQty).arg(line.neededQty).arg(line.stockQty));
				}
				else
				{
					cell->setBackground(QBrush());
					cell->setToolTip(QString());
				}
			}
		}

		m_loading = wasLoading;
		m_ui->statusLabel->setText(partlistStatusSummary(lines));
	}

	void PartlistPanel::scheduleHeaderSave()
	{
		if (m_loading)
		{
			return;
		}
		m_saveTimer->start();
	}

	void PartlistPanel::autosaveHeader()
	{
		if (m_loading || m_partlist.id == NoPartlistId)
		{
			return;
		}
		m_saveTimer->stop();

		const int previousMultiplier = m_partlist.multiplier;
		const std::string previousName = m_partlist.name;
		m_partlist.name = m_ui->nameEdit->text().trimmed().toStdString();
		m_partlist.projectLinkUrl = m_ui->projectLinkEdit->text().trimmed().toStdString();
		m_partlist.description = m_ui->descriptionEdit->toPlainText().toStdString();
		m_partlist.multiplier = m_ui->multiplierSpin->value();
		m_controller.save(m_partlist);

		m_ui->openProjectLinkButton->setEnabled(!m_partlist.projectLinkUrl.empty());

		if (m_partlist.name != previousName)
		{
			// The selector shows the name, so it has to follow — but rebuilding it would fire
			// currentIndexChanged and reload the list out from under the field being typed in.
			const bool wasLoading = m_loading;
			m_loading = true;
			m_ui->partlistCombo->setItemText(m_ui->partlistCombo->currentIndex(),
				tr("%1  (%n line(s))", "", static_cast<int>(m_items.size()))
					.arg(QString::fromStdString(m_partlist.name)));
			m_loading = wasLoading;
		}

		if (m_partlist.multiplier != previousMultiplier)
		{
			// Every row's needed quantity just changed; the picker widgets did not, so only the
			// computed columns are rewritten and whatever had focus keeps it.
			refreshComputedColumns(m_controller.lines(m_partlist.id));
		}
	}

	void PartlistPanel::saveLines()
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

	void PartlistPanel::onCellChanged(QTableWidgetItem* item)
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

	void PartlistPanel::addLine()
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

	void PartlistPanel::removeLine()
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

	void PartlistPanel::updateButtons()
	{
		m_ui->removeLineButton->setEnabled(m_ui->itemTable->currentRow() >= 0
			&& !m_items.empty());
	}

	void PartlistPanel::openProjectLink()
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

	void PartlistPanel::orderShortfall()
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
		// Confirming an arrival in there restocks, which moves every shortfall on this screen —
		// and every stock count in the table above it.
		reload();
		emit stockChanged();
	}

}
