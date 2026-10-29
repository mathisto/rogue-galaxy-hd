#include "ui/ui_widgets.h"

#include "ui/ui_style.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>

namespace dq8::ui {
namespace {
ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

// Eases a per-item value toward its target, for small animations.
float animate(ImGuiID id, float target, float speed = 14.0f) {
    float &value = *ImGui::GetStateStorage()->GetFloatRef(id, target);
    value += (target - value) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
    if (std::fabs(target - value) < 0.002f)
        value = target;
    return value;
}

void centeredText(ImDrawList *list, ImVec2 min, ImVec2 max, ImU32 color, const char *text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    list->AddText(ImVec2(std::floor((min.x + max.x - size.x) * 0.5f), std::floor((min.y + max.y - size.y) * 0.5f)),
                  color, text);
}
// Set by the controls that draw their own focus ring, for rowDetail().
bool g_controlFocused = false;
bool g_stacked = false;
// Where a settings table ends (screen x): the help under a label runs to it.
float g_tableRight = 0.0f;
} // namespace

bool navFocused() { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

void focusRing(ImDrawList *list, ImVec2 min, ImVec2 max, float rounding) {
    // White, so it reads against the gold of a chosen segment or a switched-on toggle.
    list->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), palette::kText,
                  rounding + 1.0f, 2.0f);
}

bool beginWindow(const char *name, bool *open, ImGuiWindowFlags flags) {
    const bool visible = ImGui::Begin(name, open, flags);
    if (visible)
        decorateWindow();
    return visible;
}

bool toggle(const char *id, bool *value) {
    const float frame = ImGui::GetFrameHeight();
    const float height = std::floor(frame * 0.8f);
    const float width = std::floor(height * 1.9f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // InvisibleButton leaves keyboard and gamepad navigation out unless asked.
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, frame), ImGuiButtonFlags_EnableNav);
    if (pressed)
        *value = !*value;
    const float t = animate(ImGui::GetItemID(), *value ? 1.0f : 0.0f);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList *list = ImGui::GetWindowDrawList();
    const ImVec2 min(pos.x, pos.y + (frame - height) * 0.5f);
    const ImVec2 max(min.x + width, min.y + height);
    const ImU32 off = IM_COL32(255, 255, 255, hovered ? 48 : 34);
    list->AddRectFilled(min, max, mix(off, withAlpha(palette::kGold, 0.92f), t), height * 0.5f);
    list->AddRect(min, max, IM_COL32(255, 255, 255, 70), height * 0.5f, 1.0f);
    const float radius = height * 0.5f - 3.0f;
    if (navFocused()) {
        g_controlFocused = true;
        focusRing(list, ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), height * 0.5f);
    }
    const ImVec2 knob(min.x + height * 0.5f + (width - height) * t, min.y + height * 0.5f);
    list->AddCircleFilled(ImVec2(knob.x, knob.y + 1.0f), radius, IM_COL32(0, 0, 10, 90));
    list->AddCircleFilled(knob, radius, mix(IM_COL32(205, 210, 230, 255), IM_COL32(255, 255, 255, 255), t));
    return pressed;
}

