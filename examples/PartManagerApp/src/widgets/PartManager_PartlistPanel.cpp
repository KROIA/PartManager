#include "widgets/PartManager_PartlistPanel.h"
#include "ui_PartManager_PartlistPanel.h"

#include "controllers/PartManager_PartEditorController.h"
#include "ui/PartManager_OrderManagerDialog.h"
#include "ui/PartManager_PartEditorDialog.h"
#include "ui/PartManager_PartPickerDialog.h"
#include "ui/PartManager_PartlistImportDialog.h"

#include <QComboBox>
#include <QDataStream>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
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

		// How tall a grid row is once it carries a thumbnail, and how big that thumbnail is drawn.
		constexpr int ThumbnailSize = 28;

		enum Column
		{
			ColumnDesignators = 0,
			ColumnImage,
			ColumnPart,
			ColumnQtyPerUnit,
			ColumnQtyTotal,
			ColumnInStock,
			ColumnShortfall,
			ColumnRemove,
			ColumnCount
		};

		// The per-row delete button: red enough to read as "this destroys something", flat so a
		// column of them does not look like a column of dialogs.
		const char* const RemoveButtonStyle =
			"QPushButton { color: #c0392b; font-weight: bold; border: none; background: transparent; }"
			"QPushButton:hover { color: #ffffff; background: #c0392b; border-radius: 3px; }";
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

		// All the extra height goes to the grid. Without this the header fields and the two labels
		// grow with the dock and the rows — the only part worth more space — stay put.
		m_ui->mainLayout->setStretch(1, 1);
		m_ui->bodyLayout->setStretch(1, 1);

		m_saveTimer->setSingleShot(true);
		m_saveTimer->setInterval(HeaderSaveDebounceMs);
		connect(m_saveTimer, &QTimer::timeout, this, &PartlistPanel::autosaveHeader);

		m_ui->itemTable->setColumnCount(ColumnCount);
		m_ui->itemTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Designators") << QString() << tr("Part") << tr("Qty/unit") << tr("Qty total")
			<< tr("In stock") << tr("Still needed") << QString());
		m_ui->itemTable->horizontalHeader()->setSectionResizeMode(ColumnPart, QHeaderView::Stretch);
		m_ui->itemTable->horizontalHeader()->setSectionResizeMode(ColumnImage, QHeaderView::Fixed);
		m_ui->itemTable->horizontalHeader()->setSectionResizeMode(ColumnRemove, QHeaderView::Fixed);
		m_ui->itemTable->setColumnWidth(ColumnImage, ThumbnailSize + 6);
		m_ui->itemTable->setColumnWidth(ColumnRemove, 28);
		m_ui->itemTable->setIconSize(QSize(ThumbnailSize, ThumbnailSize));

		// Rows are dragged to reorder them, and a part dragged out of the browser may well be
		// aimed at the grid rather than at the panel around it. Both land in eventFilter().
		m_ui->itemTable->setDragEnabled(true);
		m_ui->itemTable->setAcceptDrops(true);
		m_ui->itemTable->viewport()->setAcceptDrops(true);
		m_ui->itemTable->setDragDropMode(QAbstractItemView::DragDrop);
		m_ui->itemTable->setDropIndicatorShown(true);
		m_ui->itemTable->viewport()->installEventFilter(this);

		connect(m_ui->nameEdit, &QLineEdit::textChanged, this, &PartlistPanel::scheduleHeaderSave);
		connect(m_ui->projectLinkEdit, &QLineEdit::textChanged, this, &PartlistPanel::scheduleHeaderSave);
		connect(m_ui->descriptionEdit, &QPlainTextEdit::textChanged, this, &PartlistPanel::scheduleHeaderSave);
		// The multiplier changes every row's needed quantity, so it saves immediately and reloads
		// rather than waiting out the debounce with stale numbers on screen.
		connect(m_ui->multiplierSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &PartlistPanel::autosaveHeader);
		connect(m_ui->openProjectLinkButton, &QPushButton::clicked,
			this, &PartlistPanel::openProjectLink);

		m_ui->itemTable->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(m_ui->itemTable, &QTableWidget::customContextMenuRequested,
			this, &PartlistPanel::showRowMenu);

		connect(m_ui->itemTable, &QTableWidget::itemChanged, this, &PartlistPanel::onCellChanged);
		connect(m_ui->itemTable, &QTableWidget::itemSelectionChanged,
			this, &PartlistPanel::updateButtons);
		connect(m_ui->addLineButton, &QPushButton::clicked, this, &PartlistPanel::addLine);
		// Double-clicking the Part cell is how an unresolved import row gets repaired, now that
		// the column is text rather than a combo box over every part in the database.
		connect(m_ui->itemTable, &QTableWidget::cellDoubleClicked, this,
			[this](int row, int column)
			{
				if (column == ColumnPart)
				{
					PartPickerDialog picker(m_controller, this);
					if (picker.exec() == QDialog::Accepted)
					{
						assignPart(row, picker.selectedPartId());
					}
				}
			});
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

	bool PartlistPanel::eventFilter(QObject* watched, QEvent* event)
	{
		if (m_ui->itemTable == nullptr || watched != m_ui->itemTable->viewport())
		{
			return QWidget::eventFilter(watched, event);
		}

		switch (event->type())
		{
		case QEvent::DragEnter:
		case QEvent::DragMove:
		{
			// QDragEnterEvent and QDragMoveEvent are both QDropEvents, and everything this needs
			// (the source widget and the payload) lives on that base.
			QDropEvent* drag = static_cast<QDropEvent*>(event);
			const bool ours = drag->source() == m_ui->itemTable;
			if (m_partlist.id != NoPartlistId && (ours || droppedPartId(drag->mimeData()) != 0))
			{
				// **Always a copy, even for a reorder.** A drag the view started and that ends in
				// Qt::MoveAction makes QAbstractItemView::startDrag() delete the dragged row's
				// items on the way out — after this handler has already rebuilt the grid, so it
				// wiped whichever row had landed on that index. Reordering is done on m_items
				// here; the view must not also try to do it.
				drag->setDropAction(Qt::CopyAction);
				drag->accept();
				return true;
			}
			return true;   // handled: refusing here keeps the panel below from re-offering it
		}
		case QEvent::Drop:
		{
			QDropEvent* drop = static_cast<QDropEvent*>(event);
			if (m_partlist.id == NoPartlistId)
			{
				return true;
			}
			if (drop->source() == m_ui->itemTable)
			{
				// Dropped past the last row (or on empty space below it) means "put it last".
				const QModelIndex target = m_ui->itemTable->indexAt(drop->pos());
				const int to = target.isValid()
					? target.row() : static_cast<int>(m_items.size()) - 1;
				drop->setDropAction(Qt::CopyAction);   // see the DragMove branch
				drop->accept();
				moveRow(m_ui->itemTable->currentRow(), to);
				return true;
			}
			const int partId = droppedPartId(drop->mimeData());
			if (partId != 0)
			{
				drop->setDropAction(Qt::CopyAction);
				drop->accept();
				addPart(partId);
			}
			return true;
		}
		default:
			break;
		}
		return QWidget::eventFilter(watched, event);
	}

	void PartlistPanel::addPart(int partId)
	{
		if (partId == NoPartId || m_partlist.id == NoPartlistId)
		{
			return;
		}
		// A part the list already carries gets one more of itself rather than a second row: each
		// row would otherwise measure its own shortfall against the same untouched stock, and the
		// build would be under-ordered. mergeDuplicateItems() is what actually collapses it, so
		// adding, repairing an import row and reading a BOM back all obey the same rule.
		PartlistItem item;
		item.partlistId = m_partlist.id;
		item.partId = partId;
		item.quantityPerUnit = 1;
		m_items.push_back(item);
		saveMergedItems();

		for (size_t i = 0; i < m_items.size(); ++i)
		{
			if (m_items[i].partId == partId)
			{
				m_ui->itemTable->selectRow(static_cast<int>(i));
				break;
			}
		}
	}

	void PartlistPanel::assignPart(int row, int partId)
	{
		if (row < 0 || row >= static_cast<int>(m_items.size()) || partId == NoPartId)
		{
			return;
		}
		m_items[static_cast<size_t>(row)].partId = partId;
		// If another row already holds that part, this one is folded into it — designators and all.
		saveMergedItems();
	}

	void PartlistPanel::removeRow(int row)
	{
		if (row < 0 || row >= static_cast<int>(m_items.size()))
		{
			return;
		}
		// No confirmation: one line is cheap to re-add, and the part itself is untouched.
		m_items.erase(m_items.begin() + row);
		m_controller.saveItems(m_partlist.id, m_items);
		reload();
	}

	void PartlistPanel::moveRow(int from, int to)
	{
		const int count = static_cast<int>(m_items.size());
		if (from < 0 || from >= count || to < 0 || to >= count || from == to)
		{
			return;
		}
		const PartlistItem moved = m_items[static_cast<size_t>(from)];
		m_items.erase(m_items.begin() + from);
		m_items.insert(m_items.begin() + to, moved);
		// Row order is the list's own order — saveItems() rewrites the rows in the order given,
		// so nothing else has to know that a drag happened.
		m_controller.saveItems(m_partlist.id, m_items);
		reload();
		m_ui->itemTable->selectRow(to);
	}

	void PartlistPanel::saveMergedItems()
	{
		mergeDuplicateItems(m_items);
		m_controller.saveItems(m_partlist.id, m_items);
		reload();
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
		// "Manual" on its own says nothing; the caption's explanation follows the value, because
		// the value is what anyone puzzled by it will hover.
		m_ui->sourceLabel->setToolTip(m_ui->sourceCaptionLabel->toolTip());
		m_ui->openProjectLinkButton->setEnabled(!m_partlist.projectLinkUrl.empty());

		std::vector<PartlistLine> lines = m_controller.lines(m_partlist.id);
		m_items.clear();
		for (const PartlistLine& line : lines)
		{
			m_items.push_back(line.item);
		}
		// One row per part, enforced on the way in as well as on the way out: a BOM import can
		// perfectly well list the same part on three lines, and those three are one position with
		// three designators, not three positions each measured against the same stock.
		if (mergeDuplicateItems(m_items))
		{
			m_controller.saveItems(m_partlist.id, m_items);
			lines = m_controller.lines(m_partlist.id);
			m_items.clear();
			for (const PartlistLine& line : lines)
			{
				m_items.push_back(line.item);
			}
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

		// One query for the whole grid; a row with no picture simply gets no icon.
		const std::map<int, QString> images = m_controller.imagePaths();
		for (size_t i = 0; i < lines.size(); ++i)
		{
			const int row = static_cast<int>(i);
			const PartlistItem& item = lines[i].item;

			m_ui->itemTable->setItem(row, ColumnDesignators,
				new QTableWidgetItem(QString::fromStdString(item.designators)));

			// The photo, same file the browser's table paints — a BOM is read by recognising
			// parts, and "0603 4k7" and "0603 47k" are one character apart in text.
			QTableWidgetItem* image = new QTableWidgetItem();
			image->setFlags(image->flags() & ~Qt::ItemIsEditable);
			const std::map<int, QString>::const_iterator found = images.find(item.partId);
			if (found != images.end() && !found->second.isEmpty())
			{
				const QPixmap picture(found->second);
				if (!picture.isNull())
				{
					image->setData(Qt::DecorationRole, picture.scaled(ThumbnailSize, ThumbnailSize,
						Qt::KeepAspectRatio, Qt::SmoothTransformation));
				}
			}
			m_ui->itemTable->setItem(row, ColumnImage, image);
			m_ui->itemTable->setRowHeight(row, ThumbnailSize + 4);

			// Plain text, not a picker. A row's part is settled the moment it is dropped in, and
			// a combo box over every part in the database was one stray scroll wheel away from
			// silently turning a resistor into a connector.
			Part named;
			named.name = lines[i].partName;
			named.mpn = lines[i].partMpn;
			QTableWidgetItem* part = new QTableWidgetItem(lines[i].resolved
				? partPickerLabel(named) : tr("— not matched —"));
			part->setFlags(part->flags() & ~Qt::ItemIsEditable);
			part->setToolTip(tr("Double-click to point this line at a different part."));
			m_ui->itemTable->setItem(row, ColumnPart, part);

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

			QPushButton* remove = new QPushButton(QStringLiteral("✕"), m_ui->itemTable);
			remove->setStyleSheet(QString::fromLatin1(RemoveButtonStyle));
			remove->setToolTip(tr("Removes this line from the list. The part itself is untouched."));
			remove->setCursor(Qt::ArrowCursor);
			connect(remove, &QPushButton::clicked, this, [this, row]() { removeRow(row); });
			m_ui->itemTable->setCellWidget(row, ColumnRemove, remove);

			for (int column = ColumnQtyTotal; column < ColumnRemove; ++column)
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
		m_ui->itemTable->setColumnWidth(ColumnImage, ThumbnailSize + 6);
		m_ui->itemTable->setColumnWidth(ColumnRemove, 28);
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
					cell->setToolTip(tr("This line points at no part yet. Drag one out of the "
						"component browser, or double-click the Part column — until then it cannot "
						"be ordered and is left out of “Order Missing Parts”."));
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
					// The Part cell keeps its hint: this pass runs on every quantity change, and
					// clearing it here would make double-click undiscoverable after the first edit.
					cell->setToolTip(column == ColumnPart
						? tr("Double-click to point this line at a different part.") : QString());
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
		// An empty line has nothing to fill it with now that the Part column is not a picker, so
		// this asks which part first and adds the row second.
		PartPickerDialog picker(m_controller, this);
		if (picker.exec() == QDialog::Accepted)
		{
			addPart(picker.selectedPartId());
		}
	}

	void PartlistPanel::showRowMenu(const QPoint& position)
	{
		const QModelIndex index = m_ui->itemTable->indexAt(position);
		if (!index.isValid() || index.row() >= static_cast<int>(m_items.size()))
		{
			return;
		}
		const int row = index.row();
		const int partId = m_items[static_cast<size_t>(row)].partId;

		QMenu menu(this);
		if (partId == NoPartId)
		{
			// Nothing to open yet, so the one thing this row needs is the only thing offered.
			QAction* choose = menu.addAction(tr("Choose Part…"));
			connect(choose, &QAction::triggered, this, [this, row]()
				{
					PartPickerDialog picker(m_controller, this);
					if (picker.exec() == QDialog::Accepted)
					{
						assignPart(row, picker.selectedPartId());
					}
				});
			menu.exec(m_ui->itemTable->viewport()->mapToGlobal(position));
			return;
		}

		PartEditorController editor(m_controller.handle());
		const QString datasheet =
			QString::fromStdString(editor.roleFilePath(partId, PartFileRole::Datasheet));
		const std::string mouserUrl = PartEditorController::mouserPageUrl(
			editor.mouserPartNumber(partId), editor.mouserUrl(partId));

		QAction* open = menu.addAction(tr("Open Component Editor…"));
		connect(open, &QAction::triggered, this, [this, partId]()
			{
				PartEditorDialog dialog(m_controller.handle(), partId, this);
				dialog.exec();
				// The editor autosaves as it goes (§10), so stock, name and attachments can all
				// have moved by the time it closes — and the browser above is just as stale.
				reload();
				emit stockChanged();
			});

		QAction* mouser = menu.addAction(tr("Open on Mouser"));
		mouser->setEnabled(!mouserUrl.empty());
		// Disabled entries say why, the same way the preview panel's buttons do.
		mouser->setToolTip(mouserUrl.empty()
			? tr("This part has no Mouser part number.") : QString::fromStdString(mouserUrl));
		connect(mouser, &QAction::triggered, this, [mouserUrl]()
			{ QDesktopServices::openUrl(QUrl(QString::fromStdString(mouserUrl))); });

		QAction* sheet = menu.addAction(tr("Open Datasheet"));
		sheet->setEnabled(!datasheet.isEmpty());
		sheet->setToolTip(datasheet.isEmpty()
			? tr("No datasheet is attached to this part.") : datasheet);
		connect(sheet, &QAction::triggered, this, [datasheet]()
			{ QDesktopServices::openUrl(QUrl::fromLocalFile(datasheet)); });

		menu.setToolTipsVisible(true);
		menu.exec(m_ui->itemTable->viewport()->mapToGlobal(position));
	}

	void PartlistPanel::updateButtons()
	{
		m_ui->addLineButton->setEnabled(m_partlist.id != NoPartlistId);
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
