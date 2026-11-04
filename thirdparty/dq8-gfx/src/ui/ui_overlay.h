// The in-game menu, drawn with Dear ImGui over the SDL GPU backend's window:
// a menu bar (F1, or Guide / Back+Start on a controller), the settings window,
// notifications and the frame-rate counter.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "ui/ui_glyphs.h"
#include "ui/ui_settings.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct ImGuiContext;

namespace dq8::ui {

// Screenshots are PNG. SDL_SavePNG arrived in SDL 3.4; with SDL 3.2 they are BMP.
#if SDL_VERSION_ATLEAST(3, 4, 0)
inline constexpr const char *kImageExtension = ".png";
inline bool saveImage(SDL_Surface *surface, const char *path) { return SDL_SavePNG(surface, path); }
#else
inline constexpr const char *kImageExtension = ".bmp";
inline bool saveImage(SDL_Surface *surface, const char *path) { return SDL_SaveBMP(surface, path); }
#endif

// What the menu can ask of the game around it. A control whose function is
// empty is not shown.
// A switch of the game's own, shown on the Game page and kept in the
// settings file's [game] section under `key`, unless not `remembered`
// (Rogue Galaxy: added).
struct GameOption {
    std::string key;
    std::string label;
    std::string help;
    bool defaultValue = false;
    // A number from minimum to maximum instead of a switch, when maximum is
    // the larger, shown as a slider labelled by `format` (printf, its %d the
    // value), between - and + buttons that move it by `step`, and a Default
    // button.
    int minimum = 0;
    int maximum = 0;
    int defaultNumber = 0;
    std::string format;
    int step = 1;
    // A switch the file does not keep: it is off at every start, and the
    // menu shows what HostServices::gameOptionState says, as something
    // besides the menu (a key) can turn it on or off.
    bool remembered = true;
    // Takes effect at the next start: once it differs from what this run
    // started with, the menu says so in red.
    bool restart = false;
    // A number that picks one of these, shown as buttons side by side
    // instead of a slider: the value is minimum + the index (Rogue Galaxy:
    // added).
    std::vector<std::string> choices = {};
    // A button instead of a switch (Rogue Galaxy: added): pressing it calls
    // HostServices::setGameOption(key, true) once; `format` is its label.
    bool action = false;
    bool number() const { return maximum > minimum; }
};

// What the install folder holds, for the Setup and HD page (Rogue Galaxy: added).
struct SetupInfo {
    int textureFiles = 0;  // HD texture pictures found; 0 when there are none
    int movieFiles = 0;    // HD movies found
    int movieTotal = 0;    // and how many the game has
    bool canRun = false;   // the setup program is in the game's folder
    std::string release;   // the version the setup recorded, empty when the folder was not made by it
};

struct HostServices {
    std::string version;
    // The commit it was built from, beside the version in the About window
    // (Rogue Galaxy: added).
    std::string revision;
    std::function<void(float)> setVolume; // 0 to 1
    std::function<void(bool)> setPaused;
    // The game's speed against real time; 0 runs it as fast as it can go.
    std::function<void(double)> setSpeed;
    // The controller's fast-forward on L2 and R2 switched on or off (Rogue
    // Galaxy: added).
    std::function<void(bool)> setTriggerFastForward;
    // Frames the game has finished drawing, for the counter; presents are
    // counted instead when it is empty.
    std::function<uint64_t()> completedRenders;
    // The guest's vsync clock, which DQ8_UI_SCRIPT is timed against, as
    // DQ8_PAD_SCRIPT is; presents are counted instead when it is empty.
    std::function<uint64_t()> guestFrame;
    // The game's own switches, and what turns one on or off (Rogue Galaxy:
    // added).
    std::vector<GameOption> gameOptions;
    // The Game page's sections: a title each, before the option of its key.
    std::vector<std::pair<std::string, std::string>> gameSections;
    // The long explanation of an option, by key, shown as a tooltip on hover
    // and focus; `help` stays one short line (menu UX pass).
    std::vector<std::pair<std::string, std::string>> gameDetails;
    // The keys that open the sections that start folded.
    std::vector<std::string> gameFolded;
    std::function<void(const std::string &key, bool on)> setGameOption;
    std::function<void(const std::string &key, int value)> setGameNumber;
    std::function<bool(const std::string &key)> gameOptionState;
    // Cheats (Rogue Galaxy: added): the options whose key starts with
    // "cheat" make the Cheats page, "cheats_master" being its master switch.
    // The status is 0 for none, 1 for the master on, 2 for the master on
    // with a cheat in use; refusals counts the saves the host turned down.
    std::function<int()> cheatsStatus;
    std::function<unsigned()> cheatsRefusals;
    // The Setup and HD page (Rogue Galaxy: added): what is installed, and a
    // button that opens the setup window. runSetup says whether it started.
    std::function<SetupInfo()> setupInfo;
    std::function<bool()> runSetup;
};

// Game: Rogue Galaxy's own switches (added). In the order of the page list:
// the ones used most first, Cheats last.
enum class SettingsPage : uint8_t { Display, Sound, Controls, Game, Interface, Setup, Cheats, Count };
inline constexpr const char *kSettingsPageNames[] = {"Display", "Sound", "Controls", "Game", "Interface", "Setup and HD", "Cheats"};

class Overlay final : public gfx::SdlGpuOverlay {
public:
    Overlay(gfx::SdlGpuBackend &backend, Settings settings, std::string settingsPath, HostServices host);
    ~Overlay() override;
    Overlay(const Overlay &) = delete;
    Overlay &operator=(const Overlay &) = delete;

