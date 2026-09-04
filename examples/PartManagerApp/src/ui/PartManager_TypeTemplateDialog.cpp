#include "ui/PartManager_TypeTemplateDialog.h"
#include "ui_PartManager_TypeTemplateDialog.h"
#include "controllers/PartManager_MainWindowController.h"
#include "domain/PartManager_PartFileRole.h"
#include "search/PartManager_SearchEngine.h"
#include "widgets/PartManager_KeywordCheckList.h"
#include "units/PartManager_UnitTable.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSyntaxHighlighter>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidgetItem>

namespace PartManager
{
	namespace
	{
		// Item data roles. Column 0 of a table row carries the row's database id; the tree
		// carries the part_type id.
		constexpr int RowIdRole = Qt::UserRole;
		constexpr int TypeIdRole = Qt::UserRole;

		// Paints the `{key}` placeholders in the naming pattern while it is being typed: green once
		// the letters so far spell a key this type has, orange until they do. The closing brace is
		// not waited for — a run is highlighted from `{` to `}` *or to the end of the line*, which
		// is what makes the colour flip on the last letter typed rather than on the brace after it.
		// No Q_OBJECT: it declares no signals or slots, so moc has nothing to do here.
		class TemplateKeyHighlighter : public QSyntaxHighlighter
		{
		public:
			TemplateKeyHighlighter(QTextDocument* document, const QStringList* knownKeys)
				: QSyntaxHighlighter(document)
				, m_knownKeys(knownKeys)
			{
			}

		protected:
			void highlightBlock(const QString& text) override
			{
				int at = 0;
				while ((at = text.indexOf(QLatin1Char('{'), at)) >= 0)
				{
					const int close = text.indexOf(QLatin1Char('}'), at + 1);
					const int keyEnd = close < 0 ? text.size() : close;
					const int runEnd = close < 0 ? text.size() : close + 1;
					const QString key = text.mid(at + 1, keyEnd - at - 1);
					QTextCharFormat format;
					format.setBackground(m_knownKeys->contains(key)
						? QColor(0xB7, 0xE4, 0xB0)    // this type has that attribute
						: QColor(0xFF, 0xD1, 0x8C));  // not (yet) a key anything would fill in
					format.setForeground(QColor(0x20, 0x20, 0x20));   // both backgrounds are pale
					setFormat(at, runEnd - at, format);
					at = runEnd;
				}
			}

		private:
			const QStringList* m_knownKeys;
		};

		// Attribute table columns, in the mockup's order.
		enum AttributeColumn
		{
			AttrRequired = 0,
			AttrKey,
			AttrLabel,
			AttrUnit,
			AttrDatatype,
			AttrSearchable,
			AttrOrder
		};

		// File slot table columns.
		enum FileSlotColumn
		{
			SlotRequired = 0,
			SlotRole,
			SlotLabel,
			SlotOrder
		};

		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// A "+" or a "−" at button size, in the palette's text colour so it survives a dark theme.
		// Device-pixel-ratio aware, or it comes out fuzzy on a scaled display like everything else
		// drawn at a fixed pixel size.
		QIcon plusMinusIcon(bool plus)
		{
			const int side = 16;
			const qreal ratio = qApp->devicePixelRatio();
			QPixmap pixmap(QSize(side, side) * ratio);
			pixmap.setDevicePixelRatio(ratio);
			pixmap.fill(Qt::transparent);

			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing);
			QPen pen(qApp->palette().color(QPalette::ButtonText));
			pen.setWidthF(2.0);
			pen.setCapStyle(Qt::RoundCap);
			painter.setPen(pen);
			const qreal margin = 3.0;
			const qreal middle = side / 2.0;
			painter.drawLine(QPointF(margin, middle), QPointF(side - margin, middle));
			if (plus)
			{
				painter.drawLine(QPointF(middle, margin), QPointF(middle, side - margin));
			}
			return QIcon(pixmap);
		}

		// The §2a dropdown, "(no unit)" first — the empty string, spelled out so the cell does
		// not read as an unfilled field.
		void fillUnitCombo(QComboBox* combo)
		{
			for (const std::string& unit : UnitTable::units())
			{
				combo->addItem(unit.empty() ? QObject::tr("(no unit)") : toQt(unit), toQt(unit));
			}
		}

		void fillDatatypeCombo(QComboBox* combo)
		{
			combo->addItem(QObject::tr("Number"), static_cast<int>(AttributeDataType::Number));
			combo->addItem(QObject::tr("Dimension"), static_cast<int>(AttributeDataType::Dimension));
			combo->addItem(QObject::tr("Text"), static_cast<int>(AttributeDataType::Text));
			combo->addItem(QObject::tr("Bool"), static_cast<int>(AttributeDataType::Bool));
			combo->addItem(QObject::tr("Enum"), static_cast<int>(AttributeDataType::Enum));
		}

		// The fixed §3 vocabulary a file slot's role is picked from. Free text here would let a
		// slot name a role no part file can ever carry, and the slot would stay unfillable.
		QStringList knownFileRoles()
		{
			return QStringList{ toQt(toString(PartFileRole::Datasheet)),
				toQt(toString(PartFileRole::KicadSymbol)),
				toQt(toString(PartFileRole::KicadFootprint)),
				toQt(toString(PartFileRole::Kicad3DModel)),
				toQt(toString(PartFileRole::Image)),
				toQt(toString(PartFileRole::Other)) };
		}

		// An inherited row is shown but not touched here: greyed, unchecked-but-not-checkable,
		// and carrying the ancestor it came from so it is obvious where to go to change it.
		void markInherited(QTableWidgetItem* item, const QString& ancestorName)
		{
			item->setFlags(item->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsUserCheckable);
			item->setForeground(Qt::gray);
			item->setToolTip(QObject::tr("Inherited from \"%1\". Change it there, or add a row with "
				"the same key here to override it.").arg(ancestorName));
		}

		// The type's name for a tooltip, or an empty string when the row's owner is gone.
		QString typeNameOf(const std::vector<PartType>& types, int typeId)
		{
			for (const PartType& type : types)
			{
				if (type.id == typeId)
				{
					return toQt(type.name);   // user data — never tr()'d
				}
			}
			return QString();
		}

		// Selects the row whose id column carries `rowId`, if it is still there. A rebuilt table
		// has new items, so a selection can only be restored by id — the row index is exactly what
		// a reorder changes.
		void selectRowById(QTableWidget* table, int idColumn, int rowId)
		{
			if (rowId == 0)
			{
				return;
			}
			for (int row = 0; row < table->rowCount(); ++row)
			{
				const QTableWidgetItem* item = table->item(row, idColumn);
				if (item != nullptr && item->data(RowIdRole).toInt() == rowId)
				{
					table->selectRow(row);
					return;
				}
			}
		}

