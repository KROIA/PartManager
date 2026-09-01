#include "ui/PartManager_ColumnsDialog.h"
#include "ui_PartManager_ColumnsDialog.h"

#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>

namespace PartManager
{

	ColumnsDialog::ColumnsDialog(const QString& categoryName, const std::vector<PartColumn>& columns,
		QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::ColumnsDialog)
		, m_columns(columns)
	{
		m_ui->setupUi(this);

		// The category name is user data; only the frame around it is translated.
		m_ui->headerLabel->setText(tr("Columns shown for \"%1\".").arg(categoryName));

		for (size_t index = 0; index < m_columns.size(); ++index)
		{
			const PartColumn& column = m_columns[index];
			// Attribute labels are user data, built-in labels are already tr()'d in deriveColumns().
			QListWidgetItem* item = new QListWidgetItem(column.label, m_ui->columnList);
			item->setData(Qt::UserRole, static_cast<int>(index));
			item->setCheckState(column.visible ? Qt::Checked : Qt::Unchecked);
			if (column.key == "name")
			{
				// Pinned first and always on (see header note) — shown, but not editable.
				item->setFlags(Qt::ItemIsEnabled);
				item->setToolTip(tr("The name column always stays first."));
			}
			else
			{
				item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable
					| Qt::ItemIsDragEnabled);
			}
		}

		connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &ColumnsDialog::accept);
		connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &ColumnsDialog::reject);
		connect(m_ui->buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
			this, [this]() { m_resetRequested = true; accept(); });
	}

	ColumnsDialog::~ColumnsDialog()
	{
		delete m_ui;
	}

	std::vector<PartColumn> ColumnsDialog::columns() const
	{
		std::vector<PartColumn> result;
		for (int row = 0; row < m_ui->columnList->count(); ++row)
		{
			const QListWidgetItem* item = m_ui->columnList->item(row);
			const size_t index = static_cast<size_t>(item->data(Qt::UserRole).toInt());
			if (index >= m_columns.size())
			{
				continue;
			}
			PartColumn column = m_columns[index];
			column.visible = item->checkState() == Qt::Checked;
			result.push_back(column);
		}
		return result;
	}

	bool ColumnsDialog::resetRequested() const
	{
		return m_resetRequested;
	}

}
