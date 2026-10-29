// The menu's controls, in the game's style. All work with mouse, keyboard
// and gamepad navigation, since each is built on an ImGui item.
#pragma once

#include "ui/ui_glyphs.h"

#include <imgui.h>

namespace dq8::ui {

// Begin() plus the menu's window edge and shadow.
bool beginWindow(const char *name, bool *open, ImGuiWindowFlags flags);

// An on/off switch.
bool toggle(const char *id, bool *value);
// A row of mutually exclusive choices, filling the width it is given: equal
// segments, the words' own widths when those are uneven, a drop-down when
// they do not fit.
// `enabled` may be null; a disabled choice is shown but cannot be picked.
bool segmented(const char *id, int *value, const char *const *labels, int count,
               const bool *enabled = nullptr);

// True when the last item has keyboard or gamepad focus with the focus
// cursor showing; the custom controls draw a white ring for it.
bool navFocused();
void focusRing(ImDrawList *list, ImVec2 min, ImVec2 max, float rounding);

void sectionHeader(const char *title);
// A section header that folds its settings away; returns whether it is open.
// The state is kept for the session, `defaultOpen` the first time.
bool beginSection(const char *title, bool defaultOpen = true);
// A two-column table of settings: the label (with an optional chip such as
// "Experimental" beside it, and help underneath) on the left, the control on
// the right.
bool beginSettings(const char *id);
// `changed` puts a gold dot after the label: not the default any more.
// A page narrower than 24 em stacks each control under its label.
void settingRow(const char *label, const char *help = nullptr, const char *chipText = nullptr,
                bool changed = false);
void endSettings();
// Test aid (RG_UI_SCRIPT "rowcheck"): counts the rows laid out whose label,
// badge, dot, help and control rectangles overlap, and logs each one.
void rowCheck(bool on);
void rowCheckReport(const char *where, int *checked = nullptr, int *overlapping = nullptr);
// After a row's control: the long explanation as a tooltip, on hover and on
// keyboard or gamepad focus (the row's own help stays one short line).
void rowDetail(const char *detail);
// A small rounded label after the item before it, or on the next line when
// it does not fit beside it. Call after SameLine().
void chip(const char *text, ImU32 color);

// The settings window's page list: the selected entry carries the cursor.
// `compact` shows the icon only, for a narrow window.
bool navItem(const char *label, Icon icon, bool selected, bool compact = false);
// A menu entry that shows the triangle cursor when hovered or focused.
bool menuItem(const char *label, const char *shortcut = nullptr, bool selected = false, bool enabled = true);
// The same for an entry that opens a submenu; EndMenu() when it returns true.
bool beginSubmenu(const char *label);
// A button with an icon before its label.
bool iconButton(const char *label, Icon icon, bool primary = false);

// A small keycap, inline, for shortcut hints.
void keycap(const char *text);
void mutedText(const char *format, ...) IM_FMTARGS(1);

} // namespace dq8::ui
