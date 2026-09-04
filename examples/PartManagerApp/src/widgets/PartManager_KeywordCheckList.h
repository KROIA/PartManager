// @file PartManager_KeywordCheckList.h
// @brief A tick box per inherited §7a search word — the way a type or a part drops one.
//
// Search words are inherited down the §2b type chain and never copied, so there
// is no text field to delete a word out of: the list a "Ceramic Capacitor" or a
// single resistor sees belongs to its ancestors. This widget shows that list as
// checkboxes, and unticking one records it as an exclusion. Used by both the
// type template editor (dropping a word for a whole branch) and the part editor
// (dropping it for one part), which is why it knows nothing about either.
// @see docs/design/ARCHITECTURE.md §2b, §7a
// @see PartManager_SearchEngine.h
#pragma once

#include <QStringList>
#include <QWidget>

class QGridLayout;
class QLabel;

namespace PartManager
{

	class KeywordCheckList : public QWidget
	{
		Q_OBJECT
	public:
		explicit KeywordCheckList(QWidget* parent = nullptr);

		// What to say when there is nothing to tick — the two callers point at different places
		// the words would have to come from.
		void setEmptyText(const QString& text);

		// `keywordList` is the newline-separated list to show, `excludedList` the newline-separated
		// words that start unticked. A word in `excludedList` that is not in `keywordList` is
		// dropped rather than remembered: the boxes are the whole state, and an exclusion of a word
		// nothing declares any more would sit in the database invisible and unremovable.
		void setKeywords(const QString& keywordList, const QString& excludedList);

		// The unticked words, newline-separated, in the order they are shown.
		QString excludedKeywords() const;

	signals:
		// A box was clicked. Not emitted by setKeywords().
		void excludedChanged();

	private:
		QGridLayout* m_grid = nullptr;
		QLabel* m_emptyLabel = nullptr;
		QString m_emptyText;
		QStringList m_words;
		// setKeywords() ticks boxes, and a ticked box is indistinguishable from a clicked one at
		// the signal. Nothing else in the dialogs would notice, but the part editor's autosave would
		// write the part back on every open.
		bool m_loading = false;
	};

}
