#include "ui/PartManager_ReceiveArrivalDialog.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace PartManager
{
	namespace
	{
		// The same green the order table paints an arrived row with, so "complete" means one
		// colour across the whole screen rather than two shades of nearly-the-same.
		const char* const CompleteStyle = "color: #1B5E20; font-weight: bold;";
	}

	ReceiveArrivalDialog::ReceiveArrivalDialog(const QString& partName, int ordered,
		int alreadyReceived, double unitPrice, QWidget* parent)
		: QDialog(parent)
		, m_ordered(ordered)
		, m_alreadyReceived(alreadyReceived)
	{
		setWindowTitle(tr("Confirm arrival"));

		QLabel* title = new QLabel(partName, this);   // user data
		title->setWordWrap(true);
		title->setStyleSheet(QStringLiteral("font-weight: bold;"));

		m_arrivedSpin = new QSpinBox(this);
		// An arrival can overshoot — Mouser ships a full reel where 40 were ordered often enough
		// that refusing the number would only teach the user to lie to the field.
		m_arrivedSpin->setRange(0, 1000000);
		m_arrivedSpin->setValue(qMax(0, ordered - alreadyReceived));
		m_arrivedSpin->setToolTip(tr("How many came in this package — not the running total. "
			"PartManager adds it to what was already booked in."));

		m_priceSpin = new QDoubleSpinBox(this);
		m_priceSpin->setRange(0.0, 1000000.0);
		m_priceSpin->setDecimals(4);
		m_priceSpin->setValue(unitPrice);
		m_priceSpin->setToolTip(tr("What was actually paid per piece. 0 leaves it unrecorded — "
			"Mouser's list price and the price on the invoice are not the same number."));

		m_completeButton = new QPushButton(tr("Everything Arrived"), this);
		m_completeButton->setToolTip(tr("Fills in whatever is still outstanding on this line."));

		m_summaryLabel = new QLabel(this);
		m_summaryLabel->setWordWrap(true);
		m_completeLabel = new QLabel(this);
		m_completeLabel->setStyleSheet(QString::fromLatin1(CompleteStyle));

		QFormLayout* form = new QFormLayout();
		{
			QHBoxLayout* arrivedRow = new QHBoxLayout();
			arrivedRow->addWidget(m_arrivedSpin, 1);
			arrivedRow->addWidget(m_completeButton);
			form->addRow(tr("Arrived in this package:"), arrivedRow);
		}
		form->addRow(tr("Unit price actually paid:"), m_priceSpin);

		QDialogButtonBox* buttons =
			new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

		QVBoxLayout* layout = new QVBoxLayout(this);
		layout->addWidget(title);
		layout->addLayout(form);
		layout->addWidget(m_summaryLabel);
		layout->addWidget(m_completeLabel);
		layout->addWidget(buttons);

		connect(m_arrivedSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &ReceiveArrivalDialog::updateSummary);
		connect(m_completeButton, &QPushButton::clicked, this,
			[this]() { m_arrivedSpin->setValue(qMax(0, m_ordered - m_alreadyReceived)); });
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

		updateSummary();
		m_arrivedSpin->selectAll();
		m_arrivedSpin->setFocus();
	}

	int ReceiveArrivalDialog::newlyArrived() const
	{
		return m_arrivedSpin->value();
	}

	int ReceiveArrivalDialog::totalReceived() const
	{
		return qMax(0, m_alreadyReceived + newlyArrived());
	}

	double ReceiveArrivalDialog::unitPrice() const
	{
		return m_priceSpin->value();
	}

	void ReceiveArrivalDialog::updateSummary()
	{
		const int total = totalReceived();
		// The arithmetic spelled out, because the field asks for one number and the database
		// stores another — showing only the result would leave the user guessing which is which.
		m_summaryLabel->setText(tr("%1 already booked in + %2 now = %3 of %4 ordered.")
			.arg(m_alreadyReceived).arg(newlyArrived()).arg(total).arg(m_ordered));

		if (total >= m_ordered && m_ordered > 0)
		{
			m_completeLabel->setText(total > m_ordered
				? tr("✔ Position complete — %n more than ordered.", "", total - m_ordered)
				: tr("✔ Position complete."));
		}
		else
		{
			m_completeLabel->setText(tr("%n still outstanding after this.", "", m_ordered - total));
		}
		m_completeLabel->setStyleSheet(total >= m_ordered && m_ordered > 0
			? QString::fromLatin1(CompleteStyle) : QStringLiteral("color: #57606a;"));
		m_completeButton->setEnabled(m_alreadyReceived + m_arrivedSpin->value() != m_ordered);
	}

}