bool segmented(const char *id, int *value, const char *const *labels, int count, const bool *enabled) {
    ImGui::PushID(id);
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();
    const float padding = em(1.1f);
    // Each choice needs its word and some air: equal segments when the
    // longest fits in one, the words' own widths when not, and a drop-down
    // when even those do not fit, so no word is ever cut.
    float widest = 0.0f, total = 0.0f;
    for (int index = 0; index < count; ++index) {
        const float need = ImGui::CalcTextSize(labels[index]).x + padding;
        widest = std::max(widest, need);
        total += need;
    }
    const bool equal = widest * static_cast<float>(count) <= width;
    if (!equal && total > width) {
        ImGui::SetNextItemWidth(width);
        bool changed = false;
        if (ImGui::BeginCombo("##choice", labels[std::clamp(*value, 0, count - 1)])) {
            for (int index = 0; index < count; ++index) {
                const bool usable = !enabled || enabled[index];
                ImGui::BeginDisabled(!usable);
                if (ImGui::Selectable(labels[index], *value == index) && *value != index) {
                    *value = index;
                    changed = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        g_controlFocused = navFocused();
        ImGui::PopID();
        return changed;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float rounding = ImGui::GetStyle().FrameRounding;
    ImDrawList *list = ImGui::GetWindowDrawList();
    list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(255, 255, 255, 16), rounding);

    const float extra = equal ? 0.0f : (width - total) / static_cast<float>(count);
    float left = 0.0f;
    bool changed = false;
    for (int index = 0; index < count; ++index) {
        const float segment = equal ? width / static_cast<float>(count)
                                    : ImGui::CalcTextSize(labels[index]).x + padding + extra;
        const bool chosen = *value == index;
        const bool usable = !enabled || enabled[index];
        const ImVec2 at(pos.x + left, pos.y);
        left += segment;
        ImGui::SetCursorScreenPos(at);
        ImGui::PushID(index);
        ImGui::BeginDisabled(!usable);
        if (ImGui::InvisibleButton("##choice", ImVec2(segment, height), ImGuiButtonFlags_EnableNav) && !chosen) {
            *value = index;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = navFocused();
        ImGui::EndDisabled();
        ImGui::PopID();

        const ImVec2 min(at.x + 2.0f, at.y + 2.0f), max(at.x + segment - 2.0f, at.y + height - 2.0f);
        if (chosen) {
            list->AddRectFilled(min, max, IM_COL32(244, 203, 82, 50), rounding - 1.0f);
            list->AddRect(min, max, palette::kGold, rounding - 1.0f, 1.5f);
        } else if (hovered && usable) {
            list->AddRectFilled(min, max, palette::kHover, rounding - 1.0f);
        }
        if (focused) {
            g_controlFocused = true;
            focusRing(list, min, max, rounding - 1.0f);
        }
        const ImU32 color = chosen ? palette::kGold : usable ? palette::kText : withAlpha(palette::kTextMuted, 0.6f);
        list->PushClipRect(min, max, true);
        centeredText(list, min, max, color, labels[index]);
        list->PopClipRect();
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
    return changed;
}

void sectionHeader(const char *title) {
    std::string upper(title);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char letter) { return static_cast<char>(std::toupper(letter)); });
    ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    pushScaledFont(0.82f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette::kGold));
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float y = std::floor((min.y + max.y) * 0.5f) + 0.5f;
    if (max.x + em(0.6f) < right)
        ImGui::GetWindowDrawList()->AddLine(ImVec2(max.x + em(0.6f), y), ImVec2(right, y),
                                            withAlpha(palette::kGold, 0.28f), 1.0f);
}

bool beginSection(const char *title, bool defaultOpen) {
    ImGui::PushID(title);
    bool &open = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("##open"), defaultOpen);
    const float height = em(1.9f);
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0.0f, em(0.15f)));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##section", ImVec2(width, height), ImGuiButtonFlags_EnableNav))
        open = !open;
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = navFocused();
    ImDrawList *list = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (hovered)
        list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), palette::kHover, rounding);
    if (focused)
        focusRing(list, pos, ImVec2(pos.x + width, pos.y + height), rounding);
    const float mid = pos.y + height * 0.5f;
    const float half = em(0.28f);
    const float cx = pos.x + em(0.8f);
    if (open)
        list->AddTriangleFilled(ImVec2(cx - half, mid - half * 0.6f), ImVec2(cx + half, mid - half * 0.6f),
                                ImVec2(cx, mid + half * 0.7f), palette::kGold);
    else
        list->AddTriangleFilled(ImVec2(cx - half * 0.6f, mid - half), ImVec2(cx - half * 0.6f, mid + half),
                                ImVec2(cx + half * 0.7f, mid), palette::kGold);
    std::string upper(title);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char letter) { return static_cast<char>(std::toupper(letter)); });
    pushScaledFont(0.82f);
    const ImVec2 text = ImGui::CalcTextSize(upper.c_str());
    const float textX = pos.x + em(1.6f);
    list->AddText(ImVec2(textX, std::floor(mid - text.y * 0.5f)), palette::kGold, upper.c_str());
    ImGui::PopFont();
    const float lineX = textX + text.x + em(0.6f);
    if (lineX < pos.x + width)
        list->AddLine(ImVec2(lineX, std::floor(mid) + 0.5f), ImVec2(pos.x + width, std::floor(mid) + 0.5f),
                      withAlpha(palette::kGold, 0.28f), 1.0f);
    ImGui::PopID();
    return open;
}

