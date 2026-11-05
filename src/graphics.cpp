#include "graphics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "debug/gs_census.h"
#include "debug/gs_timing.h"
#include "debug/sampler.h"
#include "debug/stall_watch.h"
#include "depth_reads.h"
#include "flip_pacing.h"
#include "battle_rates.h"
#include "camera_view.h"
#include "cheats.h"
#include "dev_tools.h"
#include "draw_distance.h"
#include "free_camera.h"
#include "hotkeys.h"
#include "instant_text.h"
#include "hd_text.h"
#include "map_chests.h"
#include "movie_hd.h"
#include "loading_screen.h"
#include "opening.h"
#include "quick_scenes.h"
#include "scene_timing.h"
#include "settings_pages.h"
#include "setup_info.h"
#include "shop_prices.h"
#include "vif1_thread.h"
#include "ultrawide.h"
#include "widescreen.h"
#include "ps2_runtime.h"
#include "stick_response.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_threaded_backend.h"

#if defined(RG_HAS_SDLGPU)
#include <SDL3/SDL.h>

#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_native_iop.h"
#include "runtime/ps2_pad_host.h"
#include "ui/ui_overlay.h"
#include "gfx/backends/sdlgpu/sdlgpu_replacements.h"

#include <cctype>
#include <filesystem>
#include <vector>

// stb_image, as the runtime's raylib builds it.
extern "C" unsigned char *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp);
extern "C" void stbi_image_free(void *retval_from_stbi_load);
extern "C" int stbi_write_png(const char *filename, int w, int h, int comp, const void *data, int stride_in_bytes);
#endif

