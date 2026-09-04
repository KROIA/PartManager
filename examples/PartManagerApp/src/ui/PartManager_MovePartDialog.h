// @file PartManager_MovePartDialog.h
// @brief What a part loses and still owes when it is dragged into another category (§2b, §7a).
//
// Opened by dropping a row of the part table onto a category in the tree, and
// only when the move is not a clean one: the two types' effective attribute sets
// are compared first, and a part whose values all carry over and whose new
// required fields are all filled moves without a dialog at all.
//
// Nothing here touches the part's identity, its files, its tags, its stock log,
// its partlist lines or its orders — every one of those hangs off the part id,
// which a move does not change. What changes is `part_type_id` and, with it,
// which attributes the part is allowed to have.
// @see docs/design/ARCHITECTURE.md §2, §2b, §11
// @see PartManager_PartEditorController.h, PartManager_AttributeFormWidget.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include <QDialog>

class QLabel;
class QPushButton;

namespace PartManager
{

	class AttributeFormWidget;

	class MovePartDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `part` is the row being moved, `targetType` the category it was dropped on. The dialog
		// reads both types' effective attributes through the controller.
		MovePartDialog(PartEditorController& controller, const Part& part,
			const PartType& targetType, QWidget* parent = nullptr);

		// True when the two types' attributes need no decision from the user: nothing the part
		// holds would be dropped, and the target asks for nothing that is not already filled in.
		// Static so the caller can skip building the dialog at all in that case.
		static bool isCleanMove(PartEditorController& controller, const Part& part, int targetTypeId);

		// The part with its new type and its rewritten attributes — valid after exec() == Accepted.
		const Part& movedPart() const { return m_part; }

	private:
		// Greys out Apply while a required attribute of the new category is still empty, and says
		// which. The same §11 rule that blocks Create: a part is not allowed to arrive in a
		// category already incomplete.
		void updateApplyState();

		Part m_part;
		std::vector<PartTypeAttribute> m_targetAttributes;
		AttributeFormWidget* m_form;
		QLabel* m_missingLabel;
		QPushButton* m_applyButton;
	};

}
