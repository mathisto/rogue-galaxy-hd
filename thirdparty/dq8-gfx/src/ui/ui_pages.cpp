// The settings window's pages and the menu's other windows.
#include "ui/ui_overlay.h"

#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include "ui/ui_style.h"
#include "ui/ui_widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace dq8::ui {
namespace {
using gfx::PadInput;

constexpr const char *kTimes = "\xC3\x97"; // U+00D7, spelled out for compilers without /utf-8
constexpr Icon kPageIcons[] = {Icon::Display, Icon::Sound, Icon::Controls, Icon::Disc, Icon::Interface, Icon::Gear, Icon::Wrench};

// The Cheats page's options, in the host's list with the Game page's (Rogue Galaxy: added).
bool isCheatKey(const std::string &key) { return key.rfind("cheat", 0) == 0; }
constexpr const char *kProjectUrl = "https://github.com/victor-takai/rogue-galaxy-recomp";
constexpr const char *kIssuesUrl = "https://github.com/victor-takai/rogue-galaxy-recomp/issues";

constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize;

ImVec4 color(ImU32 packed) { return ImGui::ColorConvertU32ToFloat4(packed); }

// A round "x" in a window's corner.
bool closeButton(const char *id) {
    const float size = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    const ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
    ImDrawList *list = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered())
        list->AddCircleFilled(center, size * 0.5f, palette::kHover);
    drawIcon(list, Icon::Close, center, size * 0.62f,
             ImGui::IsItemHovered() ? palette::kText : palette::kTextMuted);
    return pressed;
}

// A window's title row: icon, title, and a close button at the right end.
void dialogHeader(Icon kind, const char *title, bool *open) {
    icon(kind, em(1.4f), palette::kGold);
    ImGui::SameLine(0.0f, em(0.6f));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (open) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetFrameHeight());
        if (closeButton("##close"))
            *open = false;
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y + em(0.15f)),
                                        ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + em(0.15f)),
                                        withAlpha(palette::kGold, 0.35f), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
}

void centerNextWindow() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
}

// Alternatives are separated by `joiner` ("or", or "/"); the keys of one
// alternative are a chord, joined by "+".
using KeyChord = std::initializer_list<const char *>;
void shortcutAlternatives(std::initializer_list<KeyChord> alternatives, const char *what, const char *joiner = "or") {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    bool firstAlternative = true;
    for (const KeyChord &chord : alternatives) {
        if (!firstAlternative) {
            ImGui::SameLine(0.0f, em(0.3f));
            ImGui::AlignTextToFramePadding();
            mutedText("%s", joiner);
            ImGui::SameLine(0.0f, em(0.3f));
        }
        firstAlternative = false;
        bool firstKey = true;
        for (const char *key : chord) {
            if (!firstKey) {
                ImGui::SameLine(0.0f, em(0.3f));
                ImGui::AlignTextToFramePadding();
                mutedText("+");
                ImGui::SameLine(0.0f, em(0.3f));
            }
            firstKey = false;
            keycap(key);
        }
    }
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(what);
}

std::string conflictWith(const gfx::PadConfig &config, PadInput input, bool keyboard,
                         const gfx::PadBinding &binding) {
    if (!binding.bound())
        return {};
    for (size_t other = 0; other < gfx::kPadInputCount; ++other) {
        if (static_cast<PadInput>(other) == input)
            continue;
        for (const auto &candidate : config.slots(static_cast<PadInput>(other), keyboard))
            if (candidate == binding)
                return gfx::padInputName(static_cast<PadInput>(other));
    }
    return {};
}
} // namespace

// RG_UI_SCRIPT's "scrollpage N|end": the open page scrolls to N pixels (or
// its end) once, for screenshots of what is below the fold (Rogue Galaxy:
// added). g_scrollPageInfo reports the scroll range the page last had.
float g_scrollPageTo = -1.0f;
float g_scrollPageInfo[2] = {0.0f, 0.0f}; // scroll range, visible height
// "unfold on": every folded section shows its settings, for screenshots.
bool g_unfoldAll = false;

namespace {
// What "default" means on each page: a fresh Settings.
const Settings &defaults() {
    static const Settings fresh;
    return fresh;
}

// A two-step button for the page's "restore defaults": the first press asks,
// a second within four seconds does it. The id stays put while the label
// changes, so a focused button keeps its focus.
bool restoreButton(const char *label) {
    static ImGuiID armedId = 0;
    static double armedAt = -100.0;
    const ImGuiID id = ImGui::GetID("##restore");
    const bool armed = armedId == id && ImGui::GetTime() - armedAt < 4.0;
    if (armed)
        ImGui::PushStyleColor(ImGuiCol_Button, color(withAlpha(palette::kWarn, 0.35f)));
    const std::string text = armed ? "Press again to restore" : label;
    const bool pressed = ImGui::Button((text + "###restore").c_str());
    if (armed)
        ImGui::PopStyleColor();
    if (!pressed)
        return false;
    if (armed) {
        armedId = 0;
        return true;
    }
    armedId = id;
    armedAt = ImGui::GetTime();
    return false;
}

// The foot of a page: restore its defaults, and what the gold dots mean.
bool pageFooter(const char *restoreLabel) {
    ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y),
                                        IM_COL32(255, 255, 255, 34), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
    const bool done = restoreButton(restoreLabel);
    ImGui::SameLine(0.0f, em(0.9f));
    ImGui::AlignTextToFramePadding();
    const ImVec2 dot = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + em(0.2f), dot.y + ImGui::GetFrameHeight() * 0.5f),
                                                em(0.17f), palette::kGold);
    ImGui::Dummy(ImVec2(em(0.6f), ImGui::GetFrameHeight()));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::AlignTextToFramePadding();
    mutedText("changed from the default");
    return done;
}
} // namespace