namespace rg::graphics
{
namespace
{

std::string env(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return (value && *value) ? value : fallback;
}

#if defined(RG_HAS_SDLGPU)
bool g_hasMenu = false;
dq8::ui::Overlay *g_menu = nullptr;
PS2Runtime *g_runtime = nullptr;

// The game's speed: the menu's (fast-forward, 0 unpaced) times a quick
// scene's (src/quick_scenes.h); the main thread's alone. A movie plays at
// 1 whatever they say: its sound keeps the audio device's pace (the IOP
// runs in step with it), and the picture would run away from it.
double g_menuSpeed = 1.0;
double g_sceneSpeed = 1.0;
bool g_movie = false;

void applySpeed()
{
    if (g_runtime)
    {
        const double speed = g_movie ? 1.0 : g_menuSpeed > 0.0 ? g_menuSpeed * g_sceneSpeed : 0.0;
        // A line at each change, for scripted checks of the speed keys.
        static double logged = 1.0;
        if (speed != logged)
        {
            logged = speed;
            std::fprintf(stderr, "[rg] speed %.2fx (menu %.2f, scene %.2f%s)%s\n", speed, g_menuSpeed, g_sceneSpeed,
                         g_movie ? ", movie" : "", speed == 0.0 ? ", unpaced" : "");
        }
        g_runtime->eeScheduler().setHostSpeed(speed);
    }
}

void followQuickScenes()
{
    if (!g_runtime)
    {
        return;
    }
    const double scene = rg::quickscenes::speed(g_runtime->memory().getRDRAM());
    const bool movie = rg::timing::moviePlaying();
    if (scene != g_sceneSpeed || movie != g_movie)
    {
        g_sceneSpeed = scene;
        g_movie = movie;
        applySpeed();
    }
}

// What the menu's game options switch, and the game's Settings screen's
// pages of them too.
void applyGameOption(const std::string &key, bool on)
{
    rg::settingspages::noteOption(key, on);
    if (key == "cheats_master")
        rg::cheats::setMaster(on);
    else if (key.rfind("cheat_", 0) == 0)
    {
        // The Cheats page: switches, and buttons that call with true once.
        const std::string name = key.substr(6);
        if (name == "max_money")
        {
            if (on)
                rg::cheats::fire(name);
        }
        else
            rg::cheats::setSwitch(name, on);
    }
    else if (key == "sixty_fps")
        rg::timing::setSixtyFpsPreferred(on);
    else if (key == "widescreen")
        rg::widescreen::setEnabled(on);
    else if (key == "skip_opening")
        rg::opening::setSkip(on);
    else if (key == "instant_loading_recap")
        rg::loading::setInstantRecap(on);
    else if (key == "instant_text")
        rg::instanttext::setEnabled(on);
    else if (key == "hd_movies")
        rg::hdmovie::setEnabled(on);
    else if (key == "map_chests")
        rg::mapchests::setEnabled(on);
    else if (key == "dev_tools")
        rg::devtools::setEnabled(on);
    else if (key == "free_camera")
        rg::freecamera::setEnabled(on);
    else if (key == "faster_camera")
        rg::cameraview::setFasterTurn(on);
    else if (key == "fast_transporter")
        rg::quickscenes::setEnabled(rg::quickscenes::Scene::Transporter, on);
    else if (key == "fast_revelation")
        rg::quickscenes::setEnabled(rg::quickscenes::Scene::Revelation, on);
#if defined(RG_HAS_SDLGPU)
    else if (key == "texture_replacements")
        rg::hdtext::setReplacementsEnabled(on);
#endif
}

void applyGameNumber(const std::string &key, int value)
{
    rg::settingspages::noteNumber(key, value);
    if (key == "cheat_money")
        rg::cheats::setNumber("money", value);
    else if (key == "field_of_view")
        rg::cameraview::setFieldOfView(value);
    else if (key == "camera_distance")
        rg::cameraview::setDistance(value);
    else if (key == "draw_distance")
        rg::drawdistance::setPercent(value);
    else if (key == "encounter_rate")
        rg::battlerates::setEncounterRate(value);
    else if (key == "battle_experience")
        rg::battlerates::setExperience(value);
    else if (key == "battle_money")
        rg::battlerates::setMoney(value);
    else if (key == "item_drops")
        rg::battlerates::setItemDrops(value);
    else if (key == "shop_prices")
        rg::shopprices::setPercent(value);
    else if (key == "action_gauge")
        rg::battlerates::setActionGauge(value);
    else if (key == "widescreen_menus" && !std::getenv("RG_WIDESCREEN_MENUS"))
        rg::widescreen::setMenusPillarboxed(value == 1);
    else if (key == "hud_area" && !std::getenv("RG_HUD_AREA"))
        rg::ultrawide::setHudArea(value);
    else if (key == "hud_placement" && !std::getenv("RG_HUD_PLACEMENT"))
        rg::ultrawide::setHudPlacement(value);
    else if (key == "ultrawide_fov" && !std::getenv("RG_ULTRAWIDE_FOV"))
        rg::ultrawide::setFovCap(value);
#if defined(RG_HAS_SDLGPU)
    else if (key == "button_prompts")
        rg::hdtext::setPrompts(value);
    else if (key == "font_style")
        rg::hdtext::setModernFont(value == 1);
    else if (key == "texture_memory" && !std::getenv("RG_TEXTURE_BUDGET_MB"))
        dq8::gfx::GsReplacementStore::instance().setBudgetMegabytes(static_cast<uint32_t>(value));
#endif
}

// F9 and F10 presses, counted as SDL queues their key-down events. pump()
// read the keyboard state once a presented frame, and a quick tap between two
// of them went unseen, most of all in a drop, when frames are long: the
// player's marks of 2026-10-20 never reached the log.
std::atomic<unsigned> g_f9Presses{0u};
std::atomic<unsigned> g_f10Presses{0u};
std::atomic<unsigned> g_f7Presses{0u};
// F6 turns the free camera on or off.
std::atomic<unsigned> g_f6Presses{0u};
// F8 reads the texture pack folder again (hot reload).
std::atomic<unsigned> g_f8Presses{0u};
// The longest gap between two pump() calls since the stats line last printed
// it. The presenter loop calls pump several times a frame, so this is not the
// frame time (the backend's "longest frame" is) but the longest the main
// thread was held up: by a wait for the GS, a present, a load.
std::atomic<int64_t> g_longestPumpGapNanos{0};

std::string mainStallReport()
{
    const int64_t longest = g_longestPumpGapNanos.exchange(0, std::memory_order_relaxed);
    char text[48];
    std::snprintf(text, sizeof(text), " | main stall %.1f ms", static_cast<double>(longest) / 1e6);
    return text;
}

bool SDLCALL countMarkKeys(void *, SDL_Event *event)
{
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat)
    {
        if (event->key.scancode == SDL_SCANCODE_F9)
        {
            g_f9Presses.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (event->key.scancode == SDL_SCANCODE_F10)
        {
            g_f10Presses.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (event->key.scancode == SDL_SCANCODE_F7)
        {
            g_f7Presses.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (event->key.scancode == SDL_SCANCODE_F8 && !(g_menu && g_menu->capturingBinding()))
        {
            g_f8Presses.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (event->key.scancode == SDL_SCANCODE_F6 && !(g_menu && g_menu->capturingBinding()))
        {
            // Not while the menu waits for a key to bind, which F6 is not
            // (ui_overlay.cpp's reservedKey).
            g_f6Presses.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    return true;
}

// Called once per frame by the runtime when the backend owns the window:
// services SDL events (the menu takes its own first) and hands the host pad
// to the runtime.
bool pump(void *userData)
{
    auto *backend = static_cast<dq8::gfx::SdlGpuBackend *>(userData);
    debug::notePresenterLoop();
    static const bool watching = SDL_AddEventWatch(&countMarkKeys, nullptr);
    (void)watching;
    {
        static auto previous = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const int64_t gap = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();
        previous = now;
        int64_t longest = g_longestPumpGapNanos.load(std::memory_order_relaxed);
        while (gap > longest &&
               !g_longestPumpGapNanos.compare_exchange_weak(longest, gap, std::memory_order_relaxed))
        {
        }
    }
    if (!backend->pumpEvents())
    {
        return false;
    }
    // The shape the picture takes with Widescreen on, for the game's camera:
    // under Auto the window's.
    int windowWidth = 0, windowHeight = 0;
    if (SDL_GetWindowSizeInPixels(backend->window(), &windowWidth, &windowHeight) && windowWidth > 0 &&
        windowHeight > 0)
    {
        rg::widescreen::setAspect(static_cast<float>(dq8::gfx::sdlGpuDisplayAspect(
            backend->displayOptions(), 512u, 448u, static_cast<uint32_t>(windowWidth),
            static_cast<uint32_t>(windowHeight), 1)));
    }
    // The menu has F11 and Alt+Enter; without it, F11 still toggles.
    static bool f11WasDown = false;
    const bool f11Down = !g_hasMenu && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F11];
    if (f11Down && !f11WasDown)
    {
        SDL_Window *window = backend->window();
        SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
    }
    f11WasDown = f11Down;
    // F9 writes "[mark] N s" to the log: pressed when a drop is felt, it
    // puts the moment next to the [gfx] lines of that second.
    static const auto markStart = std::chrono::steady_clock::now();
    for (unsigned n = g_f9Presses.exchange(0u, std::memory_order_relaxed); n > 0u; --n)
    {
        // On stderr with the stats lines, so a mark cannot cut one in two.
        std::fprintf(stderr, "[mark] %.1f s\n",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - markStart).count());
        // With the profile launcher, the mark also opens a profile window.
        debug::requestProfileWindow();
    }
    // F7 logs the next GS frame with RG_GS_CAPTURE (debug\RogueGalaxy-capture.bat).
    for (unsigned n = g_f7Presses.exchange(0u, std::memory_order_relaxed); n > 0u; --n)
    {
        std::fprintf(stderr, "[mark] F7: frame capture requested\n");
        debug::requestFrameCapture();
        rg::timing::requestStateReport();
    }
    // F10 writes the stall watchdog's report now (the guest threads, their
    // waits, the host stacks), for a freeze that keeps flipping.
    if (g_f10Presses.exchange(0u, std::memory_order_relaxed) != 0u)
    {
        std::fprintf(stderr, "[mark] F10: stall report requested\n");
        debug::requestStallReport();
    }
    if (g_f6Presses.exchange(0u, std::memory_order_relaxed) % 2u != 0u)
    {
        rg::freecamera::setEnabled(!rg::freecamera::enabled());
    }
    // Changes made on the game's Settings screen (its pages are there with
    // the menu alone), kept as the menu's own.
    for (rg::settingspages::Change change; g_menu && rg::settingspages::takeChange(change);)
    {
        if (change.key == "sides")
            g_menu->setSides(static_cast<dq8::gfx::SdlGpuSides>(change.value));
        else if (change.number)
            g_menu->setGameNumberValue(change.key, change.value);
        else
            g_menu->setGameOptionValue(change.key, change.value != 0);
    }
    // Display > Borders, which the pages show too, as the menu has it.
    if (g_menu)
    {
        static int sidesWas = -1;
        if (const int sides = static_cast<int>(g_menu->settings().display.sides); sides != sidesWas)
        {
            sidesWas = sides;
            rg::settingspages::noteNumber("sides", sides);
        }
    }
    followQuickScenes();
    // Button prompts on Auto follow the controller playing: a PlayStation
    // one, or not. Checked about once a second.
    static unsigned padCheck = 0u;
    if (++padCheck % 60u == 1u)
    {
        bool playStation = false;
        std::string padName;
        for (const dq8::gfx::ControllerInfo &controller : backend->padInput().controllers())
        {
            if (controller.active)
            {
                playStation = controller.type == SDL_GAMEPAD_TYPE_PS3 || controller.type == SDL_GAMEPAD_TYPE_PS4 ||
                              controller.type == SDL_GAMEPAD_TYPE_PS5;
                padName = controller.name;
                break;
            }
        }
        rg::hdtext::notePlayStationPad(playStation, padName);
        // Which buttons the prompts show, and why, once each time it changes.
        static std::string promptsShown;
        if (std::string choice = rg::hdtext::promptsChoice(); choice != promptsShown)
        {
            promptsShown = choice;
            std::printf("[font] %s\n", choice.c_str());
            if (g_menu)
            {
                g_menu->notify(choice, dq8::ui::Icon::Info);
            }
        }
    }
#if defined(RG_HAS_SDLGPU)
    // F8: the texture pack read again, and every ten seconds a line on what
    // it holds while one is in use (Rogue Galaxy: added).
    {
        auto &store = dq8::gfx::GsReplacementStore::instance();
        static unsigned f8Seen = 0u;
        if (const unsigned f8 = g_f8Presses.load(std::memory_order_relaxed); f8 != f8Seen)
        {
            f8Seen = f8;
            store.reload();
            std::fprintf(stderr, "[texrep] F8: reading the texture pack again\n");
            if (g_menu)
            {
                g_menu->notify("Texture pack: reading it again", dq8::ui::Icon::Folder);
            }
        }
        static auto lastSummary = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - lastSummary > std::chrono::seconds(10))
        {
            lastSummary = now;
            if (store.fileCount() != 0u || store.learning())
            {
                std::fprintf(stderr, "[texrep] %s\n", store.summary().c_str());
            }
        }
    }
#endif
    // The free camera turned on or off, by F6, either menu or Start: a toast.
    static bool freeCameraWas = false;
    if (const bool freeCamera = rg::freecamera::enabled(); freeCamera != freeCameraWas)
    {
        freeCameraWas = freeCamera;
        if (g_menu)
        {
            g_menu->notify(freeCamera ? "Free camera on: Start or F6 to leave" : "Free camera off",
                           dq8::ui::Icon::Camera);
        }
    }
    uint32_t held = 0u, pressed = 0u, sticks = 0u;
    backend->hostPadState(held, pressed, sticks);
    // L2 and R2 drive fast-forward (src/hotkeys.h), but not the free
    // camera's, which moves with them.
    rg::hotkeys::filterPad(held, pressed, rg::freecamera::enabled());
    ps2PadPublishHostState(held, pressed, rg::pad::shapeSticks(sticks));
    return true;
}

// The saved window size, or 80% of the display's height at 4:3, the shape
// the game is shown at, so the picture fills it.
std::pair<uint32_t, uint32_t> initialWindowSize(const dq8::ui::Settings &settings)
{
    if (settings.windowWidth >= 320 && settings.windowHeight >= 240)
    {
        return {static_cast<uint32_t>(settings.windowWidth), static_cast<uint32_t>(settings.windowHeight)};
    }
    SDL_Rect usable{0, 0, 1280, 960};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    const uint32_t height = static_cast<uint32_t>(std::max(448.0f, static_cast<float>(usable.h) * 0.8f));
    return {height * 4u / 3u, height};
}

// The image the menu's Display > Borders > Image shows beside a 4:3 picture:
// the first PNG, JPG, BMP or TGA by name in borders\ (beside the disc
// folder, in the game's working directory). It is read each time that
// choice is made, so an image put there while the game runs is taken by
// choosing it again.
bool loadBorderImage(uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)
{
    std::vector<std::filesystem::path> images;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("borders", error))
    {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (entry.is_regular_file(error) && (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                                             extension == ".bmp" || extension == ".tga"))
        {
            images.push_back(entry.path());
        }
    }
    std::sort(images.begin(), images.end());
    for (const std::filesystem::path &path : images)
    {
        int w = 0, h = 0, channels = 0;
        if (unsigned char *pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4))
        {
            width = static_cast<uint32_t>(w);
            height = static_cast<uint32_t>(h);
            rgba.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            stbi_image_free(pixels);
            std::printf("[rg] sides: %s, %dx%d\n", path.string().c_str(), w, h);
            return true;
        }
        std::fprintf(stderr, "[rg] sides: could not read %s\n", path.string().c_str());
    }
    std::fprintf(stderr, "[rg] sides: no image in borders\\, the blurred picture instead\n");
    return false;
}

// The saved place and maximized state. A place whose top edge is on no
// display now (one unplugged since) opens centred instead.
dq8::gfx::SdlGpuWindowPlacement initialWindowPlacement(const dq8::ui::Settings &settings)
{
    dq8::gfx::SdlGpuWindowPlacement placement;
    placement.maximized = settings.maximized;
    if (!settings.windowPlaced)
    {
        return placement;
    }
    const SDL_Point titleBar{settings.windowX + 64, settings.windowY};
    int count = 0;
    if (SDL_DisplayID *displays = SDL_GetDisplays(&count))
    {
        for (int index = 0; index < count && !placement.placed; ++index)
        {
            SDL_Rect bounds{};
            placement.placed = SDL_GetDisplayBounds(displays[index], &bounds) && SDL_PointInRect(&titleBar, &bounds);
        }
        SDL_free(displays);
    }
    placement.x = settings.windowX;
    placement.y = settings.windowY;
    return placement;
}

// DQ8-Recompiled's in-game menu over the window: display, sound, controls
// and the frame-rate counter (thirdparty/dq8-gfx/src/ui). What it asks of
// the game goes through HostServices.
std::unique_ptr<dq8::ui::Overlay> createMenu(PS2Runtime &runtime, dq8::gfx::SdlGpuBackend &backend,
                                             dq8::ui::Settings settings, const std::string &settingsPath)
{
    dq8::ui::HostServices host;
    host.version = RG_APP_VERSION;
    host.revision = RG_REVISION;
    // RG_MUTE=1 keeps the game silent whatever the menu's volume says.
    const bool mute = env("RG_MUTE", "0") != "0";
    host.setVolume = [mute](float volume) { ps2_native_iop::setVolume(mute ? 0.0f : volume); };
    // The sound driver runs in step with the audio device, so stopping the
    // device stops the IOP along with the EE's clock.
    host.setPaused = [&runtime](bool paused) {
        runtime.eeScheduler().setHostPaused(paused);
        ps2_native_iop::setPaused(paused);
    };
    g_runtime = &runtime;
    host.setSpeed = [](double speed) {
        g_menuSpeed = speed;
        applySpeed();
    };
    // L2 and R2 for fast-forward, the menu's setting or, for this run,
    // RG_FAST_FORWARD_TRIGGERS=0 or 1 whatever it says.
    host.setTriggerFastForward = [](bool on) {
        const std::string triggers = env("RG_FAST_FORWARD_TRIGGERS", "");
        rg::hotkeys::setTriggersEnabled(triggers.empty() ? on : triggers != "0");
    };
    rg::hotkeys::setHandlers({
        [](bool held) {
            if (g_menu)
                g_menu->setFastForwardHeld(held);
        },
        [] {
            if (g_menu)
                g_menu->toggleFastForward();
        },
    });
    // The game's flips: frames it finished, not presents, which go on while
    // it is paused.
    host.completedRenders = [] { return guestFlips(); };
    host.guestFrame = [] { return ps2PadCurrentGuestFrame(); };
    // The game's own switches, on the menu's Game page, in its sections. The
    // help is one short line; the long explanation is in gameDetails below,
    // shown as a tooltip. Menu UX pass: sections and order regrouped (the
    // keys, and so settings.ini, are unchanged).
    host.gameOptions = {
        // Camera and view
        {"sixty_fps", "60 frames per second",
         "Twice the game's own 30. Takes effect at the next start.", true, 0, 0, 0, "", 1, true, true},
        {"widescreen", "Widescreen", "A wider view that fills the window; menus stay 4:3.", false},
        {"widescreen_menus", "Widescreen menus", "Full-screen menus: 4:3 with borders, or stretched wide.", false, 0,
         1, 1, "", 1, true, false, {"Stretched", "4:3 pillarboxed"}},
        {"hud_area", "HUD area", "How wide the HUD spreads on a window wider than 16:9.", false, 0, 2, 0, "", 1, true,
         false, {"16:9", "21:9", "Full width"}},
        {"hud_placement", "HUD placement", "Where the HUD sits inside that area.", false, 0, 1, 0, "", 1, true, false,
         {"Centre", "Edges"}},
        {"ultrawide_fov", "Ultrawide field of view", "How far the view may widen on a window wider than 16:9.", false, 0,
         2, 0, "", 1, true, false, {"No limit", "Up to 21:9", "Up to 16:9"}},
        {"field_of_view", "Field of view", "How wide the camera sees; the game's is 52 degrees.", false, 40, 90, 52,
         "%d degrees", 2},
        {"camera_distance", "Camera distance", "How far the camera stays from the party; 100% is the game's.", false,
         70, 160, 100, "%d%%", 5},
        {"faster_camera", "Faster camera rotation", "The stick turns the camera twice as fast.", false},
        {"draw_distance", "Draw distance", "How far scenery is drawn; from the next map loaded.", false, 100, 400,
         100, "%d%%", 25},
        {"free_camera", "Free camera (F6)", "Fly the camera while the party stands still. Off at every start.",
         false, 0, 0, 0, "", 1, false},
        // Convenience
        {"skip_opening", "Skip the opening", "Boot goes straight to the title screen.", false},
        {"instant_loading_recap", "Show the loading recap at once",
         "The story so far appears whole when a game loads.", false},
        {"instant_text", "Instant text", "Dialogue a page at a time, not letter by letter.", false},
        {"map_chests", "Chests on the map", "Unopened chests show on the explored parts of the map.", false},
        {"fast_transporter", "Fast transporters", "Transporter warps play up to twice as fast.", false},
        {"fast_revelation", "Fast Revelation Flow", "Revelation Flow animations play up to twice as fast.", false},
        // Battles and rewards
        {"encounter_rate", "Encounter rate", "How often random battles come; 0% turns them off.", false, 0, 300, 100,
         "%d%%", 10},
        {"battle_experience", "Experience from battles", "Experience a won battle gives; 100% is the game's.", false,
         0, 500, 100, "%d%%", 25},
        {"battle_money", "Money from battles", "Money defeated enemies leave; 100% is the game's.", false, 0, 500,
         100, "%d%%", 25},
        {"item_drops", "Item drops", "How likely enemies are to drop items; 100% is the game's.", false, 0, 1000,
         100, "%d%%", 50},
        {"shop_prices", "Shop prices", "What shops ask for their items; 100% is the game's.", false, 10, 200, 100,
         "%d%%", 10},
        {"action_gauge", "Action gauge speed", "How fast the action gauge refills in battle.", false, 25, 400, 100,
         "%d%%", 25},
        // Text and HD content
        {"button_prompts", "Button prompts", "Which buttons the game's text shows.", false, 0, 2, 2, "", 1, true,
         false, {"PlayStation", "Xbox", "Auto"}},
        {"font_style", "Font", "Original, or a sharp modern font (Roboto, or your own).", false, 0, 1, 0, "", 1, true,
         false, {"Original", "Modern"}},
        {"hd_movies", "HD movies",
         "Upscaled 1080p cutscenes: " + std::to_string(rg::hdmovie::fileCount()) + " of " +
             std::to_string(rg::hdmovie::kMovieCount) + " found.",
         rg::hdmovie::fileCount() > 0},
        {"texture_replacements", "Texture replacements", "Off shows every texture as the game has it.", true},
        // Advanced
        {"texture_memory", "Texture pack memory", "Video memory an HD texture pack's pictures may take.", false, 512,
         8192, 2048, "%d MB", 512},
        {"dev_tools", "Developer tools", "Debug shortcuts the developers left in the game.", false},
        // The Cheats page (docs/CHEATS.md): the master is off at every start
        // (not remembered), the rest are remembered but act only with it on.
        {"cheats_master", "Cheats on (F5)", "Off at every start. The game cannot save while this is on.", false, 0,
         0, 0, "", 1, false},
        {"cheat_hp", "Infinite HP", "Nobody in the party takes damage.", false},
        {"cheat_ap", "Infinite AP", "AP stays full, so abilities cost nothing.", false},
        {"cheat_gauge", "Full action gauge", "The action gauge stays full: every action is ready.", false},
        {"cheat_onehit", "One-hit kills", "Every enemy, bosses included, goes down to its first hit.", false},
        {"cheat_items", "Items are not used up", "Items used from the bag stay in it.", false},
        {"cheat_money_hold", "Keep money at an amount", "Money is put back to the amount below whenever it differs.",
         false},
        {"cheat_money", "Amount", "The money kept, up to the game's 99,999,999.", false, 0, 99999999, 99999999,
         "%d", 1000000},
        {"cheat_max_money", "Money", "Sets money to the game's maximum, once.", false, 0, 0, 0, "Set to maximum", 1,
         false, false, {}, true},
    };
    host.gameDetails = {
        {"sixty_fps",
         "The game runs at 60 frames a second instead of its own 30, its logic still at the PS2's pace; movies "
         "and the loading screen keep 30. Takes effect the next time the game starts."},
        {"widescreen",
         "The game draws a wider view, and the HUD over the scene and the movies fit it; with Display > Aspect "
         "ratio on Auto the picture fills the window, whatever its shape, once a game is loaded. Menus and text "
         "are drawn for 4:3: see Widescreen menus."},
        {"widescreen_menus",
         "With Widescreen on, the full-screen menus (the main menu with items, equipment, status and Settings; "
         "save and transporter menus; factory, insect and ship menus), which the game draws for 4:3, are shown at "
         "4:3 with Display > Borders around them. Stretched shows them as wide as the window, as before."},
        {"hud_area",
         "On a window wider than 16:9, the HUD (the message windows, prompts, minimap, compass and battle gauges) is "
         "drawn in a box of this shape in the middle: 16:9 as the game was made for, 21:9, or the whole width. The "
         "picture behind it always fills the window. Nothing changes at 16:9 or narrower."},
        {"hud_placement",
         "Centre keeps the HUD as one block in the middle of its area. Edges keeps the HUD's left and right parts "
         "(the compass, the minimap, the battle gauges) at the sides of the screen and the messages and prompts "
         "in the middle."},
        {"ultrawide_fov",
         "No limit lets the view widen with the window, so 32:9 shows a very wide scene. Up to 21:9 or 16:9 stops "
         "the sideways angle there and shows a little less above and below instead; 16:9 is how the game looks on "
         "a 16:9 window."},
        {"field_of_view",
         "How wide the camera sees while exploring and in battle, in degrees across a 4:3 picture (the game's is "
         "52); a wide picture adds to it. Scenes keep their own camera."},
        {"camera_distance", "How far the camera stays from the party, against the game's (100%)."},
        {"faster_camera",
         "The stick turns the camera twice as fast, exploring, in battle and in the first-person view. Otherwise "
         "it turns at the game's speed, at 60 frames a second as at 30."},
        {"draw_distance",
         "How far away the scenery and the objects are still drawn, against the game's (100%), the fog thinned "
         "in step. Most of Salgin has no limit to raise; the open areas gain more. From the next map loaded."},
        {"free_camera",
         "The camera leaves the party, which stands still, and the controller moves it: the left stick moves the "
         "point it looks at (Square held, faster; L2 held, up and down), the right stick turns it around that "
         "point and tilts it (R2 held, nearer and farther). Turn it off, or press F6 or Start, to play again."},
        {"skip_opening", "At boot, the Sony and Level-5 logos and the opening movie give way to the title screen at once."},
        {"instant_loading_recap",
         "When a game loads, the story so far appears whole instead of line by line, and Press X comes as soon as "
         "it has loaded."},
        {"instant_text",
         "Dialogue and messages appear a page at a time instead of letter by letter, as the game itself does "
         "where a scene asks for it; the pauses written into the text stay."},
        {"map_chests",
         "The chests not yet opened show on the maps in the parts you have explored, as the game shows them once "
         "every save point of a planet is activated. Nothing is saved: the planet's own completion still comes."},
        {"fast_transporter", "A transporter's warp plays at up to twice the speed. The music and the sounds keep their pace."},
        {"fast_revelation",
         "Items going into a Revelation Flow cell, the zoom, the glow and the ability won, play at up to twice "
         "the speed."},
        {"encounter_rate",
         "How often random battles come, against the game's (100%); 0% turns them off. The story's battles stay."},
        {"battle_experience",
         "The experience a won battle gives, against the game's (100%), up to the game's 9999 a battle."},
        {"battle_money",
         "The money defeated enemies leave, against the game's (100%). Selling, chests and rewards stay as they are."},
        {"item_drops",
         "How likely defeated enemies are to leave each of their items, against the game's (100%): most have a 2 "
         "to 10% chance, which 1000% makes 20 to 100%; 0% for none. A battle still gives three kinds of items at "
         "most, as the game does."},
        {"shop_prices",
         "What shops ask for their items, against the game's (100%). Below 100% nothing sells for more than it "
         "costs, so buying and selling back makes no money."},
        {"action_gauge", "How fast the party's action gauge fills again in battle, against the game's (100%)."},
        {"button_prompts",
         "Xbox goes by position: Cross is A, Circle B, Square X, Triangle Y, L1 and R1 LB and RB, L2 and R2 LT "
         "and RT. Auto shows PlayStation buttons for a PlayStation controller and Xbox ones otherwise. Buttons "
         "painted into pictures (help pages, the loading screen) follow too."},
        {"font_style",
         "Modern draws the text in Roboto, or in the first .ttf or .otf in the fonts folder, sharp at the internal "
         "resolution, and lines wrap to its letter widths; the menus' numbers and the compass letters too. Text "
         "already on screen changes at its next page."},
        {"hd_movies",
         "The cutscenes play from upscaled 1080p video files, one a movie in the HD movie folder (RG_HD_MOVIES, "
         "else textures/SCUS-97490/movies, as 00/TITLE01.mkv, 01/01_01.mkv ...), with the game's own sound and "
         "timing. A movie without a file, or one this PC cannot decode in time, plays as the game has it. On by "
         "default when the folder holds files."},
        {"texture_replacements",
         "Off shows every texture as the game has it: no Xbox buttons, no modern font, and no texture pack from "
         "textures/SCUS-97490/replacements. F8 reads the pack's folder again, for a pack being worked on."},
        {"texture_memory",
         "How much video memory a texture pack's pictures may take; the least recently seen give way to new "
         "ones, and as much again of main memory keeps pictures ready. Raise it if textures stay blurry after a "
         "scene has loaded and the graphics card has room."},
        {"dev_tools",
         "Main menu: R3 shows every item's icon, R1 + R3 a list of them, and Cross gives one of the one picked. "
         "Help pages and info list: R1 + Select unlocks every entry. Bestiary: Select fills it. Revelation Flow: "
         "R2 + Triangle gives one of each item the picked cell asks for. Paused in a scene: Triangle + R1 + R2 "
         "ends it, also one the game would not let you skip, which can leave the story out of order."},
        {"cheats_master",
         "Turns the cheats below on or off, F5 too. Off at every start. While it is on the game cannot save, so a "
         "save with cheated numbers in it cannot happen; the first time, a notice asks for a yes."},
        {"cheat_hp",
         "HP never goes down, and tops up to full. A character who is already down stays down."},
        {"cheat_onehit", "Anything that is not a party member goes down to its first hit, bosses included."},
        {"cheat_items",
         "An item used from the bag in the field or in battle stays in the bag. Selling, discarding, combining "
         "and the Revelation Flow still use items up."},
        {"cheat_money_hold",
         "The party's money is put back to the amount below whenever it differs, so spending costs nothing and "
         "earning adds nothing."},
        {"cheat_max_money", "Sets the party's money to the game's maximum, 99,999,999, once. Needs Cheats on."},
    };
    host.gameSections = {
        {"sixty_fps", "Camera and view"},
        {"skip_opening", "Convenience"},
        {"encounter_rate", "Battles and rewards"},
        {"button_prompts", "Text and HD content"},
        {"texture_memory", "Advanced"},
        {"cheats_master", "Master switch"},
        {"cheat_hp", "Battle"},
        {"cheat_items", "Items and money"},
    };
    // These sections start folded; the others start open.
    host.gameFolded = {"texture_memory"};
    // The game's own Settings screen shows them on pages of its own.
    for (const dq8::ui::GameOption &option : host.gameOptions)
    {
        if (option.number())
        {
            rg::settingspages::setRange(option.key, option.minimum, option.maximum, option.step,
                                        option.defaultNumber);
        }
    }
    // And Display > Borders: Black, Blurred, Image.
    rg::settingspages::setRange("sides", 0, 2, 1, 0);
    host.setGameOption = &applyGameOption;
    host.gameOptionState = [](const std::string &key) {
        return (key == "free_camera" && rg::freecamera::enabled()) || (key == "cheats_master" && rg::cheats::master());
    };
    host.cheatsStatus = [] { return rg::cheats::master() ? (rg::cheats::active() ? 2 : 1) : 0; };
    host.cheatsRefusals = [] { return rg::cheats::refusedSaves(); };
    // The Setup and HD page: what the folder holds, and the setup window.
    host.setupInfo = [] {
        dq8::ui::SetupInfo info;
        info.textureFiles = static_cast<int>(dq8::gfx::GsReplacementStore::instance().fileCount());
        info.movieFiles = rg::hdmovie::fileCount();
        info.movieTotal = rg::hdmovie::kMovieCount;
        info.canRun = rg::setupinfo::setupProgramPresent();
        info.release = rg::setupinfo::recordedRelease();
        return info;
    };
    host.runSetup = [] { return rg::setupinfo::openSetup(); };
    host.setGameNumber = &applyGameNumber;
    auto menu = std::make_unique<dq8::ui::Overlay>(backend, std::move(settings), settingsPath, std::move(host));
    std::string error;
    if (!menu->initialize(backend.window(), backend.windowTextureFormat(), error))
    {
        std::fprintf(stderr, "[rg] in-game menu unavailable: %s\n", error.c_str());
        return nullptr;
    }
    menu->applySettings();
    // RG_FAST_FORWARD_SPEED=N: fast-forward's speed for this run (1.5, 2, 3,
    // 4, 8, ...; 0 or "max" unpaced), not saved; picking one in the menu
    // takes over.
    if (const std::string speed = env("RG_FAST_FORWARD_SPEED", ""); !speed.empty())
    {
        const double value = speed == "max" ? 0.0 : std::strtod(speed.c_str(), nullptr);
        menu->setFastForwardSpeedOverride(static_cast<float>(std::clamp(value, 0.0, 16.0)));
        std::printf("[rg] fast-forward speed for this run: %s\n", value > 0.0 ? speed.c_str() : "unpaced");
    }
    backend.setOverlay(menu.get());
    return menu;
}
#endif

} // namespace

Setup::Setup(PS2Runtime &runtime)
{
#if defined(RG_HAS_SDLGPU)
    if (env("RG_GS", "sdlgpu") == "sdlgpu")
    {
        const bool noWindow = env("RG_GS_NO_WINDOW", "0") != "0";
        // RG_SHOW_FPS=1 turns the menu's counter on; without a window of
        // ours the backend's readout in the title stands in, which it reads
        // under its DQ8-Recompiled name.
        const bool showFps = env("RG_SHOW_FPS", "0") != "0";
        if (showFps && noWindow)
        {
            _putenv("DQ8_GFX_SHOW_FPS=1");
        }
        std::string error;
        std::unique_ptr<dq8::gfx::SdlGpuBackend> sdl = dq8::gfx::createSdlGpuBackend(error);
        if (!sdl)
        {
            std::fprintf(stderr, "[rg] SDL GPU backend unavailable (%s); using the software rasterizer\n",
                         error.c_str());
            return;
        }
        // The menu's settings, then the variables on top for this run only:
        // they go to the backend, not into the settings the menu saves, so a
        // test run's scale does not become the player's.
        dq8::ui::Settings settings;
        const std::string settingsPath = dq8::ui::defaultSettingsPath();
        if (!dq8::ui::loadSettings(settingsPath, settings))
        {
            std::fprintf(stderr, "[rg] could not read %s; using default settings\n", settingsPath.c_str());
        }
        uint32_t runScale = settings.resolutionScale;
        // "auto": the menu's Auto for this run (the overlay follows the window).
        if (const std::string scale = env("RG_GS_SCALE", ""); !scale.empty() && scale != "auto")
        {
            runScale = std::clamp(static_cast<uint32_t>(std::strtoul(scale.c_str(), nullptr, 10)), 1u, 8u);
        }
        // The game leaves a border of its frame black or half drawn (a column
        // each side, the top row and the last two), which the television's
        // overscan hid, and at a raised scale its half-pixel effects leave a
        // sliver beyond it: two GS pixels off each edge. RG_CROP=0 shows it.
        settings.display.crop = static_cast<uint32_t>(std::strtoul(env("RG_CROP", "2").c_str(), nullptr, 10));
        dq8::gfx::SdlGpuDisplayOptions runDisplay = settings.display;
        if (const std::string mode = env("RG_PRESENT_MODE", ""); !mode.empty())
        {
            runDisplay.presentMode = mode == "vsync"       ? SDL_GPU_PRESENTMODE_VSYNC
                                     : mode == "immediate" ? SDL_GPU_PRESENTMODE_IMMEDIATE
                                                           : SDL_GPU_PRESENTMODE_MAILBOX;
        }
        if (showFps)
        {
            settings.showFps = true;
        }
        sdl->setResolutionScale(runScale);
        // The VIF1 thread's share of the wall on every stats line, beside the
        // worker's: an F9 mark then says which thread was saturated.
        dq8::gfx::setStatsExtraLine([] { return rg::vif1::busyReport() + mainStallReport(); });
        dq8::gfx::setPresentObserver(&debug::noteFrameTime);
        // Aspect ratio Auto follows the Widescreen option.
        sdl->setGameWidescreenQuery([] { return rg::widescreen::wide() ? 1 : 0; });
        sdl->setSideImageLoader(&loadBorderImage);
        // Texture replacements: packs in textures/SCUS-97490/replacements
        // (in the working directory, beside borders), read by stb_image.
        dq8::gfx::GsReplacementStore::instance().setImageLoader(
            [](const std::string &path, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba) {
                int w = 0, h = 0, channels = 0;
                unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
                if (!pixels)
                {
                    return false;
                }
                width = static_cast<uint32_t>(w);
                height = static_cast<uint32_t>(h);
                rgba.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
                stbi_image_free(pixels);
                return true;
            });
        dq8::gfx::GsReplacementStore::instance().setImageWriter(
            [](const std::string &path, uint32_t width, uint32_t height, const uint8_t *rgba) {
                return stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba,
                                      static_cast<int>(width * 4u)) != 0;
            });
        // RG_TEXTURE_PACK=dir: another pack folder for this run.
        const char *packFolder = std::getenv("RG_TEXTURE_PACK");
        dq8::gfx::GsReplacementStore::instance().setPackDirectory(packFolder && *packFolder
                                                                      ? packFolder
                                                                      : "textures/SCUS-97490/replacements");
        if (noWindow)
        {
            std::printf("[rg] GS: sdlgpu, presenting through the runtime window\n");
        }
        else
        {
            const auto [width, height] = initialWindowSize(settings);
            std::string windowError;
            if (sdl->openWindow("Rogue Galaxy Recompiled", width, height, windowError,
                                initialWindowPlacement(settings)))
            {
                runtime.setExternalPresenter(&pump, sdl.get());
                if (!sdl->supportsPresentMode(settings.display.presentMode))
                {
                    settings.display.presentMode = SDL_GPU_PRESENTMODE_VSYNC;
                }
                if (!sdl->supportsPresentMode(runDisplay.presentMode))
                {
                    runDisplay.presentMode = SDL_GPU_PRESENTMODE_VSYNC;
                }
                m_menu = createMenu(runtime, *sdl, settings, settingsPath);
                g_hasMenu = m_menu != nullptr;
                g_menu = m_menu.get();
                // The game's Settings screen shows the menu's options.
                rg::settingspages::setEnabled(g_hasMenu);
                // After the menu applied the saved options: this run's.
                sdl->setDisplayOptions(runDisplay);
                m_window = sdl.get();
            }
            else
            {
                std::fprintf(stderr, "[rg] could not open the SDL window (%s); presenting through the runtime\n",
                             windowError.c_str());
            }
        }
        std::printf("[rg] GS: sdlgpu (%s), internal scale %ux%s\n", sdl->driverName().c_str(),
                    sdl->resolutionScale(), m_menu ? ", menu on F1" : "");
        m_backend = std::move(sdl);
        m_name = "sdlgpu";
    }
#else
    (void)runtime;
#endif
}

