#include "ui/PartManager_CartStagingDialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace PartManager
{
	namespace
	{
		enum Column
		{
			ColumnPart = 0,
			ColumnMouserNumber,
			ColumnInCart,
			ColumnNeeded,
			ColumnAction,
			ColumnQuantity,
			ColumnCount
		};

		// Index order of the action combo. Set first because it is the safe default: re-staging
		// an order should leave the cart holding what the order needs, not accumulate.
		enum Action
		{
			ActionSet = 0,
			ActionAdd,
			ActionSkip
		};

		// A part already in the cart is the case the user has to look at, so it is tinted.
		const QColor AlreadyInCartColor(0xFB, 0xE1, 0x8F);
	}

	CartStagingDialog::CartStagingDialog(const StagingPlan& plan, const MouserCartResult& cart,
		const std::vector<OrderLine>& lines, QWidget* parent)
		: QDialog(parent)
	{
		setWindowTitle(tr("Review what goes into your Mouser cart"));
		setSizeGripEnabled(true);
		resize(860, 520);

		for (const MouserCartItemRequest& item : plan.items)
		{
			CartStagingRow row;
			row.mouserPartNumber = item.mouserPartNumber;
			row.outstanding = item.quantity;
			row.quantity = item.quantity;
			// The part's own name, so the user recognises the line — a Mouser article number is
			// not something anyone reads at a glance.
			for (const OrderLine& line : lines)
			{
				if (line.mouserPartNumber == item.mouserPartNumber)
				{
					row.partName = line.partName;
					break;
				}
			}
			// What the cart already holds for this article.
			for (const MouserCartLine& existing : cart.lines)
			{
				if (existing.mouserPartNumber == item.mouserPartNumber)
				{
					row.inCart = existing.quantity;
					break;
				}
			}
			m_rows.push_back(row);
		}

		QVBoxLayout* layout = new QVBoxLayout(this);

		QLabel* intro = new QLabel(this);
		intro->setWordWrap(true);
		intro->setText(cart.lines.empty()
			? tr("This order has no Mouser cart yet — a new one will be created.")
			: tr("Your Mouser cart already holds %n line(s). "
				 "“Set to” makes the cart match the number here; “Add” puts that many more in.",
				 "", static_cast<int>(cart.lines.size())));
		layout->addWidget(intro);

		m_table = new QTableWidget(this);
		m_table->setColumnCount(ColumnCount);
		m_table->setHorizontalHeaderLabels(QStringList()
			<< tr("Part") << tr("Mouser P/N") << tr("Already in cart") << tr("Order still needs")
			<< tr("Action") << tr("Quantity"));
		m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
		m_table->setAlternatingRowColors(true);
		m_table->verticalHeader()->setVisible(false);
		layout->addWidget(m_table, 1);

		m_summary = new QLabel(this);
		m_summary->setWordWrap(true);
		layout->addWidget(m_summary);

		QHBoxLayout* buttons = new QHBoxLayout();
		QPushButton* allSet = new QPushButton(tr("All: Set to"), this);
		QPushButton* allAdd = new QPushButton(tr("All: Add"), this);
		QPushButton* allSkip = new QPushButton(tr("All: Skip"), this);
		QPushButton* newCart = new QPushButton(tr("Start a New Cart…"), this);
		QPushButton* ok = new QPushButton(tr("Stage"), this);
		QPushButton* cancel = new QPushButton(tr("Cancel"), this);
		ok->setDefault(true);
		buttons->addWidget(allSet);
		buttons->addWidget(allAdd);
		buttons->addWidget(allSkip);
		buttons->addWidget(newCart);
		buttons->addStretch(1);
		buttons->addWidget(ok);
		buttons->addWidget(cancel);
		layout->addLayout(buttons);

		connect(allSet, &QPushButton::clicked, this, [this]() { setAllTo(ActionSet); });
		connect(allAdd, &QPushButton::clicked, this, [this]() { setAllTo(ActionAdd); });
		connect(allSkip, &QPushButton::clicked, this, [this]() { setAllTo(ActionSkip); });
		connect(newCart, &QPushButton::clicked, this, &CartStagingDialog::requestNewCart);
		connect(ok, &QPushButton::clicked, this, &CartStagingDialog::accept);
		connect(cancel, &QPushButton::clicked, this, &CartStagingDialog::reject);
		connect(m_table, &QTableWidget::cellChanged, this, &CartStagingDialog::onCellChanged);

		fill();
	}

	void CartStagingDialog::fill()
	{
		// The cellChanged handler would otherwise fire on every setItem() below and read a
		// half-built row.
		m_populating = true;
		m_table->clearContents();
		m_table->setRowCount(static_cast<int>(m_rows.size()));

		for (int row = 0; row < static_cast<int>(m_rows.size()); ++row)
		{
			const CartStagingRow& data = m_rows[static_cast<size_t>(row)];

			auto readOnly = [&](int column, const QString& text)
			{
				QTableWidgetItem* item = new QTableWidgetItem(text);
				item->setFlags(item->flags() & ~Qt::ItemIsEditable);
				if (data.inCart > 0)
				{
					item->setBackground(AlreadyInCartColor);
				}
				m_table->setItem(row, column, item);
			};

			readOnly(ColumnPart, data.partName.empty()
				? tr("(unnamed part)") : QString::fromStdString(data.partName));
			readOnly(ColumnMouserNumber, QString::fromStdString(data.mouserPartNumber));
			readOnly(ColumnInCart, data.inCart > 0 ? QString::number(data.inCart) : tr("—"));
			readOnly(ColumnNeeded, QString::number(data.outstanding));

			QComboBox* action = new QComboBox(m_table);
			action->addItem(tr("Set to"), ActionSet);
			action->addItem(tr("Add"), ActionAdd);
			action->addItem(tr("Skip"), ActionSkip);
			action->setCurrentIndex(data.skip ? ActionSkip : (data.add ? ActionAdd : ActionSet));
			connect(action, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[this, row](int index)
				{
					CartStagingRow& target = m_rows[static_cast<size_t>(row)];
					target.skip = (index == ActionSkip);
					target.add = (index == ActionAdd);
					updateSummary();
				});
			m_table->setCellWidget(row, ColumnAction, action);

			QTableWidgetItem* quantity = new QTableWidgetItem(QString::number(data.quantity));
			m_table->setItem(row, ColumnQuantity, quantity);
		}

		m_table->resizeColumnsToContents();
		m_table->horizontalHeader()->setSectionResizeMode(ColumnPart, QHeaderView::Stretch);
		m_populating = false;
		updateSummary();
	}

	void CartStagingDialog::onCellChanged(int row, int column)
	{
		if (m_populating || column != ColumnQuantity
			|| row < 0 || row >= static_cast<int>(m_rows.size()))
		{
			return;
		}
		QTableWidgetItem* item = m_table->item(row, column);
		if (item == nullptr)
		{
			return;
		}
		bool parsed = false;
		const int value = item->text().toInt(&parsed);
		if (!parsed || value < 0)
		{
			// Put the old value back rather than sending nonsense to a live cart.
			m_populating = true;
			item->setText(QString::number(m_rows[static_cast<size_t>(row)].quantity));
			m_populating = false;
			return;
		}
		m_rows[static_cast<size_t>(row)].quantity = value;
		updateSummary();
	}

	void CartStagingDialog::setAllTo(int actionIndex)
	{
		for (CartStagingRow& row : m_rows)
		{
			row.skip = (actionIndex == ActionSkip);
			row.add = (actionIndex == ActionAdd);
		}
		fill();
	}

	void CartStagingDialog::requestNewCart()
	{
		// Mouser has no delete-cart call and no way to list an account's carts, so the old cart
		// keeps existing and nothing here will ever find it again. That has to be said plainly.
		if (QMessageBox::question(this, tr("Start a new cart?"),
			tr("The cart this order is currently pointing at will be left as it is, and a brand-new "
			   "cart will be created when you stage.\n\n"
			   "Mouser's API cannot delete a cart or list the carts on your account, so PartManager "
			   "will not be able to find the old one again — clear it on mouser.com if you do not "
			   "want it. Continue?"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}
		m_newCart = true;
		// Nothing is in the new cart, so "already in cart" no longer applies to any row and
		// setting is the only sensible action.
		for (CartStagingRow& row : m_rows)
		{
			row.inCart = 0;
			row.add = false;
		}
		fill();
	}

	void CartStagingDialog::updateSummary()
	{
		int set = 0;
		int add = 0;
		int skip = 0;
		for (const CartStagingRow& row : m_rows)
		{
			if (row.skip || row.quantity <= 0) { ++skip; }
			else if (row.add)                  { ++add; }
			else                               { ++set; }
		}

		QStringList parts;
		if (set > 0)  { parts.append(tr("%n line(s) set to an exact quantity", "", set)); }
		if (add > 0)  { parts.append(tr("%n line(s) added on top of the cart", "", add)); }
		if (skip > 0) { parts.append(tr("%n line(s) skipped", "", skip)); }
		if (m_newCart)
		{
			parts.append(tr("a new cart will be created"));
		}
		// A mixed selection genuinely needs two requests; saying so beats the user wondering why
		// the log shows two calls.
		if (set > 0 && add > 0)
		{
			parts.append(tr("sent as two requests, since Mouser sets and adds on different endpoints"));
		}
		m_summary->setText(parts.isEmpty() ? tr("Nothing selected — nothing will be sent.")
			: parts.join(tr(", ")));
	}

	std::vector<MouserCartItemRequest> CartStagingDialog::itemsToSet() const
	{
		std::vector<MouserCartItemRequest> items;
		for (const CartStagingRow& row : m_rows)
		{
			if (row.skip || row.add || row.quantity <= 0)
			{
				continue;
			}
			MouserCartItemRequest item;
			item.mouserPartNumber = row.mouserPartNumber;
			item.quantity = row.quantity;
			items.push_back(item);
		}
		return items;
	}

	std::vector<MouserCartItemRequest> CartStagingDialog::itemsToAdd() const
	{
		std::vector<MouserCartItemRequest> items;
		for (const CartStagingRow& row : m_rows)
		{
			if (row.skip || !row.add || row.quantity <= 0)
			{
				continue;
			}
			MouserCartItemRequest item;
			item.mouserPartNumber = row.mouserPartNumber;
			item.quantity = row.quantity;
			items.push_back(item);
		}
		return items;
	}

	bool CartStagingDialog::wantsNewCart() const
	{
		return m_newCart;
	}

}
