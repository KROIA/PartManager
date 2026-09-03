// @file PartManager_AppStartup.h
// @brief The display setup every PartManager GUI process has to perform, in one place.
//
// Shared because there are two of them: the app itself and the GUI test binary.
// They have to agree — a GUI test that renders at a different scale and a
// different font size than the shipped app cannot see a layout that breaks on
// the user's actual screen, which is most of the point of having one.
//
// Split in two because Qt demands it: the attributes must be set *before* a
// QApplication exists, the font repair only makes sense *after* one does.
// @see docs/design/ARCHITECTURE.md §7
// @see examples/PartManagerApp/src/main.cpp, unittests/ExampleTest/main.cpp
#pragma once

#include <string>

class QApplication;

namespace PartManager
{

	// High-DPI attributes. Must be called before any QApplication is constructed — Qt reads
	// these once, at construction, and ignores them afterwards.
	void applyHighDpiAttributes();

	// Qt 5.15's Windows font database takes the default UI font from the DEFAULT_GUI_FONT stock
	// object, whose LOGFONT height is in *unscaled* 96-DPI pixels (-11), and then converts that
	// height to points using the *scaled* system DPI. On a 250% display that is
	// 11 * 72 / 240 = 3.3pt where it should be 8.25pt — so every layout scales correctly and
	// every label comes out unreadably small. Reproduces under both scale-factor rounding
	// policies, so it is the font path and not the scaling path.
	//
	// Qt multiplies logical point sizes by the device pixel ratio again when it paints, and under
	// PassThrough rounding that ratio *is* the system DPI over 96 — the very number the bad
	// division used. Multiplying back is therefore the exact inverse, not a fudge factor.
	//
	// Guarded so a Qt build or platform that gets this right is left alone: a plausible UI font
	// is never touched. Call before the first widget exists, so every screen inherits it.
	void repairDefaultUiFont(QApplication& app);

	// Stops the wheel from editing values. Combo boxes and spin boxes handle a wheel by
	// changing their value, which inside a scrolled form means scrolling past one silently
	// edits it — and §10 autosave writes that straight to the database, so the part is changed
	// before the user has seen it happen. The keyboard and the drop-down still work.
	//
	// Installed on the application rather than on each widget because the forms are built at
	// runtime (AttributeFormWidget, the ribbon, every dialog), so there is no one place that
	// sees them all. The filter ignores the event *and* returns true: Qt's wheel propagation
	// walks to the parent unless the event was both consumed and accepted, so this is what
	// hands the scroll to the scroll area instead of eating it.
	void installScrollGuard(QApplication& app);

	// `text` with a line break every `columns` characters or so, broken between words.
	// Rich text (anything that already starts with '<') and text that already carries newlines
	// are returned untouched — someone laid those out on purpose.
	std::string wrapToolTip(const std::string& text, int columns = 60);

	// Keeps tooltips readable by breaking the long ones over several lines. Qt renders a plain
	// tooltip as one line however wide it gets, and the explanatory ones here run to a sentence
	// or three — on a wide screen that is a ribbon of text across the whole display.
	//
	// Installed on the application, filtering QEvent::ToolTipChange, rather than wrapping each
	// string at the call site: the strings are translated, so a hard-wrapped source string would
	// have to be re-wrapped by hand in every language, and half of them come out of .ui files
	// where there is no call site to wrap at.
	void installToolTipWrapper(QApplication& app);

	// §9 theme. A QPalette swap rather than a stylesheet: RibbonWidget, the item views and every
	// dialog are ordinary Qt widgets, so they all follow a palette without any of them knowing a
	// theme exists — a stylesheet would have to name each one. Safe to call at any time; the
	// change is live, which is what the Settings dialog's combo needs.
	//
	// `themeName` takes a ThemeName constant. Anything unrecognised falls through to the system
	// default rather than a half-applied palette, because the value comes from a settings file a
	// future version may have written.
	void applyTheme(QApplication& app, const std::string& themeName);

	// §8 language. Installs the QTranslator for `languageCode` ('en' | 'de'), removing whichever
	// one was installed before. Returns false when there is no .qm for that language — which is
	// the normal state until the translations are actually built, so the caller treats it as
	// "stay in English" rather than an error.
	bool applyLanguage(QApplication& app, const std::string& languageCode);

}