void Overlay::drawSettingsWindow() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImVec2 area = viewport->WorkSize;
    const ImVec2 size(std::min(area.x - em(2.0f), em(54.0f)), std::min(area.y - em(2.0f), em(46.0f)));
    centerNextWindow();
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (m_focusSettings) {
        ImGui::SetNextWindowFocus();
        m_focusSettings = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.2f), em(1.0f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (beginWindow("Settings##dq8", nullptr, flags)) {
        dialogHeader(Icon::Gear, "Settings", &m_settingsOpen);

        // A narrow window shows the page list as icons, so the page keeps its room.
        const bool compact = size.x < em(44.0f);
        const float navWidth = compact ? em(3.4f) : em(11.0f);
        // NavFlattened: the d-pad crosses from the page list into the page and back.
        ImGui::BeginChild("##nav", ImVec2(navWidth, 0.0f), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
        if (compact)
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, em(0.2f)));
        for (int page = 0; page < static_cast<int>(SettingsPage::Count); ++page)
            if (navItem(kSettingsPageNames[page], kPageIcons[page], m_page == static_cast<SettingsPage>(page), compact))
                m_page = static_cast<SettingsPage>(page);
        if (compact)
            ImGui::PopStyleVar();
        const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        if (!compact && ImGui::GetContentRegionAvail().y > footer) {
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footer);
            pushScaledFont(0.84f);
            mutedText("Rogue Galaxy Recompiled %s", m_host.version.c_str());
            mutedText("Changes save automatically");
            ImGui::PopFont();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, em(0.8f));
        const ImVec2 rule = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(rule.x, rule.y), ImVec2(rule.x, rule.y + ImGui::GetContentRegionAvail().y),
                                            IM_COL32(255, 255, 255, 34), 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(rule.x + em(0.9f), rule.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleVar();
        // Leaves room for the scrollbar without the page touching it.
        ImGui::PushItemWidth(-em(0.6f));
        ImGui::Indent(em(0.1f));
        switch (m_page) {
        case SettingsPage::Display: drawDisplayPage(); break;
        case SettingsPage::Sound: drawSoundPage(); break;
        case SettingsPage::Controls: drawControlsPage(); break;
        case SettingsPage::Game: drawGamePage(); break;
        case SettingsPage::Interface: drawInterfacePage(); break;
        case SettingsPage::Setup: drawSetupPage(); break;
        case SettingsPage::Cheats: drawCheatsPage(); break;
        case SettingsPage::Count: break;
        }
        ImGui::Unindent(em(0.1f));
        ImGui::PopItemWidth();
        ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
        g_scrollPageInfo[0] = ImGui::GetScrollMaxY();
        g_scrollPageInfo[1] = ImGui::GetWindowHeight();
        if (g_scrollPageTo >= 0.0f) {
            ImGui::SetScrollY(g_scrollPageTo > 1.0e6f ? ImGui::GetScrollMaxY() : g_scrollPageTo);
            g_scrollPageTo = -1.0f;
        }
        ImGui::EndChild();

        // L1 and R1 turn the pages, as in the game's own menus.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            const int count = static_cast<int>(SettingsPage::Count);
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
                m_page = static_cast<SettingsPage>((static_cast<int>(m_page) + count - 1) % count);
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
                m_page = static_cast<SettingsPage>((static_cast<int>(m_page) + 1) % count);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawDisplayPage() {
    const Settings &def = defaults();
    sectionHeader("Window");
    if (beginSettings("window")) {
        settingRow("Mode");
        int mode = m_settings.fullscreen ? 1 : 0;
        static constexpr const char *kModes[] = {"Windowed", "Fullscreen"};
        if (segmented("mode", &mode, kModes, 2))
            setFullscreen(mode == 1);

        settingRow("Vertical sync", "Fast tears less than Off, with less delay than On.", nullptr,
                   m_settings.display.presentMode != def.display.presentMode);
        static constexpr SDL_GPUPresentMode kPresent[] = {SDL_GPU_PRESENTMODE_VSYNC, SDL_GPU_PRESENTMODE_MAILBOX,
                                                         SDL_GPU_PRESENTMODE_IMMEDIATE};
        static constexpr const char *kSync[] = {"On", "Fast", "Off"};
        bool supported[3] = {true, m_backend.supportsPresentMode(kPresent[1]), m_backend.supportsPresentMode(kPresent[2])};
        int sync = 0;
        for (int index = 0; index < 3; ++index)
            if (m_settings.display.presentMode == kPresent[index])
                sync = index;
        if (segmented("sync", &sync, kSync, 3, supported)) {
            m_settings.display.presentMode = kPresent[sync];
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }
        rowDetail("On waits for the display's refresh. Fast avoids tearing with less delay, where the system "
                  "offers it. Off draws as fast as it can and may tear.");
        endSettings();
    }

    sectionHeader("Picture");
    if (beginSettings("picture")) {
        const uint32_t active = m_backend.activeResolutionScale();
        const uint32_t shown = m_pendingScale != 0u ? m_pendingScale : active;
        const bool automatic = m_settings.autoScale || m_autoForced;
        settingRow("Internal resolution",
                   m_pendingScale == 0u ? "Higher is sharper and slower. Auto follows the window."
                   : pausing()          ? "Applies when the game resumes."
                                        : "Applying...",
                   "Experimental", m_settings.autoScale || m_settings.resolutionScale != def.resolutionScale);
        char preview[128];
        std::snprintf(preview, sizeof(preview), "%s%u%s   %u %s %u", automatic ? "Auto: " : "", shown, kTimes,
                      512u * shown, kTimes, 448u * shown);
        if (ImGui::BeginCombo("##scale", preview)) {
            // Rogue Galaxy: added. From the window's height in pixels.
            if (ImGui::Selectable("Auto (from the window's height)", automatic))
                setAutoScale(true);
            for (uint32_t scale = 1u; scale <= 8u; ++scale) {
                char item[64];
                std::snprintf(item, sizeof(item), "%u%s   %u %s %u%s", scale, kTimes, 512u * scale, kTimes,
                              448u * scale, scale == 1u ? "   (PS2)" : "");
                if (ImGui::Selectable(item, !automatic && scale == shown))
                    requestScale(scale);
            }
            ImGui::EndCombo();
        }
        rowDetail("Draws the game at a multiple of the PS2's own 512 x 448. Auto picks the multiple from the "
                  "window's height (see Advanced). Above 1x is slower for now.");

        const char *aspectHelp = "";
        switch (m_settings.display.aspect) {
        case gfx::SdlGpuAspect::Auto: {
            // Rogue Galaxy's is on the Game page.
            const int wide = m_backend.gameWidescreen();
            aspectHelp = wide == 1   ? "Follows Widescreen on the Game page: the window's shape."
                         : wide == 0 ? "Follows Widescreen on the Game page: 4:3."
                                     : "4:3: this game has no widescreen option.";
            break;
        }
        case gfx::SdlGpuAspect::Standard: aspectHelp = "4:3, the shape a television gave the game."; break;
        case gfx::SdlGpuAspect::Wide:
            aspectHelp = m_backend.gameWidescreen() == 1 ? "16:9 whatever the window; the game draws for it."
                                                         : "Stretches the 4:3 picture to 16:9.";
            break;
        case gfx::SdlGpuAspect::Native: aspectHelp = "Square pixels: narrower than on a television."; break;
        case gfx::SdlGpuAspect::Stretch: aspectHelp = "Fills the window, whatever its shape."; break;
        }
        settingRow("Aspect ratio", aspectHelp, "Experimental", m_settings.display.aspect != def.display.aspect);
        static constexpr const char *kAspects[] = {"Auto", "4:3", "16:9", "Native", "Stretch"};
        int aspect = static_cast<int>(m_settings.display.aspect);
        if (segmented("aspect", &aspect, kAspects, 5)) {
            m_settings.display.aspect = static_cast<gfx::SdlGpuAspect>(aspect);
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }

        static constexpr const char *kFilterHelp[] = {
            "Crisp pixels with even columns, at any size.", "Soft, smooth scaling.",
            "Raw pixels; uneven unless the size is a whole multiple.",
            // Rogue Galaxy: added.
            "AMD FSR 1: smoother edges when the window is larger.",
            "Averages every rendered pixel, for 4x to 8x resolution."};
        static constexpr const char *kFilterDetail[] = {
            "", "", "",
            "AMD FSR 1's edge-aware upscaling: smooth edges without blur when the window is larger than the "
            "internal resolution; smooth scaling when it is not. Raise the internal resolution first: it adds "
            "real detail, which no upscaler can.",
            "For an internal resolution larger than the window (4x to 8x): every rendered pixel under each "
            "screen pixel is averaged (Lanczos), so edges stay smooth and fine detail stops shimmering. Smooth "
            "scaling when the window is the larger."};
        settingRow("Scaling filter", kFilterHelp[static_cast<int>(m_settings.display.filter)], nullptr,
                   m_settings.display.filter != def.display.filter);
        static constexpr const char *kFilters[] = {"Sharp", "Smooth", "Nearest", "FSR", "Supersample"};
        int filter = static_cast<int>(m_settings.display.filter);
        if (segmented("filter", &filter, kFilters, 5)) {
            m_settings.display.filter = static_cast<gfx::SdlGpuFilter>(filter);
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }
        rowDetail(kFilterDetail[static_cast<int>(m_settings.display.filter)]);

        // Rogue Galaxy: added (`sharpening` in the file).
        settingRow("Sharpening", "AMD FSR 1 contrast sharpening, with any filter.", nullptr,
                   m_settings.display.sharpening != def.display.sharpening);
        static constexpr const char *kSharpening[] = {"Off", "Low", "Medium", "High"};
        int sharpening = std::min<int>(m_settings.display.sharpening, 3);
        if (segmented("sharpening", &sharpening, kSharpening, 4)) {
            m_settings.display.sharpening = static_cast<uint8_t>(sharpening);
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }
        rowDetail("Crisper edges and textures without halos. Off by default.");

        // Rogue Galaxy: added (`sides` in the file).
        static constexpr const char *kSidesHelp[] = {
            "Black bars beside a 4:3 picture.", "The picture, blurred, beside a 4:3 picture.",
            "Your image from the borders folder beside a 4:3 picture."};
        settingRow("Borders", kSidesHelp[static_cast<int>(m_settings.display.sides)], nullptr,
                   m_settings.display.sides != def.display.sides);
        static constexpr const char *kSides[] = {"Black", "Blurred", "Image"};
        int sides = static_cast<int>(m_settings.display.sides);
        if (segmented("sides", &sides, kSides, 3)) {
            m_settings.display.sides = static_cast<gfx::SdlGpuSides>(sides);
            m_backend.setDisplayOptions(m_settings.display);
            markDirty();
        }
        rowDetail("What fills the window beside a 4:3 picture: black bars; the picture itself, softly blurred and "
                  "a little darker; or your image, the first PNG or JPG in the game's \"borders\" folder (the "
                  "blurred picture without one).");
        endSettings();
    }

    if (beginSection("Advanced", false) || g_unfoldAll) {
        if (beginSettings("advanced")) {
            const bool automatic = m_settings.autoScale || m_autoForced;
            ImGui::BeginDisabled(!automatic);
            settingRow("Auto resolution: oversampling",
                       automatic ? "Lines drawn per line of the window; more is smoother." : "Used while Internal resolution is Auto.",
                       nullptr, m_settings.autoOversample != def.autoOversample);
            static constexpr const char *kOversample[] = {"1.0", "1.25", "1.5"};
            int oversample = m_settings.autoOversample >= 150u ? 2 : m_settings.autoOversample >= 125u ? 1 : 0;
            if (segmented("oversample", &oversample, kOversample, 3)) {
                m_settings.autoOversample = oversample == 2 ? 150u : oversample == 1 ? 125u : 100u;
                m_autoHeight = -1;
                markDirty();
            }
            rowDetail("1.0 draws at least the window's height in lines (1440 lines take 4x, 1792 lines); 1.25 and "
                      "1.5 draw more, smoother and slower. A wide window costs more at the same height.");
            settingRow("Auto resolution: highest", "The most Auto will pick.", nullptr, m_settings.autoCap != def.autoCap);
            static constexpr const char *kCaps[] = {"2x", "3x", "4x", "5x", "6x", "7x", "8x"};
            int cap = static_cast<int>(std::clamp(m_settings.autoCap, 2u, 8u)) - 2;
            if (segmented("autocap", &cap, kCaps, 7)) {
                m_settings.autoCap = static_cast<uint32_t>(cap + 2);
                m_autoHeight = -1;
                markDirty();
            }
            ImGui::EndDisabled();

            settingRow("Integer scaling", "Whole-number scale only: even lines, with borders.", nullptr,
                       m_settings.display.integerScale != def.display.integerScale);
            if (toggle("integer", &m_settings.display.integerScale)) {
                m_backend.setDisplayOptions(m_settings.display);
                markDirty();
            }
            rowDetail("Scales the picture by whole multiples of its height, so every line is the same thickness.");

            settingRow("Sharper picture", "Removes the PS2's line blending, which only blurs on a monitor.", nullptr,
                       m_settings.display.removeLineBlend != def.display.removeLineBlend);
            if (toggle("sharper", &m_settings.display.removeLineBlend)) {
                m_backend.setDisplayOptions(m_settings.display);
                markDirty();
            }
            rowDetail("Drops the PS2's line blending, a deflicker for TVs that only blurs on a monitor.");
            endSettings();
        }
    }

    if (pageFooter("Restore picture defaults")) {
        // The window's mode stays as it is.
        m_settings.display = def.display;
        m_settings.autoOversample = def.autoOversample;
        m_settings.autoCap = def.autoCap;
        m_autoHeight = -1;
        m_backend.setDisplayOptions(m_settings.display);
        requestScale(def.resolutionScale);
        markDirty();
        notify("Picture settings restored", Icon::Check);
    }
}

void Overlay::drawSoundPage() {
    const Settings &def = defaults();
    sectionHeader("Volume");
    if (beginSettings("sound")) {
        settingRow("Master volume", nullptr, nullptr, m_settings.volume != def.volume);
        if (ImGui::SliderInt("##volume", &m_settings.volume, 0, 100, "%d%%")) {
            applyVolume();
            markDirty();
        }
        settingRow("Mute", nullptr, nullptr, m_settings.muted != def.muted);
        if (toggle("mute", &m_settings.muted)) {
            applyVolume();
            markDirty();
        }
        settingRow("Mute in the background", "Silences the game while its window is not in front.", nullptr,
                   m_settings.muteInBackground != def.muteInBackground);
        if (toggle("background", &m_settings.muteInBackground)) {
            applyVolume();
            markDirty();
        }
        endSettings();
    }
    if (!m_host.setVolume)
        mutedText("Sound is not available in this build.");
    if (pageFooter("Restore sound defaults")) {
        m_settings.volume = def.volume;
        m_settings.muted = def.muted;
        m_settings.muteInBackground = def.muteInBackground;
        applyVolume();
        markDirty();
        notify("Sound settings restored", Icon::Check);
    }
}

void Overlay::drawInterfacePage() {
    const Settings &def = defaults();
    sectionHeader("Menu");
    if (beginSettings("menu")) {
        settingRow("Menu size", "Scales this menu; a small window caps it to fit.", nullptr, m_settings.uiScale != def.uiScale);
        // Applied on release: the menu resizing under the cursor mid-drag
        // would move the slider away from it.
        if (!ImGui::IsAnyItemActive() || m_uiScaleEdit == 0)
            m_uiScaleEdit = m_settings.uiScale;
        ImGui::SliderInt("##uiscale", &m_uiScaleEdit, 75, 200, "%d%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_settings.uiScale = m_uiScaleEdit;
            markDirty();
        }
        settingRow("Notifications", "Short messages about controllers, screenshots and changes.", nullptr,
                   m_settings.notifications != def.notifications);
        if (toggle("notifications", &m_settings.notifications))
            markDirty();
        settingRow("Hide the idle mouse cursor", "Hides the pointer while it is not moving.", nullptr,
                   m_settings.hideIdleCursor != def.hideIdleCursor);
        if (toggle("cursor", &m_settings.hideIdleCursor))
            markDirty();
        endSettings();
    }

    if (canPause() || canChangeSpeed()) {
        sectionHeader("Pause and speed");
        if (beginSettings("pausing")) {
            if (canPause()) {
                settingRow("Pause while the menu is open", nullptr, nullptr, m_settings.pauseInMenu != def.pauseInMenu);
                if (toggle("pausemenu", &m_settings.pauseInMenu)) {
                    applyPause();
                    markDirty();
                }
                settingRow("Pause in the background", "Stops the game while its window is not in front.", nullptr,
                           m_settings.pauseInBackground != def.pauseInBackground);
                if (toggle("pausebackground", &m_settings.pauseInBackground)) {
                    applyPause();
                    markDirty();
                }
            }
            if (canChangeSpeed()) {
                settingRow("Fast-forward speed", "F4 switches it on and off; ` held runs at it.", "Experimental",
                           m_settings.fastForwardSpeed != def.fastForwardSpeed);
                // 1.5 and 8 (Rogue Galaxy: added); "Max" is unpaced.
                static constexpr float kSpeeds[] = {1.5f, 2.0f, 3.0f, 4.0f, 8.0f, 0.0f};
                static constexpr const char *kSpeedNames[] = {"1.5\xC3\x97", "2\xC3\x97", "3\xC3\x97",
                                                              "4\xC3\x97", "8\xC3\x97", "Max"};
                constexpr int kSpeedCount = static_cast<int>(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
                int speed = 1;
                for (int index = 0; index < kSpeedCount; ++index)
                    if (fastForwardRate() == kSpeeds[index])
                        speed = index;
                if (segmented("speed", &speed, kSpeedNames, kSpeedCount))
                    setFastForwardSpeed(kSpeeds[speed]);
                rowDetail("Max runs as fast as the PC goes. Music and sounds keep their tempo, and movies play at "
                          "normal speed.");
                if (m_host.setTriggerFastForward) {
                    settingRow("L2 and R2 control fast-forward", "L2 switches it, R2 held runs it; the game loses both.",
                               nullptr, m_settings.triggerFastForward != def.triggerFastForward);
                    if (toggle("triggerspeed", &m_settings.triggerFastForward)) {
                        m_host.setTriggerFastForward(m_settings.triggerFastForward);
                        markDirty();
                    }
                    rowDetail("R2 held fast-forwards, L2 switches it on and off, with the controller or the keys "
                              "bound to them. The game then no longer gets L2 and R2 (the free camera still does).");
                }
            }
            endSettings();
        }
    }

    sectionHeader("Frame rate");
    if (beginSettings("framerate")) {
        settingRow("Show the frame rate", "Frames the game draws each second (F3).", nullptr,
                   m_settings.showFps != def.showFps);
        if (toggle("fps", &m_settings.showFps))
            markDirty();
        settingRow("Position", nullptr, nullptr, m_settings.fpsCorner != def.fpsCorner);
        static constexpr const char *kCorners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        int corner = static_cast<int>(m_settings.fpsCorner);
        if (segmented("corner", &corner, kCorners, 4)) {
            m_settings.fpsCorner = static_cast<Corner>(corner);
            markDirty();
        }
        endSettings();
    }

    if (pageFooter("Restore interface defaults")) {
        m_settings.uiScale = def.uiScale;
        m_uiScaleEdit = 0;
        m_settings.notifications = def.notifications;
        m_settings.hideIdleCursor = def.hideIdleCursor;
        m_settings.pauseInMenu = def.pauseInMenu;
        m_settings.pauseInBackground = def.pauseInBackground;
        m_settings.showFps = def.showFps;
        m_settings.fpsCorner = def.fpsCorner;
        m_settings.triggerFastForward = def.triggerFastForward;
        if (m_host.setTriggerFastForward)
            m_host.setTriggerFastForward(def.triggerFastForward);
        if (canChangeSpeed())
            setFastForwardSpeed(def.fastForwardSpeed);
        applyPause();
        markDirty();
        notify("Interface settings restored", Icon::Check);
    }
}

// Setup and HD (Rogue Galaxy: added): what the install folder holds, read from
// the folders the game reads (HD textures, HD movies) and the settings, and a
// button that opens the setup window to add or change a part.
void Overlay::drawSetupPage() {
    if (!m_host.setupInfo) {
        mutedText("This build has no setup to open.");
        return;
    }
    const double now = ImGui::GetTime();
    if (now - m_setupAt > 2.0 || now < m_setupAt) {
        m_setupInfo = m_host.setupInfo();
        m_setupAt = now;
    }
    const SetupInfo &info = m_setupInfo;
    auto group = [](int n) {
        std::string digits = std::to_string(n), out;
        for (size_t i = 0; i < digits.size(); ++i) {
            if (i && (digits.size() - i) % 3 == 0)
                out += ',';
            out += digits[i];
        }
        return out;
    };
    auto choice = [&](const char *key) -> std::string {
        for (const GameOption &option : m_host.gameOptions)
            if (option.key == key && option.number()) {
                const int index = std::clamp(gameNumber(option) - option.minimum, 0,
                                             static_cast<int>(option.choices.size()) - 1);
                return option.choices.empty() ? std::string() : option.choices[static_cast<size_t>(index)];
            }
        return "-";
    };

    sectionHeader("What is installed");
    if (beginSettings("installed")) {
        settingRow("HD textures", "Sharper pictures. Switch them on or off under Game > Texture replacements.");
        ImGui::AlignTextToFramePadding();
        if (info.textureFiles > 0)
            ImGui::Text("%s pictures", group(info.textureFiles).c_str());
        else
            mutedText("Not installed");
        settingRow("HD movies", "Upscaled cutscenes. Switch them under Game > HD movies.");
        ImGui::AlignTextToFramePadding();
        if (info.movieFiles > 0)
            ImGui::Text("%d of %d", info.movieFiles, info.movieTotal);
        else
            mutedText("Not installed");
        settingRow("Font", "Change it under Game > Font.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(choice("font_style").c_str());
        settingRow("Button prompts", "Change them under Game > Button prompts.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(choice("button_prompts").c_str());
        settingRow("Installed by", "The version of the setup that built this folder.");
        ImGui::AlignTextToFramePadding();
        if (!info.release.empty())
            ImGui::Text("Setup %s", info.release.c_str());
        else
            mutedText("Not recorded");
        endSettings();
    }

    sectionHeader("Add or change");
    if (beginSettings("setup")) {
        settingRow("Run the setup", "Adds the HD parts you skipped, or updates after a new release. Close the game "
                                    "first if it has to be rebuilt.");
        ImGui::BeginDisabled(!info.canRun);
        if (ImGui::Button("Open the setup")) {
            if (m_host.runSetup && m_host.runSetup())
                notify("The setup is opening", Icon::Check);
            else
                notify("The setup could not be started", Icon::Warning);
        }
        ImGui::EndDisabled();
        if (!info.canRun && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("RogueGalaxy-Setup.exe is not in the game's folder.");
        rowDetail("The setup opens in its own window. It can add HD textures and HD movies, and it keeps your saves "
                  "and settings. HD parts need a graphics card with Vulkan.");
        endSettings();
    }
}

// The game's own switches, in the host's sections (Rogue Galaxy: added).
// RG_UI_SCRIPT's "scrollto KEY": the Game page scrolls to that option once,
// for screenshots of options below the fold (Rogue Galaxy: added).
std::string g_scrollToGameOption;

void Overlay::drawGamePage() {
    drawGameOptions(false);
    if (pageFooter("Restore game defaults")) {
        restoreGameDefaults(false);
        notify("Game settings restored", Icon::Check);
    }
}

void Overlay::restoreGameDefaults(bool cheats) {
    for (const GameOption &option : m_host.gameOptions) {
        if (isCheatKey(option.key) != cheats || option.action || !option.remembered)
            continue;
        if (option.number())
            setGameNumberValue(option.key, option.defaultNumber);
        else
            setGameOptionValue(option.key, option.defaultValue);
    }
}

void Overlay::drawGameOptions(bool cheats) {
    if (m_host.gameOptions.empty()) {
        mutedText("The game has no options of its own.");
        return;
    }
    const bool masterOn = m_host.cheatsStatus && m_host.cheatsStatus() > 0;
    bool started = false, table = false;
    for (const GameOption &option : m_host.gameOptions) {
        if (isCheatKey(option.key) != cheats)
            continue;
        const auto section = std::find_if(m_host.gameSections.begin(), m_host.gameSections.end(),
                                          [&](const auto &entry) { return entry.first == option.key; });
        if (!started || section != m_host.gameSections.end()) {
            if (table)
                endSettings();
            const bool folded = std::find(m_host.gameFolded.begin(), m_host.gameFolded.end(), option.key) !=
                                m_host.gameFolded.end();
            const bool open = beginSection(section != m_host.gameSections.end() ? section->second.c_str() : "Game",
                                           !folded) || g_unfoldAll;
            table = open && beginSettings(("game." + option.key).c_str());
            started = true;
        }
        if (!table)
            continue;
        const std::string *detailText = nullptr;
        for (const auto &entry : m_host.gameDetails)
            if (entry.first == option.key)
                detailText = &entry.second;
        const char *detail = detailText ? detailText->c_str() : nullptr;
        const bool changed = option.remembered && !option.action &&
                             (option.number() ? gameNumber(option) != option.defaultNumber
                                              : gameOption(option) != option.defaultValue);
        // A switch that waits for a restart says so in a badge, not under its control.
        settingRow(option.label.c_str(), option.help.empty() ? nullptr : option.help.c_str(),
                   waitsForRestart(option) ? "Restart needed" : nullptr, changed);
        if (!g_scrollToGameOption.empty() && g_scrollToGameOption == option.key) {
            ImGui::SetScrollHereY(0.2f);
            g_scrollToGameOption.clear();
        }
        if (option.number() && !option.choices.empty()) {
            // Named choices side by side (Rogue Galaxy: added).
            std::vector<const char *> labels;
            for (const std::string &choice : option.choices)
                labels.push_back(choice.c_str());
            int index = std::clamp(gameNumber(option) - option.minimum, 0, static_cast<int>(labels.size()) - 1);
            if (segmented(option.key.c_str(), &index, labels.data(), static_cast<int>(labels.size()))) {
                const int next = std::clamp(option.minimum + index, option.minimum, option.maximum);
                m_settings.gameNumbers[option.key] = next;
                m_settings.gameOptions.erase(option.key);
                if (m_host.setGameNumber)
                    m_host.setGameNumber(option.key, next);
                markDirty();
            }
            rowDetail(detail);
            continue;
        }
        if (option.number()) {
            const int value = gameNumber(option);
            int next = value;
            const ImGuiStyle &style = ImGui::GetStyle();
            const float button = ImGui::GetFrameHeight();
            const float reset = ImGui::CalcTextSize("Default").x + style.FramePadding.x * 2.0f;
            ImGui::PushID(option.key.c_str());
            if (ImGui::Button("-", ImVec2(button, 0.0f)))
                next = value - option.step;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(button * 3.0f, ImGui::GetContentRegionAvail().x - button -
                                                                reset - style.ItemSpacing.x * 2.0f));
            ImGui::SliderInt("##value", &next, option.minimum, option.maximum,
                             option.format.empty() ? "%d" : option.format.c_str());
            rowDetail(detail);
            ImGui::SameLine();
            if (ImGui::Button("+", ImVec2(button, 0.0f)))
                next = value + option.step;
            ImGui::SameLine();
            ImGui::BeginDisabled(value == option.defaultNumber);
            if (ImGui::Button("Default"))
                next = option.defaultNumber;
            ImGui::EndDisabled();
            ImGui::PopID();
            next = std::clamp(next, option.minimum, option.maximum);
            if (next != value) {
                m_settings.gameNumbers[option.key] = next;
                // A "0" or "1" in the file reads as a switch too.
                m_settings.gameOptions.erase(option.key);
                if (m_host.setGameNumber)
                    m_host.setGameNumber(option.key, next);
                markDirty();
            }
            continue;
        }
        if (option.action) {
            // A one-shot: only with the master switch on.
            ImGui::BeginDisabled(!masterOn);
            ImGui::PushID(option.key.c_str());
            if (ImGui::Button(option.format.empty() ? "Apply" : option.format.c_str()) && m_host.setGameOption)
                m_host.setGameOption(option.key, true);
            ImGui::PopID();
            ImGui::EndDisabled();
            if (!masterOn && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Turn Cheats on first.");
            continue;
        }
        bool on = gameOption(option);
        if (!option.remembered) {
            if (toggle(option.key.c_str(), &on)) {
                if (option.key == "cheats_master")
                    requestCheatsMaster(on);
                else if (m_host.setGameOption)
                    m_host.setGameOption(option.key, on);
            }
            rowDetail(detail);
            continue;
        }
        if (toggle(option.key.c_str(), &on)) {
            m_settings.gameOptions[option.key] = on;
            m_settings.gameNumbers.erase(option.key);
            if (m_host.setGameOption)
                m_host.setGameOption(option.key, on);
            markDirty();
        }
        rowDetail(detail);
    }
    if (table)
        endSettings();
}

void Overlay::drawControlsPage() {
    const Settings &def = defaults();
    const auto controllers = m_backend.padInput().controllers();
    sectionHeader("Devices");
    if (beginSettings("devices")) {
        gfx::PadConfig config = m_settings.pad;
        bool changed = false;
        settingRow("Keyboard", "Plays alongside the controller.", nullptr,
                   config.keyboardEnabled != def.pad.keyboardEnabled);
        changed |= toggle("keyboard", &config.keyboardEnabled);

        settingRow("Controller", "Which controller plays the game.", nullptr,
                   config.controllers != def.pad.controllers);
        std::string preview = "Any connected controller";
        if (config.controllers == gfx::ControllerSelection::None) {
            preview = "No controller";
        } else if (config.controllers == gfx::ControllerSelection::One) {
            const bool connected = std::any_of(controllers.begin(), controllers.end(),
                                               [](const auto &c) { return c.active; });
            preview = config.controllerName + (connected ? "" : "  (not connected)");
        }
        if (ImGui::BeginCombo("##controller", preview.c_str())) {
            if (ImGui::Selectable("Any connected controller", config.controllers == gfx::ControllerSelection::Any)) {
                config.controllers = gfx::ControllerSelection::Any;
                changed = true;
            }
            for (size_t index = 0; index < controllers.size(); ++index) {
                const auto &controller = controllers[index];
                ImGui::PushID(static_cast<int>(index));
                // Identical controllers share a GUID, so the device in use is
                // what marks the choice, and the pick remembers the device.
                const bool chosen = config.controllers == gfx::ControllerSelection::One && controller.active;
                if (ImGui::Selectable(controller.name.c_str(), chosen)) {
                    config.controllers = gfx::ControllerSelection::One;
                    config.controllerGuid = controller.guid;
                    config.controllerSerial = controller.serial;
                    config.controllerName = controller.name;
                    config.controllerInstance = controller.id;
                    changed = true;
                }
                ImGui::PopID();
            }
            if (ImGui::Selectable("No controller", config.controllers == gfx::ControllerSelection::None)) {
                config.controllers = gfx::ControllerSelection::None;
                changed = true;
            }
            ImGui::EndCombo();
        }

        settingRow("Open the menu with Back + Start", "The Guide button (Xbox, PS, Home) always does.", nullptr,
                   m_settings.backStartOpensMenu != def.backStartOpensMenu);
        if (toggle("backstart", &m_settings.backStartOpensMenu))
            markDirty();
        endSettings();
        if (changed)
            setPadConfig(config);
    }

    // What is plugged in, and what of it plays.
    if (controllers.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        mutedText("No controller connected. Plug one in or pair it; it is picked up at once.");
        ImGui::PopTextWrapPos();
    } else {
        for (const auto &controller : controllers) {
            ImGui::PushID(&controller);
            icon(Icon::Controls, em(1.1f), controller.active ? palette::kGold : palette::kTextMuted);
            ImGui::SameLine(0.0f, em(0.5f));
            ImGui::TextUnformatted(controller.name.c_str());
            ImGui::SameLine(0.0f, em(0.6f));
            mutedText("%s", controllerFamilyName(controller.type));
            if (controller.batteryPercent >= 0 && (controller.power == SDL_POWERSTATE_ON_BATTERY ||
                                                   controller.power == SDL_POWERSTATE_CHARGING)) {
                ImGui::SameLine(0.0f, em(0.6f));
                mutedText("%d%%%s", controller.batteryPercent,
                          controller.power == SDL_POWERSTATE_CHARGING ? " charging" : "");
            }
            ImGui::SameLine(0.0f, em(0.6f));
            ImGui::TextColored(color(controller.active ? palette::kGood : palette::kTextMuted), "%s",
                               controller.active ? "In use" : "Not used");
            ImGui::PopID();
        }
    }

    sectionHeader("Sticks and triggers");
    if (beginSettings("analog")) {
        gfx::PadConfig config = m_settings.pad;
        bool changed = false;
        settingRow("Stick dead zone", "Ignores the small drift of a worn stick.", nullptr,
                   std::fabs(config.stickDeadZone - def.pad.stickDeadZone) > 0.001f);
        float deadZone = config.stickDeadZone * 100.0f;
        if (ImGui::SliderFloat("##deadzone", &deadZone, 0.0f, 40.0f, "%.0f%%")) {
            config.stickDeadZone = deadZone / 100.0f;
            changed = true;
        }
        settingRow("Trigger press point", "How far L2 and R2 travel before they count as pressed.", nullptr,
                   std::fabs(config.triggerThreshold - def.pad.triggerThreshold) > 0.001f);
        float threshold = config.triggerThreshold * 100.0f;
        if (ImGui::SliderFloat("##trigger", &threshold, 5.0f, 90.0f, "%.0f%%")) {
            config.triggerThreshold = threshold / 100.0f;
            changed = true;
        }
        settingRow("Invert camera left and right", nullptr, nullptr, config.invertCameraX != def.pad.invertCameraX);
        changed |= toggle("invertx", &config.invertCameraX);
        settingRow("Invert camera up and down", nullptr, nullptr, config.invertCameraY != def.pad.invertCameraY);
        changed |= toggle("inverty", &config.invertCameraY);
        endSettings();
        if (changed)
            setPadConfig(config);
    }

    sectionHeader("Button bindings");
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const float diagram = std::min(width * 0.5f, em(15.0f));
        const ImVec2 size(diagram, diagram * 0.62f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        drawPadDiagram(ImGui::GetWindowDrawList(), at, size, m_backend.padInput().preview(), m_hoveredInput);
        ImGui::SameLine(0.0f, em(1.2f));
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
        ImGui::TextUnformatted("Test your bindings");
        ImGui::PushTextWrapPos(0.0f);
        mutedText("%s", "Press anything: the pad lights up with what the game would read.");
        mutedText("%s", "Click a binding to change it; right-click to clear it.");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
    if (ImGui::BeginTabBar("##bindings")) {
        const auto pick = [&](int tab) {
            return m_selectBindings == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        };
        if (ImGui::BeginTabItem("Controller", nullptr, pick(0))) {
            m_keyboardBindings = false;
            drawBindingTable(false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keyboard", nullptr, pick(1))) {
            m_keyboardBindings = true;
            drawBindingTable(true);
            ImGui::EndTabItem();
        }
        m_selectBindings = -1;
        ImGui::EndTabBar();
    }

    if (pageFooter("Restore device and stick defaults")) {
        gfx::PadConfig config = m_settings.pad;
        config.keyboardEnabled = def.pad.keyboardEnabled;
        config.controllers = def.pad.controllers;
        config.stickDeadZone = def.pad.stickDeadZone;
        config.triggerThreshold = def.pad.triggerThreshold;
        config.invertCameraX = def.pad.invertCameraX;
        config.invertCameraY = def.pad.invertCameraY;
        m_settings.backStartOpensMenu = def.backStartOpensMenu;
        setPadConfig(config);
        notify("Device and stick settings restored (buttons keep their own Restore button)", Icon::Check);
    }
}

void Overlay::drawBindingTable(bool keyboard) {
    const SDL_GamepadType type = displayedControllerType();
    const float chip = std::floor(ImGui::GetFrameHeight() * 0.92f);
    PadInput hovered = PadInput::Count;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable(keyboard ? "##keys" : "##buttons", 3, flags)) {
        ImGui::TableSetupColumn("Game button", ImGuiTableColumnFlags_WidthStretch, 1.25f);
        ImGui::TableSetupColumn(keyboard ? "Key" : "Button", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Alternative", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color(palette::kTextMuted));
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        for (size_t index = 0; index < gfx::kPadInputCount; ++index) {
            const auto input = static_cast<PadInput>(index);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, chip + em(0.35f));
            ImGui::TableNextColumn();
            ImDrawList *list = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 name = ImGui::CalcTextSize(gfx::padInputName(input));
            ImGui::Dummy(ImVec2(chip + em(0.6f) + name.x, chip));
            drawPadGlyph(list, input, ImVec2(at.x + chip * 0.5f, at.y + chip * 0.5f), chip * 0.9f);
            list->AddText(ImVec2(at.x + chip + em(0.6f), std::floor(at.y + (chip - name.y) * 0.5f)), palette::kText,
                          gfx::padInputName(input));

            for (int slot = 0; slot < static_cast<int>(gfx::kBindingSlots); ++slot) {
                ImGui::TableNextColumn();
                const gfx::PadBinding binding = m_settings.pad.slots(input, keyboard)[slot];
                ImGui::PushID(static_cast<int>(index * gfx::kBindingSlots + slot));
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                const float cellWidth = ImGui::GetContentRegionAvail().x;
                if (ImGui::InvisibleButton("##bind", ImVec2(cellWidth, chip), ImGuiButtonFlags_EnableNav))
                    beginBindingCapture(input, keyboard, slot);
                const bool over = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    gfx::PadConfig config = m_settings.pad;
                    config.slots(input, keyboard)[slot] = {};
                    setPadConfig(config);
                }
                if (over)
                    list->AddRectFilled(ImVec2(cell.x - em(0.2f), cell.y - em(0.12f)),
                                        ImVec2(cell.x + cellWidth, cell.y + chip + em(0.12f)), palette::kHover,
                                        ImGui::GetStyle().FrameRounding);
                drawBindingChip(list, binding, type, ImVec2(cell.x + em(0.2f), cell.y), chip,
                                binding.bound() ? 1.0f : 0.7f);
                const std::string clash = conflictWith(m_settings.pad, input, keyboard, binding);
                if (!clash.empty()) {
                    const float x = cell.x + em(0.2f) + bindingChipWidth(binding, type, chip) + em(0.7f);
                    drawIcon(list, Icon::Warning, ImVec2(x, cell.y + chip * 0.5f), em(0.95f), palette::kWarn);
                }
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted(bindingLabel(binding, type).c_str());
                    if (!clash.empty())
                        ImGui::TextColored(color(palette::kWarn), "Also bound to %s", clash.c_str());
                    mutedText("%s", "Click to change, right-click to clear");
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            if (ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex())
                hovered = input;
        }
        ImGui::EndTable();
    }
    m_hoveredInput = hovered;
    ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
    if (iconButton(keyboard ? "Restore default keys" : "Restore default buttons", Icon::Reset)) {
        gfx::PadConfig config = m_settings.pad;
        const gfx::PadConfig defaults = gfx::PadConfig::defaults();
        (keyboard ? config.keyboard : config.controller) = keyboard ? defaults.keyboard : defaults.controller;
        setPadConfig(config);
        notify(keyboard ? "Keys restored" : "Buttons restored", Icon::Check);
    }
}

void Overlay::drawAbout() {
    centerNextWindow();
    const ImVec2 area = ImGui::GetMainViewport()->WorkSize;
    // Never taller than the window: it scrolls instead.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(area.x - em(2.0f), area.y - em(2.0f)));
    ImGui::SetNextWindowSize(ImVec2(std::min(area.x - em(2.0f), em(31.0f)), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.4f), em(1.2f)));
    if (beginWindow("##about", nullptr, kDialogFlags & ~ImGuiWindowFlags_AlwaysAutoResize)) {
        icon(Icon::Disc, em(3.0f), palette::kGold);
        ImGui::SameLine(0.0f, em(0.9f));
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextUnformatted("Rogue Galaxy Recompiled");
        ImGui::PopFont();
        if (m_host.revision.empty())
            mutedText("Version %s", m_host.version.c_str());
        else
            mutedText("Version %s (%s)", m_host.version.c_str(), m_host.revision.c_str());
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetFrameHeight());
        if (closeButton("##close"))
            m_aboutOpen = false;
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("A native PC port of Rogue Galaxy, made by statically recompiling the PlayStation 2 "
                               "game. It plays from your own copy of the game.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        if (beginSettings("about")) {
            settingRow("Renderer");
            ImGui::Text("SDL GPU (%s)", m_backend.driverName().c_str());
            settingRow("Internal resolution");
            ImGui::Text("%u%s", m_backend.activeResolutionScale(), kTimes);
            const int sdl = SDL_GetVersion();
            settingRow("Built with");
            ImGui::Text("SDL %d.%d.%d, Dear ImGui %s", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
                        SDL_VERSIONNUM_MICRO(sdl), IMGUI_VERSION);
            settingRow("Recompiler");
            ImGui::TextUnformatted("PS2Recomp");
            endSettings();
        }
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        if (iconButton("Project page", Icon::Link, true))
            SDL_OpenURL(kProjectUrl);
        ImGui::SameLine();
        if (iconButton("Report a problem", Icon::Warning))
            SDL_OpenURL(kIssuesUrl);
        ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
        pushScaledFont(0.84f);
        ImGui::PushTextWrapPos(0.0f);
        mutedText("%s", "Rogue Galaxy is a trademark of Sony Interactive Entertainment. This project is not "
                        "affiliated with Sony or Level-5, and includes nothing from the game.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawShortcuts() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImVec2 area = viewport->WorkSize;
    centerNextWindow();
    // Never taller than the window: it scrolls instead.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(area.x - em(2.0f), area.y - em(2.0f)));
    const bool sideBySide = area.x >= em(54.0f);
    ImGui::SetNextWindowSize(ImVec2(std::min(area.x - em(2.0f), sideBySide ? em(52.0f) : em(28.0f)), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.4f), em(1.2f)));
    if (beginWindow("##shortcuts", nullptr, kDialogFlags & ~ImGuiWindowFlags_AlwaysAutoResize)) {
        dialogHeader(Icon::Keyboard, "Shortcuts", &m_shortcutsOpen);
        const bool hasFreeCamera = std::any_of(m_host.gameOptions.begin(), m_host.gameOptions.end(),
                                               [](const GameOption &option) { return option.key == "free_camera"; });
        const auto keyboard = [&] {
            sectionHeader("Keyboard");
            if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_None)) {
                ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, em(9.5f));
                ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
                shortcutAlternatives({{"F1"}}, "Open or close this menu");
                if (canPause())
                    shortcutAlternatives({{"F2"}}, "Pause or resume");
                shortcutAlternatives({{"F3"}}, "Show the frame rate");
                if (canChangeSpeed()) {
                    shortcutAlternatives({{"F4"}}, "Fast-forward on or off");
                    shortcutAlternatives({{"`"}}, "Fast-forward while held");
                }
                if (m_host.cheatsStatus)
                    shortcutAlternatives({{"F5"}}, "Cheats on or off");
                if (hasFreeCamera)
                    shortcutAlternatives({{"F6"}}, "Free camera on or off");
                shortcutAlternatives({{"F8"}}, "Reload the texture pack");
                shortcutAlternatives({{"F11"}, {"Alt", "Enter"}}, "Fullscreen");
                shortcutAlternatives({{"F12"}}, "Screenshot of the game");
                shortcutAlternatives({{"Shift", "F12"}}, "Screenshot with the menu");
                shortcutAlternatives({{"Esc"}}, "Back, in the menu");
                ImGui::EndTable();
            }
        };
        const auto controller = [&] {
            sectionHeader("Controller");
            if (ImGui::BeginTable("##pad", 2, ImGuiTableFlags_None)) {
                ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, em(9.5f));
                ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
                shortcutAlternatives({{"Guide"}}, "Open or close the menu");
                if (m_settings.backStartOpensMenu)
                    shortcutAlternatives({{"Back", "Start"}}, "Open or close the menu");
                shortcutAlternatives({{"L1"}, {"R1"}}, "Previous and next settings page", "/");
                if (canChangeSpeed() && m_host.setTriggerFastForward && m_settings.triggerFastForward) {
                    shortcutAlternatives({{"R2"}}, "Fast-forward while held");
                    shortcutAlternatives({{"L2"}}, "Fast-forward on or off");
                }
                if (hasFreeCamera)
                    shortcutAlternatives({{"Start"}}, "Leave the free camera");
                shortcutAlternatives({{"B / Circle"}}, "Back, in the menu");
                ImGui::EndTable();
            }
        };
        if (sideBySide && ImGui::BeginTable("##columns", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            keyboard();
            ImGui::TableNextColumn();
            controller();
            ImGui::EndTable();
        } else if (!sideBySide) {
            keyboard();
            controller();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::drawStats() {
    const double time = now();
    if (m_statsAt < 0.0 || time - m_statsAt > 0.5) {
        m_statsPrevious = m_stats;
        m_stats = m_backend.stats();
        m_statsSpan = m_statsAt < 0.0 ? 0.0 : time - m_statsAt;
        m_statsAt = time;
    }
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + em(0.8f), viewport->WorkPos.y + em(0.8f)), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing;
    if (!m_menuOpen)
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.0f), em(0.8f)));
    if (beginWindow("##stats", nullptr, flags)) {
        dialogHeader(Icon::Chart, "Renderer", m_menuOpen ? &m_statsOpen : nullptr);
        const double span = m_statsSpan > 0.0 ? m_statsSpan : 1.0;
        const auto rate = [&](uint64_t now, uint64_t before) {
            return m_statsSpan > 0.0 && now >= before ? static_cast<double>(now - before) / span : 0.0;
        };
        if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, em(11.0f));
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, em(8.0f));
            const auto row = [&](const char *name, const char *format, double value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                mutedText("%s", name);
                ImGui::TableNextColumn();
                ImGui::Text(format, value);
            };
            row("Frames shown", "%.0f /s", rate(m_stats.presents, m_statsPrevious.presents));
            row("Draw calls", "%.0f /s", rate(m_stats.drawCalls, m_statsPrevious.drawCalls));
            row("Triangles", "%.1f K/s", rate(m_stats.trianglesDrawn, m_statsPrevious.trianglesDrawn) / 1000.0);
            row("Render passes", "%.0f /s", rate(m_stats.renderPasses, m_statsPrevious.renderPasses));
            row("Texture cache hits", "%.1f %%", m_stats.textureHitRate() * 100.0);
            row("Textures built", "%.0f /s", rate(m_stats.textureBuilds, m_statsPrevious.textureBuilds));
            row("Target resolves", "%.0f /s", rate(m_stats.colorResolves, m_statsPrevious.colorResolves));
            row("Transfers", "%.0f KiB/s", rate(m_stats.transferBytes, m_statsPrevious.transferBytes) / 1024.0);
            row("Pipelines", "%.0f", static_cast<double>(m_stats.pipelinesCreated));
            ImGui::EndTable();
        }
        sectionHeader("Approximations");
        if (ImGui::BeginTable("##approximations", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, em(11.0f));
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, em(8.0f));
            const auto row = [&](const char *name, uint64_t value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                mutedText("%s", name);
                ImGui::TableNextColumn();
                ImGui::TextColored(color(value ? palette::kWarn : palette::kGood), "%llu",
                                   static_cast<unsigned long long>(value));
            };
            row("Inexact blends", m_stats.inexactBlends);
            row("Saturated factors", m_stats.saturatedBlendFactors);
            row("Destination alpha", m_stats.destinationAlphaFactors);
            row("Partial masks", m_stats.partialChannelMasks);
            row("Alpha fail modes", m_stats.alphaFailModes);
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Cheats (Rogue Galaxy: added). The page lists the options whose key starts
// with "cheat" (src/graphics.cpp declares them); the master switch is off at
// every start, and the first time it is turned on a notice asks for a yes.
// ---------------------------------------------------------------------------

