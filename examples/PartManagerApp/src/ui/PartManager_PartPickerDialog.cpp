#include "ui/PartManager_PartPickerDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace PartManager
{
	namespace
	{
		enum Column
		{
			ColumnName = 0,
			ColumnMpn,
			ColumnManufacturer,
			ColumnStock,
			ColumnCount
		};

		// Every whitespace-separated term has to appear somewhere in the row, so "res 4k7" narrows
		// instead of widening. No §7a query syntax here on purpose: this box filters four fields of
		// a list already in memory, and a parser that can fail needs an error line to explain itself.
		bool matches(const Part& part, const QStringList& terms)
		{
			const QString haystack = (QString::fromStdString(part.name) + QLatin1Char(' ')
				+ QString::fromStdString(part.mpn) + QLatin1Char(' ')
				+ QString::fromStdString(part.manufacturer) + QLatin1Char(' ')
				+ QString::fromStdString(part.description) + QLatin1Char(' ')
				+ QString::fromStdString(part.package)).toLower();
			for (const QString& term : terms)
			{
				if (!haystack.contains(term))
				{
					return false;
				}
			}
			return true;
		}
	}

	PartPickerDialog::PartPickerDialog(PartlistController& controller, QWidget* parent)
		: QDialog(parent)
		, m_parts(controller.allParts())
	{
		setWindowTitle(tr("Add a part"));
		resize(640, 480);

		m_filterEdit = new QLineEdit(this);
		m_filterEdit->setPlaceholderText(tr("Search name, part number, manufacturer…"));
		m_filterEdit->setClearButtonEnabled(true);

		m_table = new QTableWidget(this);
		m_table->setColumnCount(ColumnCount);
		m_table->setHorizontalHeaderLabels(QStringList()
			<< tr("Part") << tr("MPN") << tr("Manufacturer") << tr("In stock"));
		m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
		m_table->setSelectionMode(QAbstractItemView::SingleSelection);
		m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		m_table->setAlternatingRowColors(true);
		m_table->verticalHeader()->setVisible(false);
		m_table->horizontalHeader()->setSectionResizeMode(ColumnName, QHeaderView::Stretch);

		QDialogButtonBox* buttons =
			new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		m_okButton = buttons->button(QDialogButtonBox::Ok);
		m_okButton->setText(tr("Add to List"));
		m_okButton->setEnabled(false);

		QVBoxLayout* layout = new QVBoxLayout(this);
		layout->addWidget(m_filterEdit);
		layout->addWidget(m_table, 1);
		layout->addWidget(buttons);

		connect(m_filterEdit, &QLineEdit::textChanged, this, &PartPickerDialog::applyFilter);
		connect(m_table, &QTableWidget::itemSelectionChanged, this,
			[this]() { m_okButton->setEnabled(selectedPartId() != NoPartId); });
		// Double-click is how anyone picks out of a list; the button box is for the keyboard.
		connect(m_table, &QTableWidget::itemDoubleClicked, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		// Enter in the filter box takes the top hit rather than being swallowed by the default
		// button while nothing is selected — typing a unique MPN and pressing Enter has to work.
		connect(m_filterEdit, &QLineEdit::returnPressed, this, [this]()
			{
				if (m_table->rowCount() > 0)
				{
					m_table->selectRow(0);
					accept();
				}
			});

		applyFilter(QString());
		m_filterEdit->setFocus();
	}

	int PartPickerDialog::selectedPartId() const
	{
		const QTableWidgetItem* cell = m_table->item(m_table->currentRow(), ColumnName);
		if (cell == nullptr || !m_table->selectionModel()->hasSelection())
		{
			return NoPartId;
		}
		return cell->data(Qt::UserRole).toInt();
	}

	void PartPickerDialog::applyFilter(const QString& filter)
	{
		const QStringList terms = filter.toLower().simplified()
			.split(QLatin1Char(' '), QString::SkipEmptyParts);

		m_table->clearContents();
		m_table->setRowCount(0);
		for (const Part& part : m_parts)
		{
			if (!matches(part, terms))
			{
				continue;
			}
			const int row = m_table->rowCount();
			m_table->insertRow(row);

			// Every cell is the user's own data; the part id rides on column 0 the way it does in
			// the browser's table, so the two agree on where to find it.
			QTableWidgetItem* name = new QTableWidgetItem(QString::fromStdString(part.name));
			name->setData(Qt::UserRole, part.id);
			m_table->setItem(row, ColumnName, name);
			m_table->setItem(row, ColumnMpn,
				new QTableWidgetItem(QString::fromStdString(part.mpn)));
			m_table->setItem(row, ColumnManufacturer,
				new QTableWidgetItem(QString::fromStdString(part.manufacturer)));
			QTableWidgetItem* stock = new QTableWidgetItem(QString::number(part.stockQty));
			stock->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
			m_table->setItem(row, ColumnStock, stock);
		}
		m_okButton->setEnabled(false);
	}

}
