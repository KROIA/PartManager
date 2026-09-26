#include "ui/PartManager_CategoryReconcileDialog.h"
#include "ui_PartManager_CategoryReconcileDialog.h"

#include "persistence/PartManager_PartRepository.h"
#include "ui/PartManager_MovePartDialog.h"
#include "ui/PartManager_PartEditorDialog.h"
#include "ui/PartManager_PartTypePickerDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <algorithm>
#include <map>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		constexpr int TypeIdRole = Qt::UserRole;
		constexpr int PartIdRole = Qt::UserRole;
		// Whether the user has already said something about this unmatched row — either by moving
		// its parts or by answering "keep it". Only a display state; nothing in the database
		// records it, because nothing in the database changed.
		constexpr int SettledRole = Qt::UserRole + 1;

		enum UnmatchedColumn
		{
			UnmatchedCategory = 0,
			UnmatchedParts,
			UnmatchedState,
			UnmatchedColumnCount
		};

		enum IncompleteColumn
		{
			IncompletePart = 0,
			IncompleteCategory,
			IncompleteMissing,
			IncompleteColumnCount
		};

		// The labels of the required attributes a part has no value for, in the order the category
		// declares them. Labels rather than keys: the key is the ascii name the value is stored
		// under, the label is what the part editor's row is actually called, and the user is being
		// sent to that row.
		QStringList missingRequiredLabels(const std::vector<PartTypeAttribute>& attributes,
			const Part& part)
		{
			const std::map<std::string, AttributeValue> values =
				readAttributesJson(toQt(part.attributes), attributes);
			const std::vector<std::string> missing = missingRequiredKeys(attributes, values);
			QStringList labels;
			for (const PartTypeAttribute& attribute : attributes)
			{
				if (std::find(missing.begin(), missing.end(), attribute.key) != missing.end())
				{
					// The user's own label for their own field — not app chrome.
					labels.append(toQt(attribute.label.empty() ? attribute.key : attribute.label));
				}
			}
			return labels;
		}
	}

	CategoryReconcileDialog::CategoryReconcileDialog(DatabaseHandle* handle,
		const std::vector<int>& unmatchedLocalTypeIds, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::CategoryReconcileDialog)
		, m_handle(handle)
		, m_editor(handle)
		, m_unmatchedTypeIds(unmatchedLocalTypeIds)
		, m_types(handle != nullptr && handle->isOpen() ? m_editor.types() : std::vector<PartType>())
	{
		m_ui->setupUi(this);

		// Set here rather than in the .ui: the column headers are app chrome and have to go
		// through tr(), which Designer's <column> text cannot do without being marked notr.
		m_ui->unmatchedTree->setColumnCount(UnmatchedColumnCount);
		m_ui->unmatchedTree->setHeaderLabels(QStringList()
			<< tr("Category") << tr("Parts") << tr("Status"));
		m_ui->incompleteTree->setColumnCount(IncompleteColumnCount);
		m_ui->incompleteTree->setHeaderLabels(QStringList()
			<< tr("Part") << tr("Category") << tr("Missing"));

		connect(m_ui->mapButton, &QPushButton::clicked,
			this, &CategoryReconcileDialog::mapSelectedCategory);
		connect(m_ui->keepButton, &QPushButton::clicked,
			this, &CategoryReconcileDialog::keepSelectedCategory);
		connect(m_ui->editPartButton, &QPushButton::clicked,
			this, &CategoryReconcileDialog::editSelectedPart);
		connect(m_ui->refreshButton, &QPushButton::clicked,
			this, &CategoryReconcileDialog::refreshIncompleteParts);
		connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

		// Both "act on this row" buttons follow the selection rather than being always-on: with
		// nothing selected there is no row for them to be about.
		connect(m_ui->unmatchedTree, &QTreeWidget::itemSelectionChanged, this, [this]()
			{
				const bool hasRow = !m_ui->unmatchedTree->selectedItems().isEmpty();
				m_ui->mapButton->setEnabled(hasRow);
				m_ui->keepButton->setEnabled(hasRow);
			});
		connect(m_ui->incompleteTree, &QTreeWidget::itemSelectionChanged, this, [this]()
			{
				m_ui->editPartButton->setEnabled(!m_ui->incompleteTree->selectedItems().isEmpty());
			});
		// Double-click is the gesture the part table already uses to open a part; the same one
		// here means the list behaves like a list of parts and not like a form.
		connect(m_ui->incompleteTree, &QTreeWidget::itemDoubleClicked,
			this, &CategoryReconcileDialog::editSelectedPart);

		fillUnmatchedCategories();
		refreshIncompleteParts();
		// The starting total the progress bar measures against — see the header for why it is
		// frozen here instead of being re-read with the list.
		m_initialIncompleteCount = m_incompleteCount;
		updateProgress();

		// Opened cold from the category editor there is no plan, so tab one is empty and tab two
		// is the whole dialog. Landing on an empty tab would read as "nothing to do here at all".
		if (m_ui->unmatchedTree->topLevelItemCount() == 0)
		{
			m_ui->tabs->setCurrentIndex(1);
		}
	}

	CategoryReconcileDialog::~CategoryReconcileDialog()
	{
		delete m_ui;
	}

	int CategoryReconcileDialog::movedPartCount() const
	{
		return m_movedPartCount;
	}

	int CategoryReconcileDialog::incompletePartCount() const
	{
		return m_incompleteCount;
	}

	// ---------------------------------------------------------------------------------------
	// Tab one — categories nothing matched
	// ---------------------------------------------------------------------------------------

	void CategoryReconcileDialog::fillUnmatchedCategories()
	{
		m_ui->unmatchedTree->clear();
		if (m_handle == nullptr || !m_handle->isOpen())
		{
			return;
		}

		for (int typeId : m_unmatchedTypeIds)
		{
			const auto found = std::find_if(m_types.begin(), m_types.end(),
				[typeId](const PartType& type) { return type.id == typeId; });
			if (found == m_types.end())
			{
				// Deleted between the merge and this window being opened — reopened later that is
				// perfectly normal, and a row naming a category that is gone helps nobody.
				continue;
			}

			QTreeWidgetItem* item = new QTreeWidgetItem(m_ui->unmatchedTree);
			// The full path, not the leaf name: an unmatched "Ceramic" means something quite
			// different under "Capacitor" than under "Substrate", and the whole question here is
			// which of two similarly named categories this one is.
			item->setText(UnmatchedCategory, partTypePath(m_types, typeId));
			item->setData(UnmatchedCategory, TypeIdRole, typeId);
			item->setText(UnmatchedParts, QString::number(m_editor.partCountOfType(typeId)));
			item->setData(UnmatchedCategory, SettledRole, false);
		}
		m_ui->unmatchedTree->resizeColumnToContents(UnmatchedCategory);
	}

	int CategoryReconcileDialog::selectedUnmatchedTypeId() const
	{
		const QList<QTreeWidgetItem*> selected = m_ui->unmatchedTree->selectedItems();
		if (selected.isEmpty())
		{
			return NoParentType;
		}
		return selected.first()->data(UnmatchedCategory, TypeIdRole).toInt();
	}

	void CategoryReconcileDialog::mapSelectedCategory()
	{
		const int sourceTypeId = selectedUnmatchedTypeId();
		if (sourceTypeId == NoParentType || m_handle == nullptr || !m_handle->isOpen())
		{
			return;
		}

		// The existing picker, with the source itself and everything under it hidden: moving a
		// category's parts into itself is a no-op, and into its own subtype is a decision that
		// belongs in the type editor rather than here.
		PartTypePickerDialog picker(m_handle, NoParentType, sourceTypeId, this);
		picker.setNoTypeButtonText(tr("Leave them without a category"));
		if (picker.exec() != QDialog::Accepted)
		{
			return;
		}
		const int targetTypeId = picker.selectedTypeId();
		if (targetTypeId == NoParentType || targetTypeId == sourceTypeId)
		{
			return;
		}

		bool stopped = false;
		const int moved = movePartsOfType(sourceTypeId, targetTypeId, stopped);
		m_movedPartCount += moved;

		QTreeWidgetItem* item = m_ui->unmatchedTree->selectedItems().first();
		item->setText(UnmatchedParts, QString::number(m_editor.partCountOfType(sourceTypeId)));
		if (!stopped)
		{
			item->setData(UnmatchedCategory, SettledRole, true);
			item->setText(UnmatchedState, tr("%n part(s) moved to %1", "", moved)
				.arg(partTypePath(m_types, targetTypeId)));
		}
		else
		{
			// Stopped halfway is its own state: what moved stays moved, and the row still has
			// something in it, so calling it settled would hide the rest.
			item->setText(UnmatchedState, tr("Stopped after %n part(s)", "", moved));
		}

		// A move can fill a §11 gap (the target asked for what the part already had) or open one,
		// so tab two is re-read rather than left to the user to refresh.
		refreshIncompleteParts();
		updateProgress();
	}

	void CategoryReconcileDialog::keepSelectedCategory()
	{
		const QList<QTreeWidgetItem*> selected = m_ui->unmatchedTree->selectedItems();
		if (selected.isEmpty())
		{
			return;
		}
		// Deliberately writes nothing anywhere. "I looked at this and it is fine" is an answer,
		// and the only thing it changes is that the row stops asking.
		QTreeWidgetItem* item = selected.first();
		item->setData(UnmatchedCategory, SettledRole, true);
		item->setText(UnmatchedState, tr("Kept as it is"));
		updateProgress();
	}

	int CategoryReconcileDialog::movePartsOfType(int sourceTypeId, int targetTypeId, bool& outStopped)
	{
		outStopped = false;

		PartType targetType;
		for (const PartType& type : m_types)
		{
			if (type.id == targetTypeId)
			{
				targetType = type;
			}
		}
		if (targetType.id == NoParentType)
		{
			return 0;
		}

		int moved = 0;
		// Read once up front. Saving a part changes its part_type_id, so a list re-read per
		// iteration would shrink underneath the loop.
		const std::vector<Part> parts =
			PartRepository::listParts(m_handle->connection(), sourceTypeId);
		for (const Part& original : parts)
		{
			Part part = original;
			// The clean case costs nothing: every value carries over and nothing the target
			// requires is empty. Forty clean parts must not cost forty dialogs — that is the whole
			// reason isCleanMove() is static.
			if (MovePartDialog::isCleanMove(m_editor, part, targetTypeId))
			{
				part.partTypeId = targetTypeId;
			}
			else
			{
				MovePartDialog dialog(m_editor, part, targetType, this);
				if (dialog.exec() != QDialog::Accepted)
				{
					// Cancelling one part cancels the rest. The alternative — carrying on to the
					// next dialog — makes "no" mean "ask me again", and there is no way out of a
					// forty-part run short of answering forty times.
					outStopped = true;
					break;
				}
				part = dialog.movedPart();
			}

			if (!m_editor.savePart(part))
			{
				QMessageBox::warning(this, tr("Could not move the part"),
					tr("The database rejected the change to \"%1\" — it is still in its old "
					   "category, and the rest were left alone.")
					.arg(toQt(part.name)));   // user data
				outStopped = true;
				break;
			}
			++moved;
		}
		return moved;
	}

	// ---------------------------------------------------------------------------------------
	// Tab two — parts a merge left incomplete (§11)
	// ---------------------------------------------------------------------------------------

	void CategoryReconcileDialog::refreshIncompleteParts()
	{
		m_ui->incompleteTree->clear();
		m_incompleteCount = 0;
		if (m_handle == nullptr || !m_handle->isOpen())
		{
			updateProgress();
			return;
		}

		// Re-read from the database every time, never from the plan — see the header. The plan
		// describes an import that has already happened; a part may have been filled in since, and
		// sending the user to a field that is already filled is worse than not listing it.
		m_types = m_editor.types();
		const std::vector<Part> parts = PartRepository::listParts(m_handle->connection());

		// One effectiveAttributes() walk per *type*, not per part: §2b resolution is a query per
		// ancestor, and a catalogue of a thousand parts over twenty types would otherwise pay for
		// it a thousand times.
		std::map<int, std::vector<PartTypeAttribute>> attributesByType;
		for (const Part& part : parts)
		{
			auto found = attributesByType.find(part.partTypeId);
			if (found == attributesByType.end())
			{
				found = attributesByType.emplace(part.partTypeId,
					m_editor.attributesFor(part.partTypeId)).first;
			}

			const QStringList missing = missingRequiredLabels(found->second, part);
			if (missing.isEmpty())
			{
				continue;
			}

			QTreeWidgetItem* item = new QTreeWidgetItem(m_ui->incompleteTree);
			item->setText(IncompletePart, toQt(part.name));   // user data
			item->setData(IncompletePart, PartIdRole, part.id);
			item->setText(IncompleteCategory, partTypePath(m_types, part.partTypeId));
			item->setText(IncompleteMissing, missing.join(QStringLiteral(", ")));
			++m_incompleteCount;
		}
		m_ui->incompleteTree->resizeColumnToContents(IncompletePart);

		m_ui->editPartButton->setEnabled(false);
		updateProgress();
	}

	void CategoryReconcileDialog::editSelectedPart()
	{
		const QList<QTreeWidgetItem*> selected = m_ui->incompleteTree->selectedItems();
		if (selected.isEmpty() || m_handle == nullptr || !m_handle->isOpen())
		{
			return;
		}
		const int partId = selected.first()->data(IncompletePart, PartIdRole).toInt();
		if (partId <= 0)
		{
			return;
		}

		// The real editor, not a cut-down "fill in this one field" form: the value that is missing
		// often only makes sense beside the ones that are not, and §10 autosave means there is no
		// result to read back — closing it is the commit.
		PartEditorDialog dialog(m_handle, partId, this);
		dialog.exec();
		refreshIncompleteParts();
	}

	void CategoryReconcileDialog::updateProgress()
	{
		int settled = 0;
		const int unmatched = m_ui->unmatchedTree->topLevelItemCount();
		for (int i = 0; i < unmatched; ++i)
		{
			if (m_ui->unmatchedTree->topLevelItem(i)->data(UnmatchedCategory, SettledRole).toBool())
			{
				++settled;
			}
		}
		m_ui->unmatchedStatusLabel->setText(unmatched == 0
			? tr("Nothing was left unmatched.")
			: tr("%1 of %2 looked at").arg(settled).arg(unmatched));

		// Measured against the count this dialog opened with, so filling one part always moves the
		// bar forward — and a part that goes incomplete for an unrelated reason meanwhile does not
		// move it back. A run that started with nothing to do is shown as done, not as empty.
		m_ui->incompleteProgress->setMaximum(std::max(m_initialIncompleteCount, 1));
		m_ui->incompleteProgress->setValue(m_initialIncompleteCount == 0
			? 1
			: std::max(0, m_initialIncompleteCount - m_incompleteCount));
		m_ui->incompleteStatusLabel->setText(m_incompleteCount == 0
			? tr("Nothing is missing a required value.")
			: tr("%n part(s) still missing a value", "", m_incompleteCount));
	}

}