void Overlay::drawCheatsPage() {
    const int status = m_host.cheatsStatus ? m_host.cheatsStatus() : 0;
    ImGui::PushTextWrapPos(0.0f);
    mutedText("%s", "Trainer-style helpers. They act only while Cheats on is set, and the game cannot save then.");
    ImGui::PopTextWrapPos();
    if (status > 0) {
        ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
        icon(Icon::Warning, em(1.0f), palette::kWarn);
        ImGui::SameLine(0.0f, em(0.5f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(color(palette::kWarn), "Cheats are on: saving is blocked until they are off (F5).");
        ImGui::PopTextWrapPos();
    }
    drawGameOptions(true);
    ImGui::Dummy(ImVec2(0.0f, em(0.4f)));
    ImGui::PushTextWrapPos(0.0f);
    mutedText("%s", "Experience, money and drop rates are on the Game page, and Developer tools there gives items.");
    ImGui::PopTextWrapPos();
    if (pageFooter("Reset all cheats")) {
        restoreGameDefaults(true);
        notify("Cheats reset", Icon::Check);
    }
}

bool Overlay::toggleCheatsMaster() {
    if (!m_host.cheatsStatus || !m_host.setGameOption)
        return false;
    requestCheatsMaster(m_host.cheatsStatus() == 0);
    return true;
}

void Overlay::requestCheatsMaster(bool on) {
    if (!m_host.setGameOption)
        return;
    if (on) {
        const auto seen = m_settings.gameOptions.find("cheats_notice_seen");
        if (seen == m_settings.gameOptions.end() || !seen->second) {
            // The first time: the notice, which turns them on when accepted.
            m_cheatNoticeOpen = true;
            m_focusSettings = true;
            return;
        }
    }
    m_host.setGameOption("cheats_master", on);
    notify(on ? "Cheats on: saving is blocked until you turn them off (F5)" : "Cheats off: saving works again",
           on ? Icon::Warning : Icon::Info, 4.0);
}

void Overlay::drawCheatNotice() {
    centerNextWindow();
    const ImVec2 area = ImGui::GetMainViewport()->WorkSize;
    // Never taller than the window: it scrolls instead.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(area.x - em(2.0f), area.y - em(2.0f)));
    ImGui::SetNextWindowSize(ImVec2(std::min(area.x - em(2.0f), em(32.0f)), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(1.4f), em(1.2f)));
    if (beginWindow("##cheatnotice", nullptr, kDialogFlags & ~ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetWindowFocus();
        dialogHeader(Icon::Warning, "Turn cheats on?", nullptr);
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Cheats change the game's numbers while you play: health, AP, money, damage. "
                               "They can make the story trivial, and one-hit kills can skip a fight the story "
                               "needs.");
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        ImGui::TextUnformatted("The game cannot save while cheats are on, so a cheated save never reaches your "
                               "memory card. Turn Cheats off (Settings > Cheats, or F5) before you save.");
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        mutedText("%s", "Shown once. Cheats are off every time the game starts.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
        if (iconButton("Turn cheats on", Icon::Check, true)) {
            m_settings.gameOptions["cheats_notice_seen"] = true;
            markDirty();
            saveIfDirty(true);
            m_cheatNoticeOpen = false;
            requestCheatsMaster(true);
        }
        ImGui::SameLine();
        if (iconButton("Keep cheats off", Icon::Close))
            m_cheatNoticeOpen = false;
        // The safe answer is the one a stray press lands on.
        ImGui::SetItemDefaultFocus();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// "Cheats on", in a corner of the picture, as long as the master is on.
void Overlay::drawCheatBadge() {
    const int status = m_host.cheatsStatus ? m_host.cheatsStatus() : 0;
    if (m_host.cheatsRefusals) {
        const unsigned refusals = m_host.cheatsRefusals();
        if (refusals != m_refusalsSeen) {
            m_refusalsSeen = refusals;
            notify("Cheats are on, so the game was not saved. Turn them off (F5) to save.", Icon::Warning, 6.0);
        }
    }
    if (status <= 0)
        return;
    ImVec2 min, max;
    pictureBounds(min, max);
    const float margin = em(0.6f);
    ImGui::SetNextWindowPos(ImVec2(min.x + margin, max.y - margin), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(0.7f), em(0.35f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (beginWindow("##cheatbadge", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        icon(Icon::Warning, em(1.0f), palette::kWarn);
        ImGui::SameLine(0.0f, em(0.45f));
        ImGui::TextUnformatted(status > 1 ? "Cheats on: saving blocked" : "Cheats on, none chosen: saving blocked");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace dq8::ui