bool beginSettings(const char *id) {
    const float available = ImGui::GetContentRegionAvail().x;
    // Narrow pages put each control under its label instead of beside it.
    g_stacked = available < em(24.0f);
    g_tableRight = ImGui::GetCursorScreenPos().x + available;
    // NoClip: the help under a label may run on under the control's column.
    if (!ImGui::BeginTable(id, g_stacked ? 1 : 2, ImGuiTableFlags_NoClip))
        return false;
    if (!g_stacked) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                                std::clamp(available * 0.42f, em(9.0f), em(17.0f)));
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    }
    return true;
}

// The row check (RG_UI_SCRIPT "rowcheck"): each settings row's label, badge,
// dot, help and control rectangles are compared as the row is laid out, so a
// layout that lets any of them overlap is counted and logged.
namespace {
struct Box {
    ImVec2 min, max;
    bool valid = false;
};
struct RowBoxes {
    bool open = false;
    std::string name;
    Box label, badge, dot, help;
    ImVec2 controlStart;
};
RowBoxes g_row;
bool g_rowCheck = false;
// Each row counts once, however many frames draw it.
std::set<std::string> g_rowsSeen, g_rowsBad;

Box lastItemBox() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true}; }

bool intersects(const Box &a, const Box &b) {
    return a.valid && b.valid && a.min.x < b.max.x - 0.5f && b.min.x < a.max.x - 0.5f &&
           a.min.y < b.max.y - 0.5f && b.min.y < a.max.y - 0.5f;
}

// Called once the row's control has been submitted (its last item is the
// last thing the row drew): compares the row's parts.
void finishRow() {
    if (!g_row.open)
        return;
    g_row.open = false;
    if (!g_rowCheck)
        return;
    Box control;
    control.valid = !g_stacked;
    control.min = g_row.controlStart;
    const ImVec2 last = ImGui::GetItemRectMax();
    control.max = ImVec2(std::max(last.x, control.min.x + 1.0f), std::max(last.y, control.min.y + 1.0f));
    struct Part {
        const char *name;
        const Box *box;
    };
    const Part parts[] = {{"label", &g_row.label}, {"badge", &g_row.badge}, {"dot", &g_row.dot},
                          {"help", &g_row.help},   {"control", &control}};
    g_rowsSeen.insert(g_row.name);
    bool bad = false;
    for (int first = 0; first < 5; ++first)
        for (int second = first + 1; second < 5; ++second)
            if (intersects(*parts[first].box, *parts[second].box)) {
                bad = true;
                if (!g_rowsBad.count(g_row.name))
                    std::fprintf(stderr, "[rowcheck] OVERLAP in \"%s\": %s and %s\n", g_row.name.c_str(),
                             parts[first].name, parts[second].name);
            }
    if (bad)
        g_rowsBad.insert(g_row.name);
}
} // namespace

void rowCheck(bool on) {
    g_rowCheck = on;
    g_rowsSeen.clear();
    g_rowsBad.clear();
}

void rowCheckReport(const char *where, int *checked, int *overlapping) {
    const int rows = static_cast<int>(g_rowsSeen.size()), bad = static_cast<int>(g_rowsBad.size());
    std::fprintf(stderr, "[rowcheck] %s: %d rows checked, %d overlapping\n", where, rows, bad);
    if (checked)
        *checked = rows;
    if (overlapping)
        *overlapping = bad;
    g_rowsSeen.clear();
    g_rowsBad.clear();
}

