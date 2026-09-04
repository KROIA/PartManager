#include "widgets/PartManager_KeywordCheckList.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>

namespace PartManager
{

	namespace
	{
		// Enough to keep a dozen short words off a scrollbar, few enough that a long one
		// ("Operationsverstaerker") still fits its column in the narrow half of a form.
		constexpr int ColumnCount = 3;
	}

	KeywordCheckList::KeywordCheckList(QWidget* parent)
		: QWidget(parent)
	{
		m_grid = new QGridLayout(this);
		m_grid->setContentsMargins(0, 0, 0, 0);
		m_grid->setHorizontalSpacing(12);
		m_grid->setVerticalSpacing(2);

		m_emptyLabel = new QLabel(this);
		m_emptyLabel->setWordWrap(true);
		// Word wrap makes a label ask for its longest *word* as a minimum width, which is enough to
		// widen the dialog around it. Nothing here needs a width of its own.
		m_emptyLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		m_emptyLabel->setEnabled(false);
		m_grid->addWidget(m_emptyLabel, 0, 0, 1, ColumnCount);
	}

	void KeywordCheckList::setEmptyText(const QString& text)
	{
		m_emptyText = text;
		m_emptyLabel->setText(text);
	}

	void KeywordCheckList::setKeywords(const QString& keywordList, const QString& excludedList)
	{
		m_loading = true;
		for (QCheckBox* box : findChildren<QCheckBox*>())
		{
			delete box;   // takes it out of the layout too
		}

		m_words.clear();
		for (const QString& word : keywordList.split(QLatin1Char('\n')))
		{
			const QString trimmed = word.trimmed();
			if (!trimmed.isEmpty() && !m_words.contains(trimmed))
			{
				m_words.append(trimmed);
			}
		}

		QStringList excluded;
		for (const QString& word : excludedList.split(QLatin1Char('\n')))
		{
			excluded.append(word.trimmed());
		}

		for (int i = 0; i < m_words.size(); ++i)
		{
			QCheckBox* box = new QCheckBox(m_words.at(i), this);   // user data, not translated
			box->setChecked(!excluded.contains(m_words.at(i), Qt::CaseInsensitive));
			connect(box, &QCheckBox::toggled, this, [this]()
				{
					if (!m_loading)
					{
						emit excludedChanged();
					}
				});
			m_grid->addWidget(box, i / ColumnCount, i % ColumnCount);
		}

		m_emptyLabel->setVisible(m_words.isEmpty());
		m_emptyLabel->setText(m_emptyText);
		m_loading = false;
	}

	QString KeywordCheckList::excludedKeywords() const
	{
		QStringList excluded;
		for (QCheckBox* box : findChildren<QCheckBox*>())
		{
			if (!box->isChecked())
			{
				excluded.append(box->text());
			}
		}
		// findChildren() gives no useful order, so the shown order is restored from the word list.
		QStringList ordered;
		for (const QString& word : m_words)
		{
			if (excluded.contains(word))
			{
				ordered.append(word);
			}
		}
		return ordered.join(QLatin1Char('\n'));
	}

}
