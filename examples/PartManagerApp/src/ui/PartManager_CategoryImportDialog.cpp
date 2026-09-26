#include "ui/PartManager_CategoryImportDialog.h"
#include "ui_PartManager_CategoryImportDialog.h"

#include "backup/PartManager_BackupManager.h"
#include "database/PartManager_DatabaseRegistry.h"
#include "settings/PartManager_Settings.h"
#include "ui/PartManager_CategoryReconcileDialog.h"

#include <QButtonGroup>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		enum StackPage
		{
			SourcePage = 0,
			PlanPage,
			ApplyPage
		};

		enum PlanColumn
		{
			PlanCategory = 0,
			PlanAction,
			PlanSummary,
			PlanColumnCount
		};

		// Which plan entry a row belongs to. A conflict child carries its parent's index, so the
		// row-level buttons work from whichever of the two the user happened to click.
		constexpr int EntryIndexRole = Qt::UserRole;
		// Which conflict of that entry a child row is, -1 on an entry row.
		constexpr int ConflictIndexRole = Qt::UserRole + 1;
		// The registry path behind a drop-down entry.
		constexpr int DatabasePathRole = Qt::UserRole;

		// The same family of tints PartMigrationDialog's worklist paints state with, so a row
		// means the same thing on both screens.
		const QColor AddedRowColor(0xD6, 0xEF, 0xD0);      // something new arrives
		const QColor MergedRowColor(0xDC, 0xE8, 0xF7);     // something already here is reconciled
		const QColor AttentionRowColor(0xFA, 0xE3, 0xC0);  // §11: parts go incomplete when this lands
		const QColor SkippedTextColor(0x90, 0x90, 0x90);   // absent, rather than a state of its own

		// A value the two sides disagree about can legitimately be empty on either side, and a
		// blank cell beside another blank cell reads as "no difference" when the difference is
		// exactly that one of them is empty.
		QString displayValue(const std::string& value)
		{
			return value.empty()
				? CategoryImportDialog::tr("(empty)")
				: QString::fromStdString(value);   // user data — the other database's own text
		}

		// Attribute keys are ascii identifiers the user chose, not app chrome: listed as they are.
		QString joinKeys(const std::vector<std::string>& keys)
		{
			QStringList list;
			for (const std::string& key : keys)
			{
				list.append(QString::fromStdString(key));
			}
			return list.join(QStringLiteral(", "));
		}

		// Two paths naming the same file. The registry stores whatever was registered and a file
		// dialog hands back whatever was clicked, so the two spellings of one database do not have
		// to match as strings. canonicalFilePath() is empty for a file that is not there, which is
		// why the raw comparison stays as the fallback.
		bool sameFile(const QString& left, const QString& right)
		{
			if (left.isEmpty() || right.isEmpty())
			{
				return false;
			}
			const QString leftCanonical = QFileInfo(left).canonicalFilePath();
			const QString rightCanonical = QFileInfo(right).canonicalFilePath();
			if (!leftCanonical.isEmpty() && !rightCanonical.isEmpty())
			{
				return leftCanonical == rightCanonical;
			}
			return QDir::cleanPath(left) == QDir::cleanPath(right);
		}
	}

	CategoryImportDialog::CategoryImportDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::CategoryImportDialog)
		, m_handle(handle)
	{
		m_ui->setupUi(this);

		m_ui->planTree->setColumnCount(PlanColumnCount);
		m_ui->planTree->setHeaderLabels(QStringList()
			<< tr("Category") << tr("What happens") << tr("Details"));
		m_ui->planTree->header()->setSectionResizeMode(PlanSummary, QHeaderView::Stretch);

		fillKnownDatabases();

		// The two source radios live in separate group boxes, and Qt's auto-exclusivity only reaches
		// siblings — without a group of their own, ticking "from a database" would leave "from a
		// file" ticked too, and reloadSource() would go on reading the (empty) file path. One group
		// here rather than reparenting them, because the two boxes are what makes the page readable.
		QButtonGroup* sourceGroup = new QButtonGroup(this);
		sourceGroup->addButton(m_ui->fileRadio);
		sourceGroup->addButton(m_ui->databaseRadio);

		connect(m_ui->fileRadio, &QRadioButton::toggled, this, &CategoryImportDialog::reloadSource);
		connect(m_ui->databaseRadio, &QRadioButton::toggled,
			this, &CategoryImportDialog::reloadSource);
		connect(m_ui->fileBrowseButton, &QPushButton::clicked,
			this, &CategoryImportDialog::browseForBundleFile);
		connect(m_ui->databaseBrowseButton, &QPushButton::clicked,
			this, &CategoryImportDialog::browseForDatabaseFile);
		connect(m_ui->databaseCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &CategoryImportDialog::reloadSource);

		connect(m_ui->nextButton, &QPushButton::clicked, this, &CategoryImportDialog::goNext);
		connect(m_ui->backButton, &QPushButton::clicked, this, &CategoryImportDialog::goBack);
		connect(m_ui->cancelButton, &QPushButton::clicked, this, &QDialog::reject);
		connect(m_ui->keepAllLocalButton, &QPushButton::clicked,
			this, &CategoryImportDialog::keepAllLocalOnSelectedRow);
		connect(m_ui->takeAllIncomingButton, &QPushButton::clicked,
			this, &CategoryImportDialog::takeAllIncomingOnSelectedRow);
		connect(m_ui->reconcileButton, &QPushButton::clicked,
			this, &CategoryImportDialog::openReconciliation);

		m_ui->warningsGroup->setVisible(false);
		m_ui->stack->setCurrentIndex(SourcePage);
		updateFooter();
	}

	CategoryImportDialog::~CategoryImportDialog()
	{
		delete m_ui;
	}

	bool CategoryImportDialog::applied() const
	{
		return m_applied;
	}

	const MergePlan& CategoryImportDialog::plan() const
	{
		return m_plan;
	}

	const PartTypeBundle& CategoryImportDialog::bundle() const
	{
		return m_bundle;
	}

	// ---------------------------------------------------------------------------------------
	// Page 1 — where from
	// ---------------------------------------------------------------------------------------

	void CategoryImportDialog::fillKnownDatabases()
	{
		const QString openPath = m_handle != nullptr ? toQt(m_handle->pmdbPath()) : QString();

		m_loading = true;
		m_ui->databaseCombo->clear();
		m_ui->databaseCombo->addItem(tr("(pick a database)"), QString());
		for (const RegisteredDatabase& entry : DatabaseRegistry::list())
		{
			const QString path = toQt(entry.pmdbPath);
			// §1b's list minus the database being written into. Not merely refused later: an entry
			// that cannot ever be chosen has no business being offered.
			if (sameFile(path, openPath))
			{
				continue;
			}
			// The folder name is the database's name (§1a has no stored name field) — user data.
			m_ui->databaseCombo->addItem(QFileInfo(path).absoluteDir().dirName(), path);
		}
		m_loading = false;
	}

	QString CategoryImportDialog::selectedDatabasePath() const
	{
		return m_ui->databaseCombo->currentData(DatabasePathRole).toString();
	}

	bool CategoryImportDialog::isOpenDatabase(const QString& path) const
	{
		if (m_handle == nullptr)
		{
			return false;
		}
		// Either spelling of the open database counts: its `.pmdb` entry file, or the
		// `partmanager.db` sitting beside it, which bundleFromDatabaseFile() also accepts.
		return sameFile(path, toQt(m_handle->pmdbPath()))
			|| sameFile(path, toQt(m_handle->databaseFilePath()));
	}

	void CategoryImportDialog::failSource(const QString& message)
	{
		m_bundle = PartTypeBundle();
		m_bundleLoaded = false;
		m_ui->sourceInfoLabel->clear();
		// Shown on the page rather than in a message box: the fix is on this page (another file,
		// another database), and a modal would cover the controls that hold the answer.
		m_ui->sourceErrorLabel->setText(message);
		updateFooter();
	}

	void CategoryImportDialog::browseForBundleFile()
	{
		const QString path = QFileDialog::getOpenFileName(this, tr("Open a category file"),
			QString(), tr("PartManager categories (*.pmcat)"));
		if (path.isEmpty())
		{
			return;
		}
		m_ui->fileRadio->setChecked(true);
		m_ui->fileEdit->setText(path);
		reloadSource();
	}

	void CategoryImportDialog::browseForDatabaseFile()
	{
		// `.db` alongside `.pmdb`: bundleFromDatabaseFile() takes either, and a user pointing at a
		// database folder often finds the SQLite file first.
		const QString path = QFileDialog::getOpenFileName(this, tr("Open a PartManager database"),
			QString(), tr("PartManager databases (*.pmdb *.db)"));
		if (path.isEmpty())
		{
			return;
		}

		m_loading = true;
		m_ui->databaseRadio->setChecked(true);
		int index = m_ui->databaseCombo->findData(path, DatabasePathRole);
		if (index < 0)
		{
			m_ui->databaseCombo->addItem(QFileInfo(path).fileName(), path);
			index = m_ui->databaseCombo->count() - 1;
		}
		m_ui->databaseCombo->setCurrentIndex(index);
		m_loading = false;
		reloadSource();
	}

	void CategoryImportDialog::reloadSource()
	{
		if (m_loading)
		{
			return;
		}

		m_bundle = PartTypeBundle();
		m_bundleLoaded = false;
		m_ui->sourceErrorLabel->clear();
		m_ui->sourceInfoLabel->clear();

		std::string error;
		if (m_ui->fileRadio->isChecked())
		{
			const QString path = m_ui->fileEdit->text();
			if (path.isEmpty())
			{
				updateFooter();
				return;
			}
			QFile file(path);
			if (!file.open(QIODevice::ReadOnly))
			{
				failSource(tr("Could not read %1 — %2.")
					.arg(QDir::toNativeSeparators(path), file.errorString()));
				return;
			}
			// The bundle is written UTF-8 and holds the user's own category names, so it is
			// decoded as UTF-8 rather than through the locale codec.
			const std::string text = QString::fromUtf8(file.readAll()).toStdString();
			if (!fromJson(text, m_bundle, error))
			{
				// Verbatim: core already phrases its refusals — an unknown format marker, a
				// bundle from a newer build (§1c) — in words meant for a person.
				failSource(toQt(error));
				return;
			}
		}
		else
		{
			const QString path = selectedDatabasePath();
			if (path.isEmpty())
			{
				updateFooter();
				return;
			}
			if (isOpenDatabase(path))
			{
				failSource(tr("That is the database this window is writing into. Importing a "
					"database into itself would change nothing — pick another one."));
				return;
			}
			if (!bundleFromDatabaseFile(path.toStdString(), m_bundle, error))
			{
				failSource(toQt(error));
				return;
			}
		}

		m_bundleLoaded = true;
		// The source database's name and the export timestamp are the other side's data and are
		// not translated; only the sentence around them is.
		const QString sourceName = m_bundle.sourceDatabaseName.empty()
			? tr("an unnamed database")
			: toQt(m_bundle.sourceDatabaseName);
		m_ui->sourceInfoLabel->setText(m_bundle.exportedAt.empty()
			? tr("%n category(ies) from %1.", "", static_cast<int>(m_bundle.nodes.size()))
				.arg(sourceName)
			: tr("%n category(ies) from %1, written %2.", "",
				static_cast<int>(m_bundle.nodes.size()))
				.arg(sourceName, toQt(m_bundle.exportedAt)));
		updateFooter();
	}

	// ---------------------------------------------------------------------------------------
	// Page 2 — the plan
	// ---------------------------------------------------------------------------------------

	void CategoryImportDialog::refreshPlanTree()
	{
		m_loading = true;
		m_ui->planTree->clear();
		m_actionCombos.assign(m_plan.entries.size(), nullptr);

		for (size_t index = 0; index < m_plan.entries.size(); ++index)
		{
			const TypeMergeEntry& entry = m_plan.entries[index];
			const int entryIndex = static_cast<int>(index);

			QTreeWidgetItem* item = new QTreeWidgetItem(m_ui->planTree);
			item->setData(0, EntryIndexRole, entryIndex);
			item->setData(0, ConflictIndexRole, -1);

			QComboBox* action = new QComboBox(m_ui->planTree);
			action->setObjectName(QStringLiteral("actionCombo%1").arg(entryIndex));
			action->addItem(tr("Add new"), static_cast<int>(TypeMergeAction::AddNew));
			// Merge is only on offer where there is something to merge into. Offering it on an
			// entry with no local match would be a choice applyMerge() has no way to honour.
			if (entry.localTypeId != NoParentType)
			{
				action->addItem(tr("Merge"), static_cast<int>(TypeMergeAction::MergeInto));
			}
			action->addItem(tr("Skip"), static_cast<int>(TypeMergeAction::Skip));
			action->setCurrentIndex(action->findData(static_cast<int>(entry.action)));
			m_ui->planTree->setItemWidget(item, PlanAction, action);
			m_actionCombos[index] = action;

			connect(action, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[this, action, item, entryIndex](int)
				{
					if (m_loading)
					{
						return;
					}
					// The combo *is* the entry's action — there is no apply-time translation step
					// where the two could disagree.
					m_plan.entries[static_cast<size_t>(entryIndex)].action =
						static_cast<TypeMergeAction>(action->currentData().toInt());
					writeEntryRow(item, entryIndex);
					updateFooter();
				});

			for (size_t conflictIndex = 0; conflictIndex < entry.conflicts.size(); ++conflictIndex)
			{
				const FieldConflict& conflict = entry.conflicts[conflictIndex];
				QTreeWidgetItem* child = new QTreeWidgetItem(item);
				child->setData(0, EntryIndexRole, entryIndex);
				child->setData(0, ConflictIndexRole, static_cast<int>(conflictIndex));
				// core hands over an untranslated English label plus a stable ascii key; the label
				// is what a person reads, the key is what identifies the field, so the key goes in
				// the tooltip where it can be quoted in a bug report.
				child->setText(PlanCategory, toQt(conflict.displayLabel));
				child->setToolTip(PlanCategory, toQt(conflict.fieldKey));
				child->setText(PlanSummary, tr("here: %1   ·   incoming: %2")
					.arg(displayValue(conflict.localValue), displayValue(conflict.incomingValue)));

				QComboBox* resolution = new QComboBox(m_ui->planTree);
				resolution->setObjectName(QStringLiteral("conflictCombo%1_%2")
					.arg(entryIndex).arg(static_cast<int>(conflictIndex)));
				resolution->addItem(tr("Keep local"), static_cast<int>(FieldResolution::KeepLocal));
				resolution->addItem(tr("Take incoming"),
					static_cast<int>(FieldResolution::TakeIncoming));
				resolution->setCurrentIndex(
					resolution->findData(static_cast<int>(conflict.resolution)));
				m_ui->planTree->setItemWidget(child, PlanAction, resolution);

				const int conflictSlot = static_cast<int>(conflictIndex);
				connect(resolution, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, resolution, entryIndex, conflictSlot](int)
					{
						if (m_loading)
						{
							return;
						}
						m_plan.entries[static_cast<size_t>(entryIndex)]
							.conflicts[static_cast<size_t>(conflictSlot)].resolution =
							static_cast<FieldResolution>(resolution->currentData().toInt());
					});
			}

			writeEntryRow(item, entryIndex);
		}
		m_loading = false;

		m_ui->planTree->resizeColumnToContents(PlanCategory);
		m_ui->planTree->resizeColumnToContents(PlanAction);
		m_ui->planTree->header()->setSectionResizeMode(PlanSummary, QHeaderView::Stretch);

		// Verbatim again, one per line: core writes these for a person — ambiguous names, datatype
		// changes, re-parents — and none of them stops the merge, so they belong next to it rather
		// than in front of it.
		QStringList warnings;
		for (const std::string& warning : m_plan.warnings)
		{
			warnings.append(toQt(warning));
		}
		m_ui->warningsGroup->setVisible(!warnings.isEmpty());
		m_ui->warningsEdit->setPlainText(warnings.join(QStringLiteral("\n")));

		int added = 0;
		int merged = 0;
		int skipped = 0;
		int affected = 0;
		planCounts(added, merged, skipped, affected);
		m_ui->planSummaryLabel->setText(tr("%1 new, %2 merged, %3 skipped")
			.arg(added).arg(merged).arg(skipped));
	}

	void CategoryImportDialog::writeEntryRow(QTreeWidgetItem* item, int entryIndex)
	{
		const TypeMergeEntry& entry = m_plan.entries[static_cast<size_t>(entryIndex)];

		// The incoming path is the other database's own category naming — user data, never tr()'d.
		// Joined for the row only: core keeps it as segments so a category whose own name holds a
		// separator stays one category, and the row identity is EntryIndexRole, not this text.
		item->setText(PlanCategory, toQt(displayPartTypePath(entry.path)));

		QStringList details;
		switch (entry.action)
		{
			case TypeMergeAction::AddNew:
				details.append(tr("Not in this database yet — it will be created."));
				break;
			case TypeMergeAction::MergeInto:
				if (!entry.conflicts.empty())
				{
					details.append(tr("%n field(s) differ", "",
						static_cast<int>(entry.conflicts.size())));
				}
				if (!entry.addedAttributeKeys.empty())
				{
					details.append(tr("%n field(s) added: %1", "",
						static_cast<int>(entry.addedAttributeKeys.size()))
						.arg(joinKeys(entry.addedAttributeKeys)));
				}
				if (details.isEmpty())
				{
					details.append(tr("Already the same here — nothing to change."));
				}
				break;
			case TypeMergeAction::Skip:
				details.append(tr("Left exactly as it is."));
				break;
		}

		// §11, on the row and not in a tooltip: this is the consequence that outlives the import.
		// Only while the entry is actually going to be applied — a skipped row makes nobody
		// incomplete, and saying it would is a false alarm.
		const bool willApply = entry.action != TypeMergeAction::Skip;
		const bool makesIncomplete = willApply && !entry.newRequiredAttributeKeys.empty();
		if (makesIncomplete)
		{
			details.append(tr("%n part(s) here will be missing a now-required value (%1).", "",
				entry.affectedPartCount).arg(joinKeys(entry.newRequiredAttributeKeys)));
		}
		item->setText(PlanSummary, details.join(QStringLiteral("  ·  ")));

		for (int column = 0; column < PlanColumnCount; ++column)
		{
			// Cleared first: a row repainted after the user changed its action must not keep the
			// colour of the action it no longer has.
			item->setBackground(column, QBrush());
			item->setForeground(column, QBrush());
			if (makesIncomplete)
			{
				item->setBackground(column, AttentionRowColor);
			}
			else if (entry.action == TypeMergeAction::AddNew)
			{
				item->setBackground(column, AddedRowColor);
			}
			else if (entry.action == TypeMergeAction::MergeInto)
			{
				item->setBackground(column, MergedRowColor);
			}
			else
			{
				item->setForeground(column, SkippedTextColor);
			}
		}
	}

	int CategoryImportDialog::selectedEntryIndex() const
	{
		const QTreeWidgetItem* item = m_ui->planTree->currentItem();
		if (item == nullptr)
		{
			return -1;
		}
		const int index = item->data(0, EntryIndexRole).toInt();
		return index >= 0 && index < static_cast<int>(m_plan.entries.size()) ? index : -1;
	}

	void CategoryImportDialog::resolveWholeEntry(int entryIndex, FieldResolution resolution)
	{
		if (entryIndex < 0)
		{
			return;
		}
		TypeMergeEntry& entry = m_plan.entries[static_cast<size_t>(entryIndex)];
		m_loading = true;
		for (size_t conflictIndex = 0; conflictIndex < entry.conflicts.size(); ++conflictIndex)
		{
			entry.conflicts[conflictIndex].resolution = resolution;
			QComboBox* combo = findChild<QComboBox*>(QStringLiteral("conflictCombo%1_%2")
				.arg(entryIndex).arg(static_cast<int>(conflictIndex)));
			if (combo != nullptr)
			{
				combo->setCurrentIndex(combo->findData(static_cast<int>(resolution)));
			}
		}
		m_loading = false;
	}

	void CategoryImportDialog::keepAllLocalOnSelectedRow()
	{
		resolveWholeEntry(selectedEntryIndex(), FieldResolution::KeepLocal);
	}

	void CategoryImportDialog::takeAllIncomingOnSelectedRow()
	{
		resolveWholeEntry(selectedEntryIndex(), FieldResolution::TakeIncoming);
	}

	void CategoryImportDialog::planCounts(int& outAdded, int& outMerged, int& outSkipped,
		int& outAffected) const
	{
		outAdded = 0;
		outMerged = 0;
		outSkipped = 0;
		outAffected = 0;
		for (const TypeMergeEntry& entry : m_plan.entries)
		{
			switch (entry.action)
			{
				case TypeMergeAction::AddNew:    ++outAdded; break;
				case TypeMergeAction::MergeInto: ++outMerged; break;
				case TypeMergeAction::Skip:      ++outSkipped; break;
			}
			if (entry.action != TypeMergeAction::Skip && !entry.newRequiredAttributeKeys.empty())
			{
				outAffected += entry.affectedPartCount;
			}
		}
	}

	// ---------------------------------------------------------------------------------------
	// Page 3 — confirm and apply
	// ---------------------------------------------------------------------------------------

	void CategoryImportDialog::applyPlan()
	{
		if (m_handle == nullptr || !m_handle->isOpen())
		{
			return;
		}

		// §9a FIRST, and not behind a tick box. applyMerge() is transactional, so a *failed*
		// import needs no snapshot — this one is for the import that succeeds and is then
		// regretted, which is the case no transaction can help with.
		const AppPreferences preferences = Settings::getPreferences();
		std::string backupError;
		const std::string snapshot = BackupManager::createSnapshot(m_handle->databaseFilePath(),
			preferences.backupFolder, preferences.backupRetentionCount, &backupError);
		if (snapshot.empty())
		{
			// Not silently skipped, and not silently fatal either: the user is the only one who
			// can decide whether an import without a way back is worth it, so they are asked, with
			// the reason the snapshot failed in front of them.
			m_ui->backupLabel->setText(tr("No snapshot could be taken — %1.")
				.arg(toQt(backupError)));
			if (QMessageBox::question(this, tr("No backup was made"),
				tr("The safety snapshot could not be written (%1). The import itself is still "
				   "all-or-nothing, but there would be no snapshot to fall back to afterwards.\n\n"
				   "Import anyway?").arg(toQt(backupError)),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			{
				return;
			}
		}
		else
		{
			m_ui->backupLabel->setText(tr("A snapshot of this database was saved as %1 before "
				"anything was changed. Settings ▸ Backups can put it back.")
				.arg(QDir::toNativeSeparators(toQt(snapshot))));
		}

		std::string error;
		if (!applyMerge(m_handle->connection(), m_bundle, m_plan, error))
		{
			// Said plainly, because the one thing a user cannot find out for themselves is
			// whether half of it landed.
			m_ui->applyResultLabel->setText(tr("The import failed and nothing was changed — the "
				"whole thing was rolled back.\n\n%1").arg(toQt(error)));
			updateFooter();
			return;
		}

		m_applied = true;
		int added = 0;
		int merged = 0;
		int skipped = 0;
		int affected = 0;
		planCounts(added, merged, skipped, affected);
		m_ui->applyResultLabel->setText(tr("Done — %1 categories added, %2 merged.")
			.arg(added).arg(merged));
		m_ui->reconcileButton->setEnabled(true);
		updateFooter();
	}

	void CategoryImportDialog::openReconciliation()
	{
		if (!m_applied || m_handle == nullptr)
		{
			return;
		}
		CategoryReconcileDialog dialog(m_handle, m_plan.unmatchedLocalTypeIds, this);
		dialog.exec();
	}

	// ---------------------------------------------------------------------------------------
	// Navigation
	// ---------------------------------------------------------------------------------------

	void CategoryImportDialog::goNext()
	{
		switch (m_ui->stack->currentIndex())
		{
			case SourcePage:
			{
				if (!m_bundleLoaded || m_handle == nullptr || !m_handle->isOpen())
				{
					return;
				}
				// Pure: planMerge() reads and plans, it writes nothing. Pressing Next here still
				// leaves the database exactly as it was.
				m_plan = planMerge(m_handle->connection(), m_bundle);
				m_planLoaded = true;
				refreshPlanTree();
				m_ui->stack->setCurrentIndex(PlanPage);
				break;
			}
			case PlanPage:
			{
				int added = 0;
				int merged = 0;
				int skipped = 0;
				int affected = 0;
				planCounts(added, merged, skipped, affected);
				m_ui->applySummaryLabel->setText(tr("%1 categories will be added, %2 merged and "
					"%3 left alone. %n part(s) will be left missing a value that has become "
					"required — they stay exactly as they are and can be filled in afterwards.",
					"", affected).arg(added).arg(merged).arg(skipped));
				m_ui->backupLabel->setText(tr("A snapshot of this database is taken before "
					"anything is written, so this import can be undone by restoring it."));
				m_ui->applyResultLabel->clear();
				m_ui->stack->setCurrentIndex(ApplyPage);
				break;
			}
			case ApplyPage:
			default:
				if (m_applied)
				{
					accept();
					return;
				}
				applyPlan();
				break;
		}
		updateFooter();
	}

	void CategoryImportDialog::goBack()
	{
		if (m_applied)
		{
			return;   // there is no "before" to go back to any more
		}
		switch (m_ui->stack->currentIndex())
		{
			case PlanPage:
				// The plan is made from the bundle, so returning to the source page — where the
				// bundle may be swapped — has to drop it rather than leave a plan describing a
				// file that is no longer selected.
				m_plan = MergePlan();
				m_planLoaded = false;
				m_actionCombos.clear();
				m_ui->planTree->clear();
				m_ui->stack->setCurrentIndex(SourcePage);
				break;
			case ApplyPage:
				m_ui->stack->setCurrentIndex(PlanPage);
				break;
			default:
				break;
		}
		updateFooter();
	}

	void CategoryImportDialog::updateFooter()
	{
		const int page = m_ui->stack->currentIndex();
		m_ui->backButton->setVisible(page != SourcePage && !m_applied);
		m_ui->cancelButton->setVisible(!m_applied);

		if (page == SourcePage)
		{
			m_ui->nextButton->setText(tr("Next"));
			m_ui->nextButton->setEnabled(m_bundleLoaded);
		}
		else if (page == PlanPage)
		{
			m_ui->nextButton->setText(tr("Next"));
			m_ui->nextButton->setEnabled(true);
		}
		else
		{
			m_ui->nextButton->setText(m_applied ? tr("Close") : tr("Apply"));
			m_ui->nextButton->setEnabled(true);
		}
	}

	void CategoryImportDialog::done(int result)
	{
		// §10: nothing is persisted before Apply, so what a mid-wizard close loses is the
		// reviewing — which is the expensive half. Asked here rather than on Cancel, so the
		// window's ✕ and Esc ask it too. Once applied there is nothing left to lose, and a
		// question would be pure noise.
		if (m_planLoaded && !m_applied)
		{
			if (QMessageBox::question(this, tr("Close without importing?"),
				tr("Nothing has been written to this database yet, so closing now changes "
				   "nothing — but the decisions you made on the plan are not saved anywhere "
				   "and would have to be made again.\n\nClose anyway?"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			{
				return;
			}
		}
		QDialog::done(result);
	}

}