    // `format` is the texture format render() draws into.
    bool initialize(SDL_Window *window, SDL_GPUTextureFormat format, std::string &error);
    // Pushes every setting to the backend, the window and the host (a game
    // switch not `remembered` is the host's own and is not pushed), and the
    // first time notes the switches this run starts with.
    void applySettings();

    bool handleEvent(const SDL_Event &event) override;
    void render(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width,
                uint32_t height) override;
    bool wantsFrames() override;
    bool capturesInput() const override;

    const Settings &settings() const { return m_settings; }
    // A game option changed elsewhere, such as the game's own settings
    // screen (Rogue Galaxy: added): passed to the host and kept as the
    // menu's own changes are.
    void setGameOptionValue(const std::string &key, bool on);
    void setGameNumberValue(const std::string &key, int value);
    // Display > Borders, changed there too (Rogue Galaxy: added).
    void setSides(gfx::SdlGpuSides sides);
    void setMenuOpen(bool open);
    void openSettings(SettingsPage page);
    // Opens one of the menu bar's menus at the next frame, by its label.
    void showMenu(const char *label);
    void openAbout();
    void openShortcuts();
    // Picks the Controls page's controller or keyboard tab.
    void showBindings(bool keyboard);
    void notify(std::string text, Icon icon = Icon::Info, double seconds = 3.0);
    void setUserPaused(bool paused);
    void setFastForward(bool on);
    // Fast-forward for as long as a key or button is held, whatever F4 has
    // it at; let go, the speed is F4's again (Rogue Galaxy: added).
    void setFastForwardHeld(bool held);
    void toggleFastForward() { setFastForward(!m_fastForward); }
    // A speed for this run instead of the settings' (RG_FAST_FORWARD_SPEED),
    // not saved; below 0 for none (Rogue Galaxy: added).
    void setFastForwardSpeedOverride(float speed);
    // Waits for the next key or controller input and binds it.
    void beginBindingCapture(gfx::PadInput input, bool keyboard, int slot);
    // Waiting for one, so a key of the host's own is not taken for it too
    // (Rogue Galaxy: added).
    bool capturingBinding() const { return m_capture.active; }
    void takeScreenshot(bool withMenu);
    std::string screenshotFolder() const;

private:
    struct Toast {
        std::string text;
        Icon icon = Icon::Info;
        double startedAt = 0.0;
        double duration = 3.0;
    };
    struct Capture {
        bool active = false;
        gfx::PadInput input = gfx::PadInput::Count;
        bool keyboard = false;
        int slot = 0;
        double startedAt = 0.0;
    };
    struct ScriptStep {
        uint64_t frame = 0u;
        double seconds = -1.0; // wall time instead, for steps a pause would hold back
        std::string command;
        std::vector<std::string> arguments;
        bool done = false;
    };

    // DQ8_UI_SCRIPT (ui_script.cpp): menu actions on the guest frame clock,
    // for tests and screenshots of the menu over the running game.
    void loadScript();
    void runScript();
    void runStep(const ScriptStep &step, uint64_t frame);

    // Event side.
    bool hotkey(const SDL_KeyboardEvent &key);
    bool gamepadShortcut(const SDL_GamepadButtonEvent &button);
    bool captureEvent(const SDL_Event &event);
    bool backOut();
    void toggleMenu(bool fromController);

    // Frame side.
    void updateScale();
    void updateNavigation();
    void drawFrame();
    void drawMenuBar();
    void drawStatus();
    void drawSettingsWindow();
    void drawDisplayPage();
    void drawSoundPage();
    void drawControlsPage();
    void drawBindingTable(bool keyboard);
    void drawGamePage();
    void drawGameOptions(bool cheats);
    // Every option of the Game page, or of the Cheats page, back to its default.
    void restoreGameDefaults(bool cheats);
    void drawCheatsPage();
    void drawCheatNotice();
    void drawCheatBadge();
    // F5, or the page's switch: the master, with the one-time notice first.
    bool toggleCheatsMaster();
    void requestCheatsMaster(bool on);
    void drawInterfacePage();
    void drawSetupPage();
    void drawCapturePrompt();
    void drawAbout();
    void drawShortcuts();
    void drawStats();
    void pictureBounds(ImVec2 &min, ImVec2 &max) const;
    void drawFps();
    void drawSpeedBadge();
    void drawPauseBanner();
    void drawToasts();
    void updateCursor();
    void sampleFrameRate();
    void collectSavedScreenshots();