void settingRow(const char *label, const char *help, const char *chipText, bool changed) {
    finishRow();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, g_stacked ? 0.0f : ImGui::GetFrameHeight() + em(0.35f));
    ImGui::TableNextColumn();
    g_row = RowBoxes{};
    g_row.open = true;
    g_row.name = label;

    // The label's cell: a gutter on the left that holds the "changed" dot
    // (always reserved, so labels line up), then the label, which wraps at
    // the cell's right edge less a gap, then the badge beside it when it
    // fits and under it when not, then the help on its own wrapped line.
    const float gutter = em(1.0f);
    const float cellRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float wrapRight = cellRight - (g_stacked ? 0.0f : em(0.6f));
    const float windowX = ImGui::GetWindowPos().x;
    ImGui::Indent(gutter);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(std::max(wrapRight, ImGui::GetCursorScreenPos().x + em(4.0f)) - windowX);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    g_row.label = lastItemBox();
    if (changed) {
        const float radius = em(0.17f);
        const ImVec2 centre(g_row.label.min.x - gutter * 0.5f, g_row.label.min.y + ImGui::GetTextLineHeight() * 0.5f);
        ImGui::GetWindowDrawList()->AddCircleFilled(centre, radius, palette::kGold);
        g_row.dot = {ImVec2(centre.x - radius, centre.y - radius), ImVec2(centre.x + radius, centre.y + radius), true};
    }
    if (chipText) {
        pushScaledFont(0.72f);
        const float chipWidth = std::floor(ImGui::CalcTextSize(chipText).x + em(1.2f));
        ImGui::PopFont();
        const float gap = em(0.5f);
        if (g_row.label.max.x + gap + chipWidth <= wrapRight)
            ImGui::SameLine(0.0f, gap);
        chip(chipText, palette::kWarn);
        g_row.badge = lastItemBox();
    }
    if (help) {
        pushScaledFont(0.84f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette::kTextMuted));
        // Always on a line of its own, wrapped at the table's right edge.
        ImGui::PushTextWrapPos(g_tableRight - windowX);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        g_row.help = lastItemBox();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::Unindent(gutter);
    if (!g_stacked)
        ImGui::TableNextColumn();
    g_row.controlStart = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void endSettings() {
    finishRow();
    ImGui::EndTable();
}

void rowDetail(const char *detail) {
    const bool focused = g_controlFocused || navFocused();
    g_controlFocused = false;
    if (!detail || !*detail)
        return;
    if (!(focused || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)))
        return;
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(em(26.0f));
        ImGui::TextUnformatted(detail);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool navItem(const char *label, Icon kind, bool selected, bool compact) {
    const float height = em(compact ? 2.0f : 2.3f);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##page", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = navFocused();
    const float y = pos.y + height * 0.5f;

    ImDrawList *list = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + width, pos.y + height);
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (selected)
        list->AddRectFilled(pos, max, IM_COL32(255, 255, 255, 20), rounding);
    else if (hovered)
        list->AddRectFilled(pos, max, palette::kHover, rounding);
    if (focused)
        focusRing(list, pos, max, rounding);
    const ImU32 color = selected || hovered || focused ? palette::kText : palette::kTextMuted;
    if (compact) {
        // Icons only; the name shows on hover and focus.
        drawIcon(list, kind, ImVec2(pos.x + width * 0.5f, y), em(1.2f), selected ? palette::kGold : color);
        if (selected)
            list->AddRectFilled(ImVec2(pos.x, pos.y + em(0.3f)), ImVec2(pos.x + 3.0f, max.y - em(0.3f)),
                                palette::kGold, 2.0f);
        if (hovered || focused)
            ImGui::SetTooltip("%s", label);
        return pressed;
    }
    if (selected)
        drawCursor(list, ImVec2(pos.x + em(1.05f), y), em(0.62f));
    drawIcon(list, kind, ImVec2(pos.x + em(2.05f), y), em(1.1f), selected ? palette::kGold : color);
    const ImVec2 text = ImGui::CalcTextSize(label);
    list->PushClipRect(pos, max, true);
    list->AddText(ImVec2(pos.x + em(3.0f), std::floor(y - text.y * 0.5f)), color, label);
    list->PopClipRect();
    return pressed;
}

namespace {
// The cursor beside the item just submitted, in the menu window `list` draws to.
// Menu entries reach half the item spacing left of their text; the cursor
// goes by the text, clear of the window's edge.
void menuCursor(ImDrawList *list, ImVec2 windowMin, ImVec2 windowSize) {
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float text = min.x + std::floor(ImGui::GetStyle().ItemSpacing.x * 0.5f);
    list->PushClipRect(windowMin, ImVec2(windowMin.x + windowSize.x, windowMin.y + windowSize.y), false);
    drawCursor(list, ImVec2(text - em(0.4f), (min.y + max.y) * 0.5f), em(0.55f));
    list->PopClipRect();
}
} // namespace