		// Fills the tree from the §7a forest builder, which already resolves the parent chain,
		// sorts siblings by name and turns an orphan or a cycle member into a root — the same
		// forest the main window's category tree paints, so the two can never disagree.
		void addNodes(QTreeWidget* tree, QTreeWidgetItem* parent, const std::vector<CategoryNode>& nodes,
			int selectedTypeId, QTreeWidgetItem*& outToSelect)
		{
			for (const CategoryNode& node : nodes)
			{
				QTreeWidgetItem* item = parent
					? new QTreeWidgetItem(parent, QStringList{ node.name })
					: new QTreeWidgetItem(tree, QStringList{ node.name });
				item->setData(0, TypeIdRole, node.typeId);
				item->setExpanded(true);
				if (node.typeId == selectedTypeId)
				{
					outToSelect = item;
				}
				addNodes(tree, item, node.children, selectedTypeId, outToSelect);
			}
		}
	}

	TypeTemplateDialog::TypeTemplateDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::TypeTemplateDialog)
		, m_controller(handle)
	{
		m_ui->setupUi(this);

		// The tables are what the dialog is for; the type tree only has to be readable.
		m_ui->bodyLayout->setStretch(0, 1);
		m_ui->bodyLayout->setStretch(1, 3);

		// Marking an attribute searchable is the one edit here with a schema consequence, and it
		// is one-way. Saying so on the header is enough — nothing in the UI has to act on it.
		m_ui->attributeTable->horizontalHeaderItem(AttrSearchable)->setToolTip(
			tr("A searchable number gets its own column on the part table, so filtering on it stays "
				"fast. Unticking this later leaves that column in place — it is simply unused."));
		m_ui->attributeTable->horizontalHeaderItem(AttrKey)->setToolTip(
			tr("The key a part's value is stored under. It is fixed once the attribute exists: "
				"renaming it would leave every value already entered under a name nothing reads."));
		// Per-section modes, set once, rather than resizeColumnsToContents() after every rebuild:
		// that call resizes *every* section to its contents — the per-section Stretch included —
		// so the columns ended up summing to less than the viewport and the last one stopped short
		// of the right edge. Label takes the slack; everything else is a checkbox, a short word or
		// a number and is content-sized.
		for (QTableWidget* table : { m_ui->attributeTable, m_ui->fileSlotTable })
		{
			QHeaderView* header = table->horizontalHeader();
			header->setStretchLastSection(false);
			for (int column = 0; column < table->columnCount(); ++column)
			{
				header->setSectionResizeMode(column, QHeaderView::ResizeToContents);
			}
		}
		m_ui->attributeTable->horizontalHeader()->setSectionResizeMode(AttrLabel, QHeaderView::Stretch);
		m_ui->fileSlotTable->horizontalHeader()->setSectionResizeMode(SlotLabel, QHeaderView::Stretch);

		// +/− and arrows, so the four buttons under each table read at a glance. Drawn rather than
		// loaded: there is a plus.png in the resources but no minus, and a drawn pair matches.
		m_ui->addAttributeButton->setIcon(plusMinusIcon(true));
		m_ui->removeAttributeButton->setIcon(plusMinusIcon(false));
		m_ui->addFileSlotButton->setIcon(plusMinusIcon(true));
		m_ui->removeFileSlotButton->setIcon(plusMinusIcon(false));
		m_ui->attributeUpButton->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
		m_ui->attributeDownButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
		m_ui->fileSlotUpButton->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
		m_ui->fileSlotDownButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));

		// §7a: both halves of the same thing in one frame — the words this branch inherits on top,
		// because they are the ones that already apply, and the words it adds of its own below.
		m_inheritedKeywords = new KeywordCheckList(this);
		m_inheritedKeywords->setEmptyText(tr("No parent type hands any search words down to this one."));
		QGroupBox* searchWordsGroup = new QGroupBox(tr("Search words"), this);
		QFormLayout* searchWordsForm = new QFormLayout(searchWordsGroup);
		// takeRow() rather than a reparent, so the row the field came from closes up instead of
		// leaving an empty one in the middle of the form.
		QFormLayout::TakeRowResult keywordRow = m_ui->formLayout->takeRow(m_ui->searchKeywordsEdit);
		delete keywordRow.labelItem;
		delete keywordRow.fieldItem;
		m_ui->searchKeywordsLabel->setText(tr("This type's own"));
		searchWordsForm->addRow(tr("Inherited"), m_inheritedKeywords);
		searchWordsForm->addRow(m_ui->searchKeywordsLabel, m_ui->searchKeywordsEdit);
		m_ui->formLayout->insertRow(6, searchWordsGroup);

		// §11: the pattern the part editor's suggested name is built from. Typed as text — the
		// syntax is one form, `{key}` — with a menu that inserts a placeholder for any attribute
		// the type has, so the keys never have to be remembered or spelled right.
		m_nameTemplateEdit = new QPlainTextEdit(this);
		m_nameTemplateEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
		m_nameTemplateEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		m_nameTemplateEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		m_nameTemplateEdit->setTabChangesFocus(true);
		// One line high, so it reads as the single-line field it is despite being a text edit.
		m_nameTemplateEdit->setFixedHeight(m_nameTemplateEdit->fontMetrics().height() + 12);
		m_nameTemplateEdit->setPlaceholderText(tr("e.g. Resistor {resistance} {package}"));
		m_nameHighlighter = new TemplateKeyHighlighter(m_nameTemplateEdit->document(),
			&m_knownTemplateKeys);
		m_nameTemplateEdit->setToolTip(tr("Every {key} is replaced by that attribute's value on the "
			"part. {manufacturer}, {mpn} and {package} are understood too. A placeholder the part "
			"has no value for simply disappears. Inherited by subtypes that declare no pattern of "
			"their own."));
		QToolButton* insertKeyButton = new QToolButton(this);
		insertKeyButton->setText(tr("Insert…"));
		insertKeyButton->setPopupMode(QToolButton::InstantPopup);
		insertKeyButton->setToolTip(tr("Puts a placeholder for one of this type's attributes into "
			"the pattern at the cursor."));
		m_insertKeyMenu = new QMenu(insertKeyButton);
		insertKeyButton->setMenu(m_insertKeyMenu);
		connect(m_insertKeyMenu, &QMenu::aboutToShow, this, &TypeTemplateDialog::refreshInsertKeyMenu);

		// What the pattern would actually produce, on a part with a value in every field — the
		// answer to "is that the name I meant" without leaving the dialog to go and look.
		m_namePreviewLabel = new QLabel(this);
		m_namePreviewLabel->setEnabled(false);
		m_namePreviewLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

		QWidget* nameTemplateRow = new QWidget(this);
		QVBoxLayout* nameTemplateColumn = new QVBoxLayout(nameTemplateRow);
		nameTemplateColumn->setContentsMargins(0, 0, 0, 0);
		nameTemplateColumn->setSpacing(2);
		QHBoxLayout* nameTemplateLayout = new QHBoxLayout();
		nameTemplateLayout->setContentsMargins(0, 0, 0, 0);
		nameTemplateLayout->addWidget(m_nameTemplateEdit);
		nameTemplateLayout->addWidget(insertKeyButton);
		nameTemplateColumn->addLayout(nameTemplateLayout);
		nameTemplateColumn->addWidget(m_namePreviewLabel);
		m_ui->formLayout->insertRow(7, tr("Name pattern"), nameTemplateRow);

		connect(m_ui->newTypeButton, &QPushButton::clicked, this, &TypeTemplateDialog::onNewType);
		connect(m_ui->deleteTypeButton, &QPushButton::clicked, this, &TypeTemplateDialog::onDeleteType);
		connect(m_ui->typeTree, &QTreeWidget::itemSelectionChanged, this,
			&TypeTemplateDialog::onTypeSelectionChanged);

		connect(m_ui->nameEdit, &QLineEdit::textEdited, this, &TypeTemplateDialog::onTypeFieldEdited);
		connect(m_ui->kicadCategoryEdit, &QLineEdit::textEdited, this, &TypeTemplateDialog::onTypeFieldEdited);
		connect(m_ui->kicadRelevantCheck, &QCheckBox::toggled, this, &TypeTemplateDialog::onTypeFieldEdited);
		connect(m_ui->descriptionEdit, &QPlainTextEdit::textChanged, this, &TypeTemplateDialog::onTypeFieldEdited);
		connect(m_ui->searchKeywordsEdit, &QPlainTextEdit::textChanged, this, &TypeTemplateDialog::onTypeFieldEdited);
		connect(m_inheritedKeywords, &KeywordCheckList::excludedChanged, this, &TypeTemplateDialog::onTypeFieldEdited);
		connect(m_nameTemplateEdit, &QPlainTextEdit::textChanged, this, [this]()
			{
				// A name pattern is one line. Enter is the only way to get a second one into a
				// text edit, and folding it away here costs less than an event filter.
				const QString typed = m_nameTemplateEdit->toPlainText();
				if (typed.contains(QLatin1Char('\n')))
				{
					QString flattened = typed;
					flattened.replace(QLatin1Char('\n'), QString());
					m_nameTemplateEdit->setPlainText(flattened);
					m_nameTemplateEdit->moveCursor(QTextCursor::End);
					return;   // setPlainText() brings us straight back here
				}
				updateNamePreview();
				onTypeFieldEdited();
			});
		connect(m_ui->domainCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			&TypeTemplateDialog::onTypeFieldEdited);
		connect(m_ui->parentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			&TypeTemplateDialog::onTypeFieldEdited);

		connect(m_ui->addAttributeButton, &QPushButton::clicked, this, &TypeTemplateDialog::onAddAttribute);
		connect(m_ui->removeAttributeButton, &QPushButton::clicked, this, &TypeTemplateDialog::onRemoveAttribute);
		connect(m_ui->attributeUpButton, &QPushButton::clicked, this, [this] { moveSelectedAttribute(-1); });
		connect(m_ui->attributeDownButton, &QPushButton::clicked, this, [this] { moveSelectedAttribute(1); });
		connect(m_ui->enumOptionsButton, &QPushButton::clicked, this, &TypeTemplateDialog::onEditEnumOptions);
		connect(m_ui->attributeTable, &QTableWidget::itemSelectionChanged, this,
			&TypeTemplateDialog::onAttributeSelectionChanged);
		connect(m_ui->attributeTable, &QTableWidget::itemChanged, this,
			&TypeTemplateDialog::onAttributeItemChanged);
		connect(m_ui->tooltipEdit, &QPlainTextEdit::textChanged, this, &TypeTemplateDialog::onTooltipEdited);

		connect(m_ui->addFileSlotButton, &QPushButton::clicked, this, &TypeTemplateDialog::onAddFileSlot);
		connect(m_ui->removeFileSlotButton, &QPushButton::clicked, this, &TypeTemplateDialog::onRemoveFileSlot);
		connect(m_ui->fileSlotUpButton, &QPushButton::clicked, this, [this] { moveSelectedFileSlot(-1); });
		connect(m_ui->fileSlotDownButton, &QPushButton::clicked, this, [this] { moveSelectedFileSlot(1); });
		connect(m_ui->fileSlotTable, &QTableWidget::itemSelectionChanged, this,
			&TypeTemplateDialog::onFileSlotSelectionChanged);
		connect(m_ui->fileSlotTable, &QTableWidget::itemChanged, this,
			&TypeTemplateDialog::onFileSlotItemChanged);

		connect(m_ui->closeButton, &QPushButton::clicked, this, &TypeTemplateDialog::accept);

		refreshTypeTree();
	}

	TypeTemplateDialog::~TypeTemplateDialog()
	{
		delete m_ui;
	}

	void TypeTemplateDialog::refreshTypeTree(int selectTypeId)
	{
		const int selectedId = selectTypeId != NoParentType ? selectTypeId : selectedType().id;

		m_reloading = true;
		m_ui->typeTree->clear();
		m_types = m_controller.types();

		QTreeWidgetItem* toSelect = nullptr;
		addNodes(m_ui->typeTree, nullptr, buildCategoryTree(m_types, std::map<int, int>()),
			selectedId, toSelect);
		if (toSelect == nullptr && m_ui->typeTree->topLevelItemCount() > 0)
		{
			toSelect = m_ui->typeTree->topLevelItem(0);
		}
		m_reloading = false;

		if (toSelect != nullptr)
		{
			m_ui->typeTree->setCurrentItem(toSelect);   // drives onTypeSelectionChanged()
		}
		else
		{
			onTypeSelectionChanged();
		}
	}

	void TypeTemplateDialog::onTypeSelectionChanged()
	{
		if (m_reloading)
		{
			return;
		}
		refreshTypeForm();
		refreshAttributeTable();
		refreshFileSlotTable();
		updateButtons();
	}

	void TypeTemplateDialog::refreshTypeForm()
	{
		const PartType type = selectedType();
		m_reloading = true;

		m_ui->nameEdit->setText(toQt(type.name));               // user data
		m_ui->domainCombo->setCurrentText(toQt(type.domain.empty() ? "generic" : type.domain));
		m_ui->kicadRelevantCheck->setChecked(type.kicadRelevant);
		m_ui->kicadCategoryEdit->setText(toQt(type.kicadCategory));
		m_ui->descriptionEdit->setPlainText(toQt(type.description));
		m_ui->searchKeywordsEdit->setPlainText(toQt(type.searchKeywords));
		// What the *ancestors* declare, not what this type does — those are the words it can only
		// keep or drop. A root type has none, which is what the widget's empty text is for.
		m_inheritedKeywords->setKeywords(
			toQt(SearchEngine::inheritedKeywords(m_types, type.parentTypeId)),
			toQt(type.excludedKeywords));
		m_nameTemplateEdit->setPlainText(toQt(type.nameTemplate));   // user data
		// What an empty field inherits, so a subtype's blank pattern does not read as "no name".
		const QString inheritedPattern = type.nameTemplate.empty()
			? nameTemplateFor(m_types, type.parentTypeId) : QString();
		m_nameTemplateEdit->setPlaceholderText(inheritedPattern.isEmpty()
			? tr("e.g. Resistor {resistance} {package}")
			: tr("inherited: %1").arg(inheritedPattern));

		// A type cannot descend from itself, directly or through a chain — the §2b walk would
		// then never reach a root. typeIdWithDescendants() is the same subtree the main window
		// resolves when it lists a category's parts.
		const std::vector<int> forbidden = type.id != NoParentType
			? typeIdWithDescendants(m_types, type.id) : std::vector<int>();
		m_ui->parentCombo->clear();
		m_ui->parentCombo->addItem(tr("(none — a root type)"), NoParentType);
		for (const PartType& candidate : m_types)
		{
			if (std::find(forbidden.begin(), forbidden.end(), candidate.id) != forbidden.end())
			{
				continue;
			}
			m_ui->parentCombo->addItem(toQt(candidate.name), candidate.id);   // user data
		}
		const int parentIndex = m_ui->parentCombo->findData(type.parentTypeId);
		m_ui->parentCombo->setCurrentIndex(parentIndex >= 0 ? parentIndex : 0);

		const int partCount = type.id != NoParentType ? m_controller.partCountOfType(type.id) : 0;
		m_ui->usageLabel->setText(tr("Used by %n part(s) of exactly this type.", "", partCount));

		m_reloading = false;
	}

	void TypeTemplateDialog::onTypeFieldEdited()
	{
		if (m_reloading)
		{
			return;
		}
		PartType type = selectedType();
		if (type.id == NoParentType)
		{
			return;
		}

		const int previousParent = type.parentTypeId;
		type.name = m_ui->nameEdit->text().trimmed().toStdString();
		type.domain = m_ui->domainCombo->currentText().toStdString();
		type.kicadRelevant = m_ui->kicadRelevantCheck->isChecked();
		type.kicadCategory = m_ui->kicadCategoryEdit->text().trimmed().toStdString();
		type.parentTypeId = m_ui->parentCombo->currentData().toInt();
		type.description = m_ui->descriptionEdit->toPlainText().toStdString();
		type.searchKeywords = m_ui->searchKeywordsEdit->toPlainText().toStdString();
		type.excludedKeywords = m_inheritedKeywords->excludedKeywords().toStdString();
		type.nameTemplate = m_nameTemplateEdit->toPlainText().toStdString();
		if (!m_controller.updateType(type))
		{
			return;
		}

		// Keep the cache in step rather than rebuilding the tree on every keystroke — a rebuild
		// would take the focus out of the field being typed into. Only the two things the tree
		// itself shows are repainted.
		for (PartType& cached : m_types)
		{
			if (cached.id == type.id)
			{
				cached = type;
			}
		}
		if (QTreeWidgetItem* item = m_ui->typeTree->currentItem())
		{
			item->setText(0, toQt(type.name));
		}
		m_ui->kicadCategoryEdit->setEnabled(type.kicadRelevant);
		if (type.parentTypeId != previousParent)
		{
			// A different parent means a different set of inherited rows, and a different place
			// in the forest — both tables and the tree have to be re-read.
			refreshTypeTree();
		}
	}

	void TypeTemplateDialog::updateNameTemplateFeedback()
	{
		m_knownTemplateKeys.clear();
		for (const PartTypeAttribute& attribute : m_attributes)
		{
			m_knownTemplateKeys.append(toQt(attribute.key));
		}
		m_knownTemplateKeys << QStringLiteral("manufacturer")
			<< QStringLiteral("mpn") << QStringLiteral("package");
		// **Only from here, never from the field's own textChanged.** rehighlight() walks the
		// document inside an edit block, which makes it emit contentsChanged — and that is
		// textChanged — so a repaint asked for while handling a keystroke asks for another one
		// forever. Typing needs no rehighlight anyway: the highlighter is called on the block that
		// changed as part of the edit. This is only for when the *key set* moved under it.
		m_nameHighlighter->rehighlight();
		updateNamePreview();
	}

	void TypeTemplateDialog::updateNamePreview()
	{
		// An invented part with a value in every field. Made up rather than read out of the
		// database on purpose: a category with no parts in it yet is exactly when the pattern is
		// being written, and a preview that only appears once the first part exists would be
		// missing at the one moment it is needed.
		const PartType type = selectedType();
		std::map<std::string, AttributeValue> values;
		for (const PartTypeAttribute& attribute : m_attributes)
		{
			AttributeValue value;
			value.present = true;
			switch (attribute.datatype)
			{
			case AttributeDataType::Dimension:
			case AttributeDataType::Number:
				value.number = 4700.0;   // reads as "4.7 k<unit>" through the §2a formatter
				break;
			case AttributeDataType::Bool:
				value.flag = true;
				break;
			case AttributeDataType::Enum:
				value.text = attribute.enumOptions.empty()
					? toQt(attribute.label) : toQt(attribute.enumOptions.front());
				break;
			default:
				value.text = toQt(attribute.label);
				break;
			}
			values[attribute.key] = value;
		}

		Part sample;
		sample.partTypeId = type.id;
		sample.manufacturer = "Acme";
		sample.mpn = "ABC-123";
		sample.package = "0603";
		sample.attributes = writeAttributesJson(m_attributes, values).toStdString();

		QString pattern = m_nameTemplateEdit->toPlainText();
		if (pattern.trimmed().isEmpty())
		{
			pattern = nameTemplateFor(m_types, type.parentTypeId);   // what an empty field inherits
		}
		const QString example = renderNameTemplate(pattern, sample, m_attributes);
		m_namePreviewLabel->setText(example.isEmpty()
			? tr("No pattern yet, so parts of this type are named by hand.")
			: tr("Example: %1").arg(example));
	}

	void TypeTemplateDialog::refreshInsertKeyMenu()
	{
		// Rebuilt each time it opens rather than on every table edit: an attribute added a moment
		// ago has to be in it, and this is the only moment anyone can tell.
		m_insertKeyMenu->clear();
		auto addKey = [this](const QString& key, const QString& label)
		{
			QAction* action = m_insertKeyMenu->addAction(label);
			connect(action, &QAction::triggered, this, [this, key]()
				{
					m_nameTemplateEdit->insertPlainText(QLatin1Char('{') + key + QLatin1Char('}'));
					// textChanged has already run updateNameTemplateFeedback() and the save.
				});
		};
		for (const PartTypeAttribute& attribute : m_attributes)
		{
			// Label and key both: the label is what the user knows the field by, the key is what
			// the pattern will read once inserted, and neither on its own is enough to pick from.
			addKey(toQt(attribute.key), tr("%1  —  {%2}")
				.arg(toQt(attribute.label), toQt(attribute.key)));
		}
		m_insertKeyMenu->addSeparator();
		addKey(QStringLiteral("manufacturer"), tr("Manufacturer  —  {manufacturer}"));
		addKey(QStringLiteral("mpn"), tr("MPN  —  {mpn}"));
		addKey(QStringLiteral("package"), tr("Package  —  {package}"));
	}

	PartType TypeTemplateDialog::selectedType() const
	{
		QTreeWidgetItem* item = m_ui->typeTree->currentItem();
		if (item == nullptr || !item->isSelected())
		{
			return PartType();
		}
		const int typeId = item->data(0, TypeIdRole).toInt();
		for (const PartType& type : m_types)
		{
			if (type.id == typeId)
			{
				return type;
			}
		}
		return PartType();
	}

	void TypeTemplateDialog::onNewType()
	{
		bool confirmed = false;
		const QString name = QInputDialog::getText(this, tr("New Type"), tr("Type name:"),
			QLineEdit::Normal, QString(), &confirmed);
		if (!confirmed || name.trimmed().isEmpty())
		{
			return;
		}

		// Born as a root with no attributes of its own: the parent is picked afterwards in the
		// form, where the combo can refuse the choices that would build a cycle.
		PartType type;
		type.name = name.trimmed().toStdString();
		type.domain = "generic";
		const int newId = m_controller.createType(type);
		if (newId == NoParentType)
		{
			QMessageBox::warning(this, tr("Could not create type"),
				tr("The type could not be created."));
			return;
		}

		refreshTypeTree(newId);
	}

	void TypeTemplateDialog::onDeleteType()
	{
		const PartType type = selectedType();
		if (type.id == NoParentType)
		{
			return;
		}

		const TypeDeletionBlock block = typeDeletionBlock(m_types, type.id,
			m_controller.partCountOfType(type.id));
		if (block.blocked())
		{
			// Naming both counts, because "cannot delete" without the number leaves the user
			// hunting for what is in the way.
			QStringList reasons;
			if (block.partCount > 0)
			{
				reasons << tr("%n part(s) still use it", "", block.partCount);
			}
			if (block.childTypeCount > 0)
			{
				reasons << tr("%n type(s) inherit from it", "", block.childTypeCount);
			}
			QMessageBox::information(this, tr("Cannot delete type"),
				tr("\"%1\" cannot be deleted: %2. Move or delete those first.")
					.arg(toQt(type.name), reasons.join(QStringLiteral(", "))));
			return;
		}

		if (QMessageBox::question(this, tr("Delete type"),
			tr("Delete \"%1\" and the attributes and file slots it declares?").arg(toQt(type.name)))
			!= QMessageBox::Yes)
		{
			return;
		}
		m_controller.deleteType(type.id);
		m_ui->typeTree->setCurrentItem(nullptr);
		refreshTypeTree();
	}

	void TypeTemplateDialog::refreshAttributeTable(int selectAttributeId)
	{
		const PartType type = selectedType();
		m_attributes = type.id != NoParentType ? m_controller.attributesFor(type.id)
			: std::vector<PartTypeAttribute>();

		m_reloading = true;
		// setRowCount(0), not clearContents(): the latter drops the items but leaves the unit and
		// datatype cell widgets — and their connections — alive on rows that no longer exist.
		m_ui->attributeTable->setRowCount(0);
		m_ui->attributeTable->setRowCount(static_cast<int>(m_attributes.size()));
		for (int row = 0; row < static_cast<int>(m_attributes.size()); ++row)
		{
			const PartTypeAttribute& attribute = m_attributes[static_cast<size_t>(row)];
			const bool own = attribute.partTypeId == type.id;
			const QString ancestor = typeNameOf(m_types, attribute.partTypeId);

			QTableWidgetItem* required = new QTableWidgetItem();
			required->setData(RowIdRole, attribute.id);
			required->setCheckState(attribute.required ? Qt::Checked : Qt::Unchecked);
			QTableWidgetItem* key = new QTableWidgetItem(toQt(attribute.key));       // user data
			key->setFlags(key->flags() & ~Qt::ItemIsEditable);
			key->setToolTip(m_ui->attributeTable->horizontalHeaderItem(AttrKey)->toolTip());
			QTableWidgetItem* label = new QTableWidgetItem(toQt(attribute.label));   // user data
			QTableWidgetItem* searchable = new QTableWidgetItem();
			searchable->setCheckState(attribute.searchable ? Qt::Checked : Qt::Unchecked);
			QTableWidgetItem* order = new QTableWidgetItem(QString::number(attribute.sortOrder));
			order->setFlags(order->flags() & ~Qt::ItemIsEditable);

			if (!own)
			{
				markInherited(required, ancestor);
				markInherited(key, ancestor);
				markInherited(label, ancestor);
				markInherited(searchable, ancestor);
				markInherited(order, ancestor);
			}
			m_ui->attributeTable->setItem(row, AttrRequired, required);
			m_ui->attributeTable->setItem(row, AttrKey, key);
			m_ui->attributeTable->setItem(row, AttrLabel, label);
			m_ui->attributeTable->setItem(row, AttrSearchable, searchable);
			m_ui->attributeTable->setItem(row, AttrOrder, order);

			if (own)
			{
				// Unit and datatype are fixed vocabularies, so they are combos rather than typed
				// text — the only cells here that Designer cannot express, since there is one per
				// data row. An inherited row gets plain grey text instead, same as the rest of it.
				QComboBox* unit = new QComboBox(m_ui->attributeTable);
				fillUnitCombo(unit);
				unit->setCurrentIndex(std::max(0, unit->findData(toQt(attribute.unit))));
				QComboBox* datatype = new QComboBox(m_ui->attributeTable);
				fillDatatypeCombo(datatype);
				datatype->setCurrentIndex(std::max(0,
					datatype->findData(static_cast<int>(attribute.datatype))));
				// Connected after the current index is set, so restoring the stored value is not
				// itself read back as an edit.
				connect(unit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, row](int) { saveAttributeRow(row); });
				connect(datatype, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, row](int) { saveAttributeRow(row); updateButtons(); });
				m_ui->attributeTable->setCellWidget(row, AttrUnit, unit);
				m_ui->attributeTable->setCellWidget(row, AttrDatatype, datatype);
			}
			else
			{
				QTableWidgetItem* unit = new QTableWidgetItem(toQt(attribute.unit));
				QTableWidgetItem* datatype = new QTableWidgetItem(toQt(toString(attribute.datatype)));
				markInherited(unit, ancestor);
				markInherited(datatype, ancestor);
				m_ui->attributeTable->setItem(row, AttrUnit, unit);
				m_ui->attributeTable->setItem(row, AttrDatatype, datatype);
			}
		}
		m_reloading = false;
		// A reordered row is still the row the user is working with — leaving it deselected means
		// pressing "Move down" twice needs a click in between, which is not what a move button is
		// for. Selecting it also re-enables the buttons for its new position.
		selectRowById(m_ui->attributeTable, AttrRequired, selectAttributeId);
		onAttributeSelectionChanged();
		// m_attributes was just rebuilt, and it is what decides which placeholders are green and
		// what the example name is made of. This is the only place that list changes.
		updateNameTemplateFeedback();
	}

	void TypeTemplateDialog::refreshFileSlotTable(int selectFileSlotId)
	{
		const PartType type = selectedType();
		m_fileSlots = type.id != NoParentType ? m_controller.fileSlotsFor(type.id)
			: std::vector<PartTypeFileSlot>();

		m_reloading = true;
		m_ui->fileSlotTable->setRowCount(0);
		m_ui->fileSlotTable->setRowCount(static_cast<int>(m_fileSlots.size()));
		for (int row = 0; row < static_cast<int>(m_fileSlots.size()); ++row)
		{
			const PartTypeFileSlot& slot = m_fileSlots[static_cast<size_t>(row)];
			const bool own = slot.partTypeId == type.id;
			const QString ancestor = typeNameOf(m_types, slot.partTypeId);

			QTableWidgetItem* required = new QTableWidgetItem();
			required->setData(RowIdRole, slot.id);
			required->setCheckState(slot.required ? Qt::Checked : Qt::Unchecked);
			QTableWidgetItem* role = new QTableWidgetItem(toQt(slot.role));
			role->setFlags(role->flags() & ~Qt::ItemIsEditable);
			role->setToolTip(tr("The file role this slot expects. It is fixed once the slot exists, "
				"so a file already attached under it keeps being found."));
			QTableWidgetItem* label = new QTableWidgetItem(toQt(slot.label));   // user data
			QTableWidgetItem* order = new QTableWidgetItem(QString::number(slot.sortOrder));
			order->setFlags(order->flags() & ~Qt::ItemIsEditable);

			if (!own)
			{
				markInherited(required, ancestor);
				markInherited(role, ancestor);
				markInherited(label, ancestor);
				markInherited(order, ancestor);
			}
			m_ui->fileSlotTable->setItem(row, SlotRequired, required);
			m_ui->fileSlotTable->setItem(row, SlotRole, role);
			m_ui->fileSlotTable->setItem(row, SlotLabel, label);
			m_ui->fileSlotTable->setItem(row, SlotOrder, order);
		}
		m_reloading = false;
		selectRowById(m_ui->fileSlotTable, SlotRequired, selectFileSlotId);
		onFileSlotSelectionChanged();
	}

	void TypeTemplateDialog::updateButtons()
	{
		const PartType type = selectedType();
		const bool hasType = type.id != NoParentType;
		m_ui->deleteTypeButton->setEnabled(hasType);
		m_ui->nameEdit->setEnabled(hasType);
		m_ui->domainCombo->setEnabled(hasType);
		m_ui->kicadRelevantCheck->setEnabled(hasType);
		m_ui->kicadCategoryEdit->setEnabled(hasType && type.kicadRelevant);
		m_ui->parentCombo->setEnabled(hasType);
		m_ui->descriptionEdit->setEnabled(hasType);
		m_ui->searchKeywordsEdit->setEnabled(hasType);
		m_inheritedKeywords->setEnabled(hasType);
		m_nameTemplateEdit->setEnabled(hasType);
		m_ui->addAttributeButton->setEnabled(hasType);
		m_ui->addFileSlotButton->setEnabled(hasType);

		// Only a row this type owns can be removed, reordered or given options — an inherited row
		// belongs to the ancestor that declares it.
		const std::vector<PartTypeAttribute> own = ownAttributes();
		const int attributeId = selectedAttributeId();
		int position = -1;
		for (int i = 0; i < static_cast<int>(own.size()); ++i)
		{
			if (own[static_cast<size_t>(i)].id == attributeId)
			{
				position = i;
			}
		}
		m_ui->removeAttributeButton->setEnabled(position >= 0);
		m_ui->attributeUpButton->setEnabled(position > 0);
		m_ui->attributeDownButton->setEnabled(position >= 0 && position < static_cast<int>(own.size()) - 1);
		m_ui->enumOptionsButton->setEnabled(position >= 0
			&& own[static_cast<size_t>(position)].datatype == AttributeDataType::Enum);

		const std::vector<PartTypeFileSlot> ownSlots = ownFileSlots();
		const int slotId = selectedFileSlotId();
		int slotPosition = -1;
		for (int i = 0; i < static_cast<int>(ownSlots.size()); ++i)
		{
			if (ownSlots[static_cast<size_t>(i)].id == slotId)
			{
				slotPosition = i;
			}
		}
		m_ui->removeFileSlotButton->setEnabled(slotPosition >= 0);
		m_ui->fileSlotUpButton->setEnabled(slotPosition > 0);
		m_ui->fileSlotDownButton->setEnabled(slotPosition >= 0
			&& slotPosition < static_cast<int>(ownSlots.size()) - 1);
	}

	std::vector<PartTypeAttribute> TypeTemplateDialog::ownAttributes() const
	{
		const int typeId = selectedType().id;
		std::vector<PartTypeAttribute> own;
		for (const PartTypeAttribute& attribute : m_attributes)
		{
			if (attribute.partTypeId == typeId)
			{
				own.push_back(attribute);
			}
		}
		return own;
	}

	std::vector<PartTypeFileSlot> TypeTemplateDialog::ownFileSlots() const
	{
		const int typeId = selectedType().id;
		std::vector<PartTypeFileSlot> own;
		for (const PartTypeFileSlot& slot : m_fileSlots)
		{
			if (slot.partTypeId == typeId)
			{
				own.push_back(slot);
			}
		}
		return own;
	}

	int TypeTemplateDialog::selectedAttributeId() const
	{
		QTableWidgetItem* item = m_ui->attributeTable->item(m_ui->attributeTable->currentRow(), AttrRequired);
		return (item != nullptr && item->isSelected()) ? item->data(RowIdRole).toInt() : 0;
	}

	int TypeTemplateDialog::selectedFileSlotId() const
	{
		QTableWidgetItem* item = m_ui->fileSlotTable->item(m_ui->fileSlotTable->currentRow(), SlotRequired);
		return (item != nullptr && item->isSelected()) ? item->data(RowIdRole).toInt() : 0;
	}

	void TypeTemplateDialog::onAttributeSelectionChanged()
	{
		// Emptying the table clears its selection, which lands here mid-rebuild. Without this the
		// m_reloading = false at the bottom would end the rebuild's guard early, and the very next
		// setItem() would be read back as a user edit — into a row whose other cells do not exist
		// yet. That is the "Move down" crash. refreshAttributeTable() calls this itself once it is
		// finished, so nothing is lost by ignoring the rebuild's own signals.
		if (m_reloading)
		{
			return;
		}

		const int attributeId = selectedAttributeId();
		m_reloading = true;
		QString tooltip;
		QString label;
		bool editable = false;
		for (const PartTypeAttribute& attribute : m_attributes)
		{
			if (attribute.id == attributeId && attributeId != 0)
			{
				tooltip = toQt(attribute.tooltip);   // user data
				label = toQt(attribute.key);
				editable = attribute.partTypeId == selectedType().id;
			}
		}
		m_ui->tooltipEdit->setPlainText(tooltip);
		m_ui->tooltipEdit->setEnabled(editable);
		m_ui->tooltipLabel->setText(label.isEmpty()
			? tr("Tooltip — select an attribute to write the hint the New Part screen shows for it.")
			: tr("Tooltip for \"%1\" — shown as the ⓘ hint on the New Part screen.").arg(label));
		m_reloading = false;
		updateButtons();
	}

	void TypeTemplateDialog::onFileSlotSelectionChanged()
	{
		updateButtons();
	}

	void TypeTemplateDialog::onAttributeItemChanged(QTableWidgetItem* item)
	{
		if (m_reloading || item == nullptr)
		{
			return;
		}
		saveAttributeRow(item->row());
	}

	void TypeTemplateDialog::onFileSlotItemChanged(QTableWidgetItem* item)
	{
		if (m_reloading || item == nullptr)
		{
			return;
		}
		saveFileSlotRow(item->row());
	}

	void TypeTemplateDialog::saveAttributeRow(int row)
	{
		QTableWidgetItem* idItem = m_ui->attributeTable->item(row, AttrRequired);
		QTableWidgetItem* labelItem = m_ui->attributeTable->item(row, AttrLabel);
		QTableWidgetItem* searchableItem = m_ui->attributeTable->item(row, AttrSearchable);
		// A half-built row is not an edit. Belt to the m_reloading braces: every path into here
		// reads all three cells, and a row is populated one cell at a time.
		if (idItem == nullptr || labelItem == nullptr || searchableItem == nullptr)
		{
			return;
		}
		const int attributeId = idItem->data(RowIdRole).toInt();
		const int typeId = selectedType().id;
		for (PartTypeAttribute& attribute : m_attributes)
		{
			if (attribute.id != attributeId || attribute.partTypeId != typeId)
			{
				continue;
			}
			attribute.required = idItem->checkState() == Qt::Checked;
			attribute.label = labelItem->text().toStdString();
			attribute.searchable = searchableItem->checkState() == Qt::Checked;
			if (QComboBox* unit = qobject_cast<QComboBox*>(m_ui->attributeTable->cellWidget(row, AttrUnit)))
			{
				attribute.unit = unit->currentData().toString().toStdString();
			}
			if (QComboBox* datatype = qobject_cast<QComboBox*>(m_ui->attributeTable->cellWidget(row, AttrDatatype)))
			{
				attribute.datatype = static_cast<AttributeDataType>(datatype->currentData().toInt());
			}
			// The cached row is updated first and written second, so `enumOptions` and `tooltip` —
			// neither of which this table paints — go back unchanged rather than empty.
			m_controller.updateAttribute(attribute);
			return;
		}
	}

	void TypeTemplateDialog::saveFileSlotRow(int row)
	{
		QTableWidgetItem* idItem = m_ui->fileSlotTable->item(row, SlotRequired);
		QTableWidgetItem* labelItem = m_ui->fileSlotTable->item(row, SlotLabel);
		if (idItem == nullptr || labelItem == nullptr)
		{
			return;
		}
		const int slotId = idItem->data(RowIdRole).toInt();
		const int typeId = selectedType().id;
		for (PartTypeFileSlot& slot : m_fileSlots)
		{
			if (slot.id != slotId || slot.partTypeId != typeId)
			{
				continue;
			}
			slot.required = idItem->checkState() == Qt::Checked;
			slot.label = labelItem->text().toStdString();
			m_controller.updateFileSlot(slot);
			return;
		}
	}

	void TypeTemplateDialog::onTooltipEdited()
	{
		if (m_reloading)
		{
			return;
		}
		const int attributeId = selectedAttributeId();
		const int typeId = selectedType().id;
		for (PartTypeAttribute& attribute : m_attributes)
		{
			if (attribute.id != attributeId || attribute.partTypeId != typeId)
			{
				continue;
			}
			// ponytail: one UPDATE per keystroke. §10 asks for a debounce; a local SQLite write is
			// far under a keypress interval, so add the timer only if a profile says otherwise.
			attribute.tooltip = m_ui->tooltipEdit->toPlainText().toStdString();
			m_controller.updateAttribute(attribute);
			return;
		}
	}

	void TypeTemplateDialog::onAddAttribute()
	{
		const PartType type = selectedType();
		if (type.id == NoParentType)
		{
			return;
		}

		// The key is asked for up front rather than typed into the table, because this is the one
		// moment it can still be chosen — see the header. Everything else about the attribute is
		// edited in place afterwards.
		bool confirmed = false;
		const QString key = QInputDialog::getText(this, tr("Add attribute"),
			tr("Key (lowercase letters, digits and underscores; cannot be changed later):"),
			QLineEdit::Normal, QString(), &confirmed);
		if (!confirmed)
		{
			return;
		}

		const std::string trimmed = key.trimmed().toStdString();
		switch (attributeKeyProblem(trimmed, m_attributes))
		{
		case AttributeKeyProblem::Empty:
			QMessageBox::warning(this, tr("Invalid key"), tr("The key cannot be empty."));
			return;
		case AttributeKeyProblem::BadFormat:
			QMessageBox::warning(this, tr("Invalid key"),
				tr("The key must start with a lowercase letter and contain only lowercase letters, "
					"digits and underscores."));
			return;
		case AttributeKeyProblem::Duplicate:
			QMessageBox::warning(this, tr("Invalid key"),
				tr("\"%1\" is already an attribute of this type, or of one it inherits from.")
					.arg(toQt(trimmed)));
			return;
		case AttributeKeyProblem::None:
			break;
		}

		PartTypeAttribute attribute;
		attribute.partTypeId = type.id;
		attribute.key = trimmed;
		attribute.label = trimmed;   // a starting point the user edits in the Label cell
		attribute.datatype = AttributeDataType::Text;
		attribute.sortOrder = static_cast<int>(ownAttributes().size());
		if (m_controller.createAttribute(attribute) == 0)
		{
			QMessageBox::warning(this, tr("Could not add attribute"),
				tr("The attribute could not be created."));
			return;
		}
		refreshAttributeTable();
		updateButtons();
	}

	void TypeTemplateDialog::onRemoveAttribute()
	{
		const int attributeId = selectedAttributeId();
		for (const PartTypeAttribute& attribute : ownAttributes())
		{
			if (attribute.id != attributeId)
			{
				continue;
			}
			// The values parts already hold under this key are not deleted with it — they simply
			// stop being read. Saying so, because "remove" over a field 400 parts filled in reads
			// as though it would wipe them.
			if (QMessageBox::question(this, tr("Remove attribute"),
				tr("Remove \"%1\" from this type? Values parts already carry under it are kept in "
					"the database but are no longer shown.").arg(toQt(attribute.key)))
				!= QMessageBox::Yes)
			{
				return;
			}
			m_controller.deleteAttribute(attributeId);
			refreshAttributeTable();
			updateButtons();
			return;
		}
	}

	void TypeTemplateDialog::onEditEnumOptions()
	{
		const int attributeId = selectedAttributeId();
		const int typeId = selectedType().id;
		for (PartTypeAttribute& attribute : m_attributes)
		{
			if (attribute.id != attributeId || attribute.partTypeId != typeId)
			{
				continue;
			}
			// One option per line, never a comma-separated field: a vocabulary typed into one line
			// makes a value containing the separator unrepresentable, which is exactly how the tag
			// names went wrong (ISSUES.md).
			QStringList lines;
			for (const std::string& option : attribute.enumOptions)
			{
				lines << toQt(option);   // user data
			}
			bool confirmed = false;
			const QString text = QInputDialog::getMultiLineText(this, tr("Enum options"),
				tr("The values \"%1\" may take, one per line:").arg(toQt(attribute.key)),
				lines.join(QLatin1Char('\n')), &confirmed);
			if (!confirmed)
			{
				return;
			}
			attribute.enumOptions.clear();
			for (const QString& line : text.split(QLatin1Char('\n')))
			{
				if (!line.trimmed().isEmpty())
				{
					attribute.enumOptions.push_back(line.trimmed().toStdString());
				}
			}
			m_controller.updateAttribute(attribute);
			return;
		}
	}

	void TypeTemplateDialog::moveSelectedAttribute(int offset)
	{
		const int movedId = selectedAttributeId();
		std::vector<PartTypeAttribute> own = ownAttributes();
		const int size = static_cast<int>(own.size());
		int position = -1;
		for (int i = 0; i < size; ++i)
		{
			if (own[static_cast<size_t>(i)].id == movedId)
			{
				position = i;
			}
		}
		const int target = position + offset;
		if (position < 0 || target < 0 || target >= size)
		{
			return;
		}
		std::swap(own[static_cast<size_t>(position)], own[static_cast<size_t>(target)]);

		// The whole own group is renumbered rather than just the pair that swapped: seeded rows can
		// share a sort_order, and writing only two into a tie leaves an order that reads unchanged.
		// ponytail: this reorders the type's OWN rows only — §2b says a subtype cannot move an
		// inherited row relative to its ancestor's others, so there is nothing else to renumber.
		for (int i = 0; i < size; ++i)
		{
			PartTypeAttribute attribute = own[static_cast<size_t>(i)];
			if (attribute.sortOrder == i)
			{
				continue;
			}
			attribute.sortOrder = i;
			m_controller.updateAttribute(attribute);
		}
		refreshAttributeTable(movedId);
		updateButtons();
	}

	void TypeTemplateDialog::onAddFileSlot()
	{
		const PartType type = selectedType();
		if (type.id == NoParentType)
		{
			return;
		}

		bool confirmed = false;
		const QString role = QInputDialog::getItem(this, tr("Add file slot"),
			tr("File role (cannot be changed later):"), knownFileRoles(), 0, false, &confirmed);
		if (!confirmed)
		{
			return;
		}

		const std::string chosen = role.toStdString();
		if (fileSlotRoleProblem(chosen, m_fileSlots) == AttributeKeyProblem::Duplicate)
		{
			QMessageBox::warning(this, tr("Role already used"),
				tr("\"%1\" is already a file slot of this type, or of one it inherits from.")
					.arg(role));
			return;
		}

		PartTypeFileSlot slot;
		slot.partTypeId = type.id;
		slot.role = chosen;
		slot.label = chosen;   // a starting point the user edits in the Label cell
		slot.sortOrder = static_cast<int>(ownFileSlots().size());
		if (m_controller.createFileSlot(slot) == 0)
		{
			QMessageBox::warning(this, tr("Could not add file slot"),
				tr("The file slot could not be created."));
			return;
		}
		refreshFileSlotTable();
		updateButtons();
	}

	void TypeTemplateDialog::onRemoveFileSlot()
	{
		const int slotId = selectedFileSlotId();
		for (const PartTypeFileSlot& slot : ownFileSlots())
		{
			if (slot.id != slotId)
			{
				continue;
			}
			// Only the expectation goes; files parts already carry under the role stay attached.
			if (QMessageBox::question(this, tr("Remove file slot"),
				tr("Remove \"%1\" from this type? Files already attached to parts are kept.")
					.arg(toQt(slot.role))) != QMessageBox::Yes)
			{
				return;
			}
			m_controller.deleteFileSlot(slotId);
			refreshFileSlotTable();
			updateButtons();
			return;
		}
	}

	void TypeTemplateDialog::moveSelectedFileSlot(int offset)
	{
		const int movedId = selectedFileSlotId();
		std::vector<PartTypeFileSlot> own = ownFileSlots();
		const int size = static_cast<int>(own.size());
		int position = -1;
		for (int i = 0; i < size; ++i)
		{
			if (own[static_cast<size_t>(i)].id == movedId)
			{
				position = i;
			}
		}
		const int target = position + offset;
		if (position < 0 || target < 0 || target >= size)
		{
			return;
		}
		std::swap(own[static_cast<size_t>(position)], own[static_cast<size_t>(target)]);

		for (int i = 0; i < size; ++i)
		{
			PartTypeFileSlot slot = own[static_cast<size_t>(i)];
			if (slot.sortOrder == i)
			{
				continue;
			}
			slot.sortOrder = i;
			m_controller.updateFileSlot(slot);
		}
		refreshFileSlotTable(movedId);
		updateButtons();
	}

}