Setup::~Setup()
{
#if defined(RG_HAS_SDLGPU)
    // The runtime still holds the backend (it is destroyed after this), but
    // the presenter has stopped: take the menu off it before the menu goes.
    if (m_window && m_menu)
    {
        m_window->setOverlay(nullptr);
    }
    g_menu = nullptr;
    m_menu.reset();
#endif
}

void Setup::install(PS2Runtime &runtime)
{
    std::unique_ptr<GSRasterBackend> backend = std::move(m_backend);
    if (!backend)
    {
        backend = std::make_unique<GSCpuBackend>();
    }
    // In 3D scenes the software rasterizer was more than half of the game
    // thread's time; behind the queue it runs beside the game instead.
    if (env("RG_GS_WORKER", "1") != "0")
    {
        // The queue counts an upload's bytes in full, and the game uploads
        // about 4 MB of textures a frame: at the framework's 4 MiB the VIF1
        // thread spent a sixth of its time waiting for the worker to drain.
        constexpr size_t kQueueBytes = 16u * 1024u * 1024u;
        backend = std::make_unique<GSThreadedBackend>(debug::timeGsBackend(std::move(backend), "worker"),
                                                      kQueueBytes);
    }
    backend = paceOnFlips(serveDepthReadsLocally(
        debug::censusGsBackend(debug::timeGsBackend(std::move(backend), "frontend"))));
    runtime.gs().setRasterBackend(std::move(backend));
    std::printf("[rg] GS backend: %s%s\n", m_name, env("RG_GS_WORKER", "1") != "0" ? " on its own thread" : "");
}

} // namespace rg::graphics