    // Settings side.
    void markDirty();
    void saveIfDirty(bool force);
    void rememberWindow(const SDL_WindowEvent &event);
    void applyVolume();
    void applyPause();
    // A game switch's value: the settings file's, or the option's default;
    // for one not `remembered`, what HostServices::gameOptionState says.
    bool gameOption(const GameOption &option) const;
    // A `restart` switch changed since this run started.
    bool waitsForRestart(const GameOption &option) const;
    // A game number's value, the same way, within the option's range.
    int gameNumber(const GameOption &option) const;
    void setFullscreen(bool fullscreen);
    void setPadConfig(const gfx::PadConfig &config);
    // Asks for an internal resolution and saves it. Asking for the active one
    // replaces a request still queued, which cancels it.
    void requestScale(uint32_t scale);
    // Internal resolution Auto (Rogue Galaxy: added): the scale for the
    // window's height now, and asking for it when it changed (resize,
    // another monitor, fullscreen), with a toast. RG_GS_SCALE=auto forces it.
    uint32_t autoScaleFor(int heightPixels) const;
    void updateAutoScale();
    void setAutoScale(bool on);
    bool m_autoForced = false;
    int m_autoHeight = -1;
    uint32_t m_autoAsked = 0u;
    double m_autoChangedAt = 0.0;
    bool pausing() const;
    bool canPause() const { return static_cast<bool>(m_host.setPaused); }
    bool canChangeSpeed() const { return static_cast<bool>(m_host.setSpeed); }
    std::string speedLabel(float speed) const;
    // Rogue Galaxy: added. Either F4's fast-forward or a held one is on.
    bool fastForwarding() const { return m_fastForward || m_fastForwardHeld; }
    // The speed fast-forward runs at: the override's, or the settings'.
    float fastForwardRate() const;
    // Hands the host the speed the two make.
    void applySpeed();
    void setFastForwardSpeed(float speed);
    // Which controller the Controls page draws, what its buttons are called
    // and the line that says why (Rogue Galaxy: added).
    struct ShownPad {
        PadBody body = PadBody::PlayStation;
        SDL_GamepadType labels = SDL_GAMEPAD_TYPE_PS4;
        std::string caption;
    };
    ShownPad shownPad() const;
    // The pad as the Controls page lights it; RG_FAKE_PAD_HELD adds inputs in tests.
    gfx::PadSnapshot padPreview() const;
    double now() const;

    gfx::SdlGpuBackend &m_backend;
    Settings m_settings;
    std::string m_settingsPath;
    HostServices m_host;
    // The switches as this run started with them, for those that wait for a
    // restart (Rogue Galaxy: added).
    std::map<std::string, bool> m_startOptions;
    bool m_startCaptured = false;
    SDL_Window *m_window = nullptr;
    ImGuiContext *m_context = nullptr;
    bool m_platformReady = false;
    bool m_rendererReady = false;
    float m_appliedScale = 0.0f;

    bool m_menuOpen = false;
    bool m_settingsOpen = false;
    bool m_focusSettings = false;
    SettingsPage m_page = SettingsPage::Display;
    // The Setup and HD page: read again every two seconds while it is shown.
    SetupInfo m_setupInfo;
    double m_setupAt = -10.0;
    bool m_aboutOpen = false;
    bool m_shortcutsOpen = false;
    bool m_statsOpen = false;
    std::string m_showMenu;
    bool m_windowFocused = true;
    bool m_userPaused = false;
    bool m_hostPaused = false;
    bool m_fastForward = false;
    bool m_fastForwardHeld = false;
    float m_speedOverride = -1.0f;
    bool m_gamepadNavHeld = false;
    bool m_keyboardBindings = false;
    int m_selectBindings = -1;
    gfx::PadInput m_hoveredInput = gfx::PadInput::Count;
    Capture m_capture;
    uint32_t m_pendingScale = 0u;
    double m_pendingScaleSince = 0.0;

    std::vector<Toast> m_toasts;
    std::vector<std::pair<SDL_JoystickID, std::string>> m_controllerNames;

    double m_rateStart = 0.0;
    uint64_t m_rateRenders = 0u;
    uint64_t m_presents = 0u;
    float m_fps = 0.0f;
    // Below the counter when they share a corner.
    float m_toastTop = 0.0f;
    float m_fpsLeftBottom = 0.0f;
    std::array<float, 64> m_fpsHistory{};
    size_t m_fpsHistoryNext = 0u;

    gfx::SdlGpuStats m_stats{};
    gfx::SdlGpuStats m_statsPrevious{};
    double m_statsAt = -1.0;
    double m_statsSpan = 0.0;
    double m_lastMouseMotion = 0.0;
    int m_uiScaleEdit = 0;

    bool m_dirty = false;
    double m_dirtyAt = 0.0;

    bool m_closePopups = false;
    bool m_cheatNoticeOpen = false;
    unsigned m_refusalsSeen = 0u;
    double m_refusalToastAt = -100.0;
    std::vector<ScriptStep> m_script;
    double m_scriptStart = 0.0;

    struct Saver {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex m_savedMutex;
    std::vector<std::pair<bool, std::string>> m_saved;
    std::vector<Saver> m_savers;
};

} // namespace dq8::ui
