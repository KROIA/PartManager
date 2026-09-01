#include "ui/PartManager_PartlistManagerDialog.h"
#include "ui_PartManager_PartlistManagerDialog.h"

#include "ui/PartManager_PartlistEditorDialog.h"

#include <QHeaderView>
#include <QMessageBox>
#include <QTableWidgetItem>

namespace PartManager
{
	namespace
	{
		// The partlist id a row stands for. Rows are filtered, so a row index is not an index
		// into m_partlists and the id has to travel with the item.
		constexpr int PartlistIdRole = Qt::UserRole;

		enum Column
		{
			ColumnName = 0,
			ColumnProjectLink,
			ColumnMultiplier,
			ColumnItems,
			ColumnSource,
			ColumnUpdated,
			ColumnCount
		};
	}

	PartlistManagerDialog::PartlistManagerDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::PartlistManagerDialog)
		, m_controller(handle)
	{
		m_ui->setupUi(this);

		m_ui->partlistTable->setColumnCount(ColumnCount);
		m_ui->partlistTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Name") << tr("Project link") << tr("Multiplier") << tr("Items")
			<< tr("Source") << tr("Updated"));
		m_ui->partlistTable->horizontalHeader()->setSectionResizeMode(ColumnName, QHeaderView::Stretch);

		connect(m_ui->filterEdit, &QLineEdit::textChanged, this, &PartlistManagerDialog::showPartlists);
		connect(m_ui->partlistTable, &QTableWidget::itemSelectionChanged,
			this, &PartlistManagerDialog::updateButtons);
		connect(m_ui->partlistTable, &QTableWidget::itemDoubleClicked,
			this, &PartlistManagerDialog::openSelected);
		connect(m_ui->newButton, &QPushButton::clicked, this, &PartlistManagerDialog::newPartlist);
		connect(m_ui->openButton, &QPushButton::clicked, this, &PartlistManagerDialog::openSelected);
		connect(m_ui->deleteButton, &QPushButton::clicked, this, &PartlistManagerDialog::deleteSelected);
		connect(m_ui->closeButton, &QPushButton::clicked, this, &PartlistManagerDialog::accept);

		reload();
	}

	PartlistManagerDialog::~PartlistManagerDialog()
	{
		delete m_ui;
	}

	void PartlistManagerDialog::reload()
	{
		m_partlists = m_controller.partlists();
		showPartlists();
	}

	void PartlistManagerDialog::showPartlists()
	{
		const QString filter = m_ui->filterEdit->text().trimmed();

		m_ui->partlistTable->clearContents();
		m_ui->partlistTable->setRowCount(0);
		int row = 0;
		for (const Partlist& partlist : m_partlists)
		{
			const QString name = QString::fromStdString(partlist.name);
			if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive))
			{
				continue;
			}

			m_ui->partlistTable->insertRow(row);
			// Name, project link and source are the user's own data, so no tr(); the multiplier's
			// "x" prefix and the em dash for "nothing here" are chrome.
			const QString cells[ColumnCount] = {
				name,
				partlist.projectLinkUrl.empty()
					? tr("—") : QString::fromStdString(partlist.projectLinkUrl),
				tr("×%1").arg(partlist.multiplier),
				QString::number(m_controller.itemCount(partlist.id)),
				partlistSourceLabel(partlist.source),
				QString::fromStdString(partlist.updatedAt),
			};
			for (int column = 0; column < ColumnCount; ++column)
			{
				QTableWidgetItem* item = new QTableWidgetItem(cells[column]);
				item->setData(PartlistIdRole, partlist.id);
				item->setToolTip(cells[column]);
				m_ui->partlistTable->setItem(row, column, item);
			}
			++row;
		}

		m_ui->partlistTable->resizeColumnsToContents();
		m_ui->partlistTable->horizontalHeader()->setSectionResizeMode(ColumnName, QHeaderView::Stretch);

		if (m_partlists.empty())
		{
			m_ui->statusLabel->setText(tr("No partlists yet — “New Partlist” starts one."));
		}
		else
		{
			m_ui->statusLabel->setText(tr("%n partlist(s), newest first.", "", row));
		}
		updateButtons();
	}

	const Partlist* PartlistManagerDialog::selectedPartlist() const
	{
		QTableWidgetItem* item = m_ui->partlistTable->item(m_ui->partlistTable->currentRow(), ColumnName);
		if (item == nullptr)
		{
			return nullptr;
		}
		const int id = item->data(PartlistIdRole).toInt();
		for (const Partlist& partlist : m_partlists)
		{
			if (partlist.id == id)
			{
				return &partlist;
			}
		}
		return nullptr;
	}

	void PartlistManagerDialog::updateButtons()
	{
		const bool hasSelection = selectedPartlist() != nullptr;
		m_ui->openButton->setEnabled(hasSelection);
		m_ui->deleteButton->setEnabled(hasSelection);
	}

	void PartlistManagerDialog::newPartlist()
	{
		Partlist partlist;
		partlist.name = tr("New partlist").toStdString();
		partlist.source = PartlistSource::Manual;

		const int id = m_controller.create(partlist);
		if (id == NoPartlistId)
		{
			QMessageBox::warning(this, tr("Could not create the partlist"),
				tr("The database rejected the new partlist."));
			return;
		}

		// Straight into the editor: an empty list named "New partlist" is not something anyone
		// wants to look at in the overview first.
		PartlistEditorDialog editor(m_controller, id, this);
		editor.exec();
		reload();
	}

	void PartlistManagerDialog::openSelected()
	{
		const Partlist* partlist = selectedPartlist();
		if (partlist == nullptr)
		{
			return;
		}
		PartlistEditorDialog editor(m_controller, partlist->id, this);
		editor.exec();
		reload();
	}

	void PartlistManagerDialog::deleteSelected()
	{
		const Partlist* partlist = selectedPartlist();
		if (partlist == nullptr)
		{
			return;
		}

		// The list's lines go with it and there is no undo, so the name is spelled out rather
		// than asking about "the selected partlist".
		const QString name = QString::fromStdString(partlist->name);
		if (QMessageBox::question(this, tr("Delete this partlist?"),
			tr("“%1” and all of its lines will be deleted. The parts themselves are not touched.")
				.arg(name),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}

		if (!m_controller.remove(partlist->id))
		{
			QMessageBox::warning(this, tr("Could not delete the partlist"),
				tr("The database rejected the deletion of “%1”.").arg(name));
		}
		reload();
	}

}
