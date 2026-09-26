#include "ui/PartManager_PartTypePickerDialog.h"
#include "ui_PartManager_PartTypePickerDialog.h"

#include "controllers/PartManager_MainWindowController.h"   // buildCategoryTree, typeIdWithDescendants
#include "controllers/PartManager_PartEditorController.h"   // types()

#include "domain/PartManager_TypeIcon.h"
#include "widgets/PartManager_TypeIconPainter.h"

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QStringList>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <algorithm>
#include <map>

namespace PartManager
{
	namespace
	{
		constexpr int TypeIdRole = Qt::UserRole;

		// What separates the steps of a path. A single glyph rather than " > ": the path sits inside
		// a button label beside real category names, and an angle bracket reads as markup there.
		const QString& pathSeparator()
		{
			static const QString separator = QStringLiteral(" \xE2\x80\xBA ");   // U+203A
			return separator;
		}
	}

	QString partTypePath(const std::vector<PartType>& types, int typeId)
	{
		if (typeId == NoParentType)
		{
			return QString();
		}

		std::map<int, const PartType*> byId;
		for (const PartType& type : types)
		{
			byId[type.id] = &type;
		}

		// Root last while walking up, reversed at the end. `seen` is the cycle guard: a
		// parent_type_id loop is a broken database, not a reason to hang the dialog that shows it.
		QStringList reversed;
		std::vector<int> seen;
		int current = typeId;
		while (current != NoParentType
			&& std::find(seen.begin(), seen.end(), current) == seen.end())
		{
			seen.push_back(current);
			const auto found = byId.find(current);
			if (found == byId.end())
			{
				break;
			}
			reversed.append(QString::fromStdString(found->second->name));   // user data
			current = found->second->parentTypeId;
		}

		std::reverse(reversed.begin(), reversed.end());
		return reversed.join(pathSeparator());
	}

	PartTypePickerDialog::PartTypePickerDialog(DatabaseHandle* handle, int preselectedTypeId,
		QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::PartTypePickerDialog)
		, m_types(PartEditorController(handle).types())
	{
		build(preselectedTypeId, NoParentType);
	}

	PartTypePickerDialog::PartTypePickerDialog(DatabaseHandle* handle, int preselectedTypeId,
		int excludeSubtreeOfTypeId, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::PartTypePickerDialog)
		, m_types(PartEditorController(handle).types())
	{
		build(preselectedTypeId, excludeSubtreeOfTypeId);
	}

	PartTypePickerDialog::~PartTypePickerDialog()
	{
		delete m_ui;
	}

	void PartTypePickerDialog::build(int preselectedTypeId, int excludeSubtreeOfTypeId)
	{
		m_ui->setupUi(this);

		// Saying it once here rather than per item: the glyphs below are drawn at exactly this size,
		// and a view whose icon size disagrees scales them and they go soft — the same reason the
		// main window sets it on its own tree.
		m_ui->typeTree->setIconSize(QSize(CategoryGlyphSize, CategoryGlyphSize));

		m_okButton = m_ui->buttonBox->button(QDialogButtonBox::Ok);
		// Its own button rather than a row in the tree — see the header for why. ResetRole keeps it
		// on the left of OK/Cancel on every platform, which is where a "neither of these" answer
		// belongs.
		m_noTypeButton = m_ui->buttonBox->addButton(tr("No category"), QDialogButtonBox::ResetRole);

		const std::vector<int> excluded = typeIdWithDescendants(m_types, excludeSubtreeOfTypeId);
		QTreeWidgetItem* preselected = nullptr;
		for (const CategoryNode& root : buildCategoryTree(m_types, std::map<int, int>(),
			std::map<int, int>(), std::map<int, int>()))
		{
			addTypeItem(root, nullptr, excluded, preselectedTypeId, preselected);
		}

		if (preselected != nullptr)
		{
			// Every ancestor opened, then scrolled to: a selection three levels down that nobody can
			// see is the same as no selection at all.
			for (QTreeWidgetItem* ancestor = preselected->parent(); ancestor != nullptr;
				ancestor = ancestor->parent())
			{
				ancestor->setExpanded(true);
			}
			m_ui->typeTree->setCurrentItem(preselected);
			m_ui->typeTree->scrollToItem(preselected);
		}
		else
		{
			// Nothing chosen yet: the roots opened one level, not expandAll(). A catalogue with a
			// few hundred types expanded whole is a wall of text to scroll, and the first decision
			// the user makes is which family it belongs to anyway.
			m_ui->typeTree->expandToDepth(0);
		}

		connect(m_ui->filterEdit, &QLineEdit::textChanged, this,
			&PartTypePickerDialog::applyFilter);
		connect(m_ui->typeTree, &QTreeWidget::itemSelectionChanged, this,
			&PartTypePickerDialog::updateOkState);
		// Double-click is how anyone picks out of a tree; the button box is for the keyboard. Guarded
		// on the item, because double-clicking the blank area below the last row reports no item.
		connect(m_ui->typeTree, &QTreeWidget::itemDoubleClicked, this,
			[this](QTreeWidgetItem* item, int)
			{
				if (item != nullptr)
				{
					m_selectedTypeId = item->data(0, TypeIdRole).toInt();
					accept();
				}
			});
		connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, [this]()
			{
				QTreeWidgetItem* item = m_ui->typeTree->currentItem();
				m_selectedTypeId = item != nullptr ? item->data(0, TypeIdRole).toInt() : NoParentType;
				accept();
			});
		connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(m_noTypeButton, &QPushButton::clicked, this, [this]()
			{
				m_selectedTypeId = NoParentType;
				accept();
			});

