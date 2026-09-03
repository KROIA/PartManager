// @file PartManager_ReceiveArrivalDialog.h
// @brief "Confirm arrival" for one order line — what came in *this* package (§4).
//
// The repository stores a running total (`receiveItem(totalReceived)`), but a
// package is what the user actually holds: 6 arrive on Monday, 4 on Thursday,
// and typing "10" the second time is a subtraction nobody should have to do.
// So this asks for the increment and shows the sum it makes, rather than asking
// for the sum.
//
// Built in code rather than in a .ui: the whole dialog is four widgets whose
// only interesting behaviour is the running total, and Designer cannot express
// that half anyway.
// @see docs/design/ARCHITECTURE.md §4
// @see PartManager_OrderManagerDialog.h, PartManager_OrderRepository.h
#pragma once

#include <QDialog>
#include <QString>

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace PartManager
{

	class ReceiveArrivalDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `partName` is user data. `ordered`/`alreadyReceived` come from the line; `unitPrice` is
		// what the line last recorded, offered again as the starting point.
		ReceiveArrivalDialog(const QString& partName, int ordered, int alreadyReceived,
			double unitPrice, QWidget* parent = nullptr);

		// What arrived in this package — the increment, not the total.
		int newlyArrived() const;
		// The running total to store: alreadyReceived + newlyArrived(), clamped at nothing below 0.
		int totalReceived() const;
		double unitPrice() const;

	private:
		// Repaints the running-total line and the completion marker after every edit.
		void updateSummary();

		int m_ordered = 0;
		int m_alreadyReceived = 0;
		QSpinBox* m_arrivedSpin = nullptr;
		QDoubleSpinBox* m_priceSpin = nullptr;
		QLabel* m_summaryLabel = nullptr;
		QLabel* m_completeLabel = nullptr;
		QPushButton* m_completeButton = nullptr;
	};

}