bool menuItem(const char *label, const char *shortcut, bool selected, bool enabled) {
    const bool pressed = ImGui::MenuItem(label, shortcut, selected, enabled);
    if (enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused()))
        menuCursor(ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), ImGui::GetWindowSize());
    return pressed;
}

bool beginSubmenu(const char *label) {
    // Taken first: once the submenu is open, it is the current window.
    ImDrawList *list = ImGui::GetWindowDrawList();
    const ImVec2 windowMin = ImGui::GetWindowPos(), windowSize = ImGui::GetWindowSize();
    const bool open = ImGui::BeginMenu(label);
    if (open || ImGui::IsItemHovered() || ImGui::IsItemFocused())
        menuCursor(list, windowMin, windowSize);
    return open;
}

bool iconButton(const char *label, Icon kind, bool primary) {
    const float iconSize = em(0.95f);
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    const ImVec2 text = ImGui::CalcTextSize(label);
    const ImVec2 size(padding.x * 2.0f + iconSize + em(0.45f) + text.x, ImGui::GetFrameHeight());
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.96f, 0.80f, 0.32f, 0.88f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.86f, 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.90f, 0.55f, 1.0f));
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::Button("##button", size);
    ImGui::PopID();
    if (primary)
        ImGui::PopStyleColor(3);
    const ImU32 color = primary ? IM_COL32(16, 20, 44, 255) : palette::kText;
    ImDrawList *list = ImGui::GetWindowDrawList();
    drawIcon(list, kind, ImVec2(pos.x + padding.x + iconSize * 0.5f, pos.y + size.y * 0.5f), iconSize, color);
    list->AddText(ImVec2(pos.x + padding.x + iconSize + em(0.45f), std::floor(pos.y + (size.y - text.y) * 0.5f)),
                  color, label);
    return pressed;
}

void keycap(const char *text) {
    // Laid out one frame tall, so it lines up with framed text beside it.
    const float frame = ImGui::GetFrameHeight();
    pushScaledFont(0.8f);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float height = std::floor(frame * 0.78f);
    const float width = std::max(height, textSize.x + em(0.8f));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, frame));
    const ImVec2 min(at.x, std::floor(at.y + (frame - height) * 0.5f));
    const ImVec2 max(min.x + width, min.y + height);
    ImDrawList *list = ImGui::GetWindowDrawList();
    const float rounding = height * 0.24f;
    list->AddRectFilled(ImVec2(min.x, min.y + 2.0f), ImVec2(max.x, max.y + 1.0f), IM_COL32(0, 0, 10, 120), rounding);
    list->AddRectFilled(min, max, IM_COL32(255, 255, 255, 34), rounding);
    list->AddRect(min, max, IM_COL32(255, 255, 255, 84), rounding, 1.0f);
    centeredText(list, min, max, palette::kText, text);
    ImGui::PopFont();
}

void chip(const char *text, ImU32 color) {
    // One frame tall, as keycap is, to line up with framed text beside it.
    const float frame = ImGui::GetFrameHeight();
    pushScaledFont(0.72f);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float height = std::floor(textSize.y + em(0.5f));
    const float width = std::floor(textSize.x + em(1.2f));
    if (ImGui::GetContentRegionAvail().x < width)
        ImGui::NewLine();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, frame));
    const ImVec2 min(at.x, std::floor(at.y + (frame - height) * 0.5f));
    const ImVec2 max(min.x + width, min.y + height);
    ImDrawList *list = ImGui::GetWindowDrawList();
    list->AddRectFilled(min, max, withAlpha(color, 0.14f), height * 0.5f);
    list->AddRect(min, max, withAlpha(color, 0.75f), height * 0.5f, 1.0f);
    centeredText(list, min, max, color, text);
    ImGui::PopFont();
}

void mutedText(const char *format, ...) {
    va_list args;
    va_start(args, format);
    ImGui::TextDisabledV(format, args);
    va_end(args);
}

} // namespace dq8::ui