		updateOkState();
		m_ui->filterEdit->setFocus();
	}

	int PartTypePickerDialog::selectedTypeId() const
	{
		return m_selectedTypeId;
	}

	void PartTypePickerDialog::setNoTypeButtonText(const QString& text)
	{
		m_noTypeButton->setText(text);
	}

	const std::vector<PartType>& PartTypePickerDialog::types() const
	{
		return m_types;
	}

	void PartTypePickerDialog::addTypeItem(const CategoryNode& node, QTreeWidgetItem* parent,
		const std::vector<int>& excluded, int preselectedTypeId, QTreeWidgetItem*& outPreselected)
	{
		// Dropped with everything under it: `excluded` already carries the whole subtree, but
		// returning here also stops the recursion, so a descendant cannot be built under a parent
		// that does not exist.
		if (std::find(excluded.begin(), excluded.end(), node.typeId) != excluded.end())
		{
			return;
		}

		QTreeWidgetItem* item = parent != nullptr
			? new QTreeWidgetItem(parent)
			: new QTreeWidgetItem(m_ui->typeTree);

		// The bare name, without the main window's "(n)" — see the header for why the counts are
		// left out. The name is the user's own data and is never tr()'d.
		item->setText(0, node.name);
		item->setData(0, TypeIdRole, node.typeId);
		// The full path in the tooltip, because two branches are allowed to hold a type of the same
		// name and the row alone cannot tell them apart.
		item->setToolTip(0, partTypePath(m_types, node.typeId));

		// The same rule the main window's tree paints by: a type that classifies as Generic gets no
		// glyph, since a column of look-alike initials boxes says nothing the name does not.
		if (TypeIconStyle::forType(node.name.toStdString()).glyph != TypeGlyph::Generic)
		{
			item->setIcon(0, TypeIconPainter::icon(node.name, CategoryGlyphSize, devicePixelRatioF()));
		}

		if (node.typeId == preselectedTypeId && preselectedTypeId != NoParentType)
		{
			outPreselected = item;
		}

		for (const CategoryNode& child : node.children)
		{
			addTypeItem(child, item, excluded, preselectedTypeId, outPreselected);
		}
	}

	void PartTypePickerDialog::applyFilter(const QString& filter)
	{
		const QString trimmed = filter.trimmed();
		bool anyVisible = false;
		for (int i = 0; i < m_ui->typeTree->topLevelItemCount(); ++i)
		{
			anyVisible = filterItem(m_ui->typeTree->topLevelItem(i), trimmed) || anyVisible;
		}

		if (!trimmed.isEmpty())
		{
			// Everything that survived is opened, or a branch kept alive purely by a deep descendant
			// would still be a closed row the user has to guess at.
			m_ui->typeTree->expandAll();
		}
		m_ui->emptyLabel->setText(anyVisible || trimmed.isEmpty()
			? QString()
			: tr("No category matches “%1”.").arg(trimmed));
		updateOkState();
	}

	bool PartTypePickerDialog::filterItem(QTreeWidgetItem* item, const QString& filter)
	{
		bool childMatches = false;
		for (int i = 0; i < item->childCount(); ++i)
		{
			// Not short-circuited: every child has to be walked, or the ones after the first hit
			// keep whatever visibility the previous filter left them with.
			childMatches = filterItem(item->child(i), filter) || childMatches;
		}

		const bool selfMatches = filter.isEmpty()
			|| item->text(0).contains(filter, Qt::CaseInsensitive);
		const bool visible = selfMatches || childMatches;
		item->setHidden(!visible);
		return visible;
	}

	void PartTypePickerDialog::updateOkState()
	{
		const QTreeWidgetItem* item = m_ui->typeTree->currentItem();
		// A hidden row is still the "current" one after a filter narrowed it away, and confirming a
		// category the user can no longer see is exactly the kind of wrong answer this dialog exists
		// to prevent.
		m_okButton->setEnabled(item != nullptr && !item->isHidden()
			&& item->data(0, TypeIdRole).toInt() != NoParentType);
	}

}
