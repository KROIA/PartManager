#include "ui/PartManager_StockDialog.h"
#include "ui_PartManager_StockDialog.h"

#include "domain/PartManager_StockTransaction.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QSpinBox>

namespace PartManager
{
	namespace
	{
		// The one place the app paints a "this is wrong, recount me" number (§3).
		const char* NegativeStyleSheet = "color: #c0392b; font-weight: bold;";
	}

	StockDialog::StockDialog(const QString& partName, int currentQuantity, Mode mode, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::StockDialog)
		, m_mode(mode)
		, m_currentQuantity(currentQuantity)
	{
		m_ui->setupUi(this);

		const bool restocking = m_mode == Mode::Restock;
		setWindowTitle(restocking ? tr("Restock") : tr("Take Out"));
		// The part's name is user data; only the frame around it is translated.
		m_ui->headerLabel->setText(restocking
			? tr("Add to \"%1\" — %2 on the shelf now.").arg(partName).arg(currentQuantity)
			: tr("Take out of \"%1\" — %2 on the shelf now.").arg(partName).arg(currentQuantity));

		// A manual take-out is usually a build, but a broken part is just as real — logging that as a
		// checkout makes the history lie about where the parts went.
		if (restocking)
		{
			m_ui->reasonLabel->hide();
			m_ui->reasonCombo->hide();
		}
		else
		{
			m_ui->reasonCombo->addItem(tr("Used in a build"),
				QString::fromLatin1(StockReason::CheckoutPartlist));
			m_ui->reasonCombo->addItem(tr("Lost or broken"), QString::fromLatin1(StockReason::Loss));
		}

		connect(m_ui->quantitySpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, [this](int) { updatePreview(); });
		connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &StockDialog::accept);
		connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &StockDialog::reject);

		updatePreview();
		m_ui->quantitySpin->selectAll();
	}

	StockDialog::~StockDialog()
	{
		delete m_ui;
	}

	int StockDialog::quantity() const
	{
		return m_ui->quantitySpin->value();
	}

	QString StockDialog::note() const
	{
		return m_ui->noteEdit->text().trimmed();
	}

	std::string StockDialog::reason() const
	{
		if (m_mode == Mode::Restock)
		{
			return StockReason::Restock;
		}
		return m_ui->reasonCombo->currentData().toString().toStdString();
	}

	void StockDialog::updatePreview()
	{
		const int delta = m_mode == Mode::Restock ? quantity() : -quantity();
		const int resulting = m_currentQuantity + delta;

		// §3 allows the over-draw, so this warns rather than disabling the OK button.
		m_ui->resultLabel->setText(resulting < 0
			? tr("Leaves %1 in stock — more than the shelf holds, which is recorded as-is.").arg(resulting)
			: tr("Leaves %1 in stock.").arg(resulting));
		m_ui->resultLabel->setStyleSheet(resulting < 0 ? QString::fromLatin1(NegativeStyleSheet) : QString());
	}

}
