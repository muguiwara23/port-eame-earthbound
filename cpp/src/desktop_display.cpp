#include "eb/desktop_display.hpp"
#include "eb/app_icon.hpp"
#include "eb/asset_store.hpp"
#include "eb/asset_cache.hpp"
#include "eb/debug_panel.hpp"
#include "eb/display_settings.hpp"
#include "eb/controller_preferences.hpp"
#include "eb/frame_pacer.hpp"
#include "eb/frame_presenter.hpp"
#include "eb/game_session.hpp"
#include "eb/launch_options.hpp"
#include "eb/session_storage.hpp"
#include "desktop_input.hpp"
#include "porteame/ios.h"
#include "porteame/touch.h"
#include "porteame/touch_gles2.h"
#include "generated_assets.hpp"
#include "gl_es.hpp"
#include <SDL3/SDL.h>
#define SDL_MAIN_NOIMPL // SDL_SetMainReady only; the entry point is desktop_application.cpp's
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#ifdef __linux__
#include <cerrno>
#include <sys/resource.h>
#endif

namespace eb {
namespace {
constexpr int width = 256;
constexpr int height = 224;
} // namespace
struct DesktopDisplay::Impl {
  public:
    double frame_rate() const {
        int interval = 0;
        if (settings_.high_frame_rate() || !SDL_GL_GetSwapInterval(&interval) || interval == 0)
            return eb::FramePacer::frame_rate;
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
        if (!mode || mode->refresh_rate <= 0)
            return eb::FramePacer::frame_rate;
        return eb::FramePacer::rate_for_refresh(int(std::lround(mode->refresh_rate)), settings_.variable_refresh);
    }
    double presentation_rate() const {
        double rate = settings_.frame_limit;
        if (settings_.variable_refresh && vsync_requested_) {
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
            if (mode && mode->refresh_rate > 0)
                rate = rate > 0 ? std::min(rate, mode->refresh_rate * .99) : mode->refresh_rate * .99;
        }
        return rate;
    }
    void update_swap_interval() {
        const int wanted =
            vsync_requested_ && (!settings_.high_frame_rate() || settings_.variable_refresh) ? 1 : 0;
        if (wanted == requested_swap_interval_)
            return;
        requested_swap_interval_ = wanted;
        if (!SDL_GL_SetSwapInterval(wanted))
            std::cerr << (wanted ? "Vsync unavailable; using the frame clock: "
                                 : "Immediate presentation unavailable; the driver may limit FPS: ")
                      << SDL_GetError() << '\n';
    }
    // Own SDL/window/GL resources as one lifetime. settings is borrowed from main
    // and survives the DesktopDisplay so edits can be saved after the last game frame.
    explicit Impl(const LaunchOptions &options, eb::DisplaySettings &settings)
        : vsync_requested_(options.vsync), settings_(settings),
          controller_preferences_(options.config.empty() ? "" : options.config + ".controllers"),
          controller_settings_(load_controller_settings(controller_preferences_)) {
#ifndef SDL_PLATFORM_IOS // there SDL_main has started SDL
        SDL_SetMainReady();
#endif
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
            throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
        initialized_ = true;
#ifdef __linux__
        // Foreground simulation produces audio on this thread. Competing
        // background builds/searches can otherwise delay an 8 ms tick by tens
        // of milliseconds. This is a best-effort, non-realtime adjustment to
        // our thread only; it needs no elevation and never changes game clocks.
        errno = 0;
        const int old_nice = getpriority(PRIO_PROCESS, 0);
        if (errno == 0 && old_nice > -10 && setpriority(PRIO_PROCESS, 0, -10) == 0)
            previous_nice_ = old_nice;
#endif
        try {
            // Port/eame: OpenGL ES 3.0 (iOS's), running the ES 2.0 calls of gl_es.hpp.
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
            SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
            const auto initial_width = settings.render_width(width * options.scale, height * options.scale);
            int window_width = initial_width * options.scale, window_height = height * options.scale;
            SDL_Rect available{};
            if (settings.widescreen && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &available)) {
                // A large aspect setting should still open within the desktop.
                // This changes host window size, not the virtual game viewport.
                const double fit = std::min({1.0, double(std::max(256, available.w - 48)) / window_width,
                                             double(std::max(224, available.h - 80)) / window_height});
                window_width = int(window_width * fit);
                window_height = int(window_height * fit);
            }
            SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef SDL_PLATFORM_IOS
            // Port/eame: a phone game in landscape from its first frame (the
            // hint must come before the window), over the whole screen.
            SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
            flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
#endif
            window_ = SDL_CreateWindow("Phase Distorter", window_width, window_height, flags);
            if (!window_)
                throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
            eb::set_app_icon(window_);
            context_ = SDL_GL_CreateContext(window_);
            if (!context_)
                throw std::runtime_error(std::string("SDL_GL_CreateContext: ") + SDL_GetError());
            if (options.start_fullscreen && !SDL_SetWindowFullscreen(window_, true))
                std::cerr << "Fullscreen unavailable: " << SDL_GetError() << '\n';
            update_swap_interval();
            presenter_ = std::make_unique<eb::FramePresenter>();
            presenter_->prepare_scene_effects();
            if (settings_.crt_filter) presenter_->prepare_crt();
            input_ = std::make_unique<DesktopInput>();
            input_->configure(controller_settings_);
            touch_ = std::make_unique<TouchPad>();
            std::cout << "OpenGL: " << glGetString(GL_VERSION) << " / " << glGetString(GL_RENDERER) << '\n';
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Impl() { cleanup(); }
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    eb::GameAssets import_assets(std::string &path, const std::string &error,
                                 const std::string &default_directory, bool lock_game, eb::GameVersion game,
                                 const std::string &found_rom = {}) {
        // This temporary panel is destroyed before the in-game panel is created.
        // No CPU/APU/bus exists yet, so invalid input cannot start partial gameplay.
        eb::AssetImportPanel importer(window_, context_);
        if (!error.empty())
            importer.set_error(error);
        std::optional<std::string> found;
        if (!found_rom.empty()) {
            // Port/eame: show the screen saying so (one frame) before the import's
            // work, so iOS sees the app draw and the player sees what it does.
            importer.set_status("Importing " + found_rom + " ...");
            int dw{}, dh{};
            SDL_GetWindowSizeInPixels(window_, &dw, &dh);
            glViewport(0, 0, dw, dh);
            glClearColor(0.035f, 0.043f, 0.065f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            importer.draw();
            SDL_GL_SwapWindow(window_);
            found = found_rom;
        }
        while (!importer.exit_requested()) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                importer.process_event(event);
            }
            if (importer.exit_requested())
                return {};
            int dw{}, dh{};
            SDL_GetWindowSizeInPixels(window_, &dw, &dh);
            glViewport(0, 0, dw, dh);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.035f, 0.043f, 0.065f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            importer.draw();
            SDL_GL_SwapWindow(window_);
            auto rom = importer.take_import_request();
            if (!rom)
                rom = std::exchange(found, std::nullopt);
            if (rom) {
                try {
                    const auto profile = eb::identify_rom(native_path(*rom), eb::asset_profiles());
                    if (lock_game && profile.version != game)
                        throw std::runtime_error(
                            "That ROM belongs to a different game than the selected --game option.");
                    if (!default_directory.empty())
                        path = default_directory + game_basename(profile.version) + ".ebpak";
                    // Reload through the same validation path used on later runs;
                    // successful extraction alone does not bypass pack verification.
                    eb::import_assets(native_path(*rom), native_path(path), profile.layout);
                    return eb::load_game_assets(native_path(path), eb::asset_profiles());
                } catch (const std::exception &failure) {
                    importer.set_error(failure.what());
                }
            }
            SDL_Delay(10);
        }
        return {};
    }

    void start_panel(bool visible, const std::string &game_title, const std::string &preferences,
                     eb::GameVersion game, const std::string &custom_assets) {
        game_title_ = game_title;
        cache_directory_ = native_path(preferences);
        game_ = game;
        custom_assets_ = custom_assets;
        SDL_SetWindowTitle(window_, ("Phase Distorter - " + game_title).c_str());
        panel_ = std::make_unique<eb::DebugPanel>(window_, context_);
        panel_->set_controller_settings(controller_settings_);
        panel_->set_visible(visible);
        refresh_asset_cache();
    }

    bool fullscreen() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }

    std::optional<eb::GameVersion> take_game_request() { return std::exchange(next_game_, std::nullopt); }
    std::optional<eb::SaveStateSnapshotRequest> take_snapshot_action() {
        return panel_ ? panel_->take_snapshot_action() : std::nullopt;
    }
    void adopt_debug(const eb::GameDebug &debug) {
        if (panel_) panel_->set_game_settings(debug.settings());
        debug_snapshot_ = debug.snapshot();
    }
    void set_snapshot_state(std::vector<eb::SaveStateSnapshotInfo> snapshots, std::string status, bool available) {
        snapshots_ = std::move(snapshots);
        snapshot_status_ = std::move(status);
        snapshots_available_ = available;
    }
    void update_debug(eb::GameDebug &debug) {
        if (!panel_)
            return;
        debug.configure(panel_->game_settings());
        if (auto action = panel_->take_game_action())
            debug.request(*action);
        debug_snapshot_ = debug.snapshot();
    }

    unsigned render_width() const {
        int drawable_width{}, drawable_height{};
        SDL_GetWindowSizeInPixels(window_, &drawable_width, &drawable_height);
        return settings_.render_width(drawable_width,
                                      std::max(1, drawable_height - menu_height_pixels(drawable_height)));
    }
    bool wants_register_diagnostics() const { return panel_ && panel_->visible(); }

    bool events(std::uint16_t &buttons) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (pe_ios_is_background(&event)) {
                if (!wait_for_foreground())
                    return false;
                continue;
            }
            if (touch_event(event))
                continue;
            const bool was_visible = panel_ && panel_->visible();
            const bool consumed = panel_ && panel_->process_event(event);
            if (panel_ && !was_visible && panel_->visible())
                refresh_asset_cache();
            if (event.type == SDL_EVENT_DROP_FILE)
                continue;
            // Fullscreen is a global host shortcut even while the panel captures
            // gameplay keys, so handle it before honoring the consumed flag.
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F11 && !event.key.repeat) {
                toggle_fullscreen();
                continue;
            }
            if (consumed)
                continue;
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE))
                return false;
            input_->process_device_event(event);
        }
        if (pe_touch_take_menu(touch_->touch) && panel_)
            panel_->set_visible(!panel_->visible());
        if (panel_ && panel_->take_close_request())
            return false; // the game loop ends: the save and settings are written
        // Scripted input is deterministic test/game input, independent of the UI.
        // Only physical keyboard/controller input is captured by the open panel.
        if (panel_ && panel_->captures_game_input())
            return true;
        buttons |= input_->buttons();
        buttons |= std::uint16_t(pe_touch_buttons(touch_->touch));
        return true;
    }

    void present(const SessionDiagnostics &session, PresentationPicture picture,
                 const std::string &capture = {}) {
        int drawable_width = 0, drawable_height = 0;
        SDL_GetWindowSizeInPixels(window_, &drawable_width, &drawable_height);
        const int top_inset = menu_height_pixels(drawable_height);
        const double aspect = picture.fixed_aspect > 0
            ? picture.fixed_aspect
            : settings_.target_aspect(drawable_width, std::max(1, drawable_height - top_inset));
        if (!picture.scene ||
            !presenter_->draw_scene(*picture.scene, drawable_width, drawable_height, aspect, top_inset))
            presenter_->draw(picture.pixels, int(picture.width), height, drawable_width, drawable_height,
                             aspect, top_inset);
        presenter_->apply_crt(settings_.crt_filter);
        draw_touch_controls(drawable_width, drawable_height, aspect, top_inset);
        gles::drain_errors("after the touch controls");
        if (panel_) {
            // Copy observations rather than exposing mutable hardware to the UI.
            // Formatting full register strings is only needed for a visible panel.
            eb::DebugDiagnostics diagnostics;
            diagnostics.game_title = game_title_;
            diagnostics.frames = session.frames;
            diagnostics.master_clocks = session.master_clocks;
            diagnostics.cpu_instructions = session.cpu_instructions;
            diagnostics.spc_instructions = session.audio_cpu_instructions;
            diagnostics.audio_frames = session.audio_frames;
            diagnostics.source_width = int(session.source_width);
            diagnostics.drawable_width = drawable_width;
            diagnostics.drawable_height = drawable_height;
            diagnostics.fullscreen = fullscreen();
            diagnostics.game_debug = debug_snapshot_;
            diagnostics.machine_debug_available = session.machine_debug_available;
            diagnostics.cache = cache_info_;
            diagnostics.snapshots = snapshots_;
            diagnostics.snapshot_status = snapshot_status_;
            diagnostics.snapshots_available = snapshots_available_;
            if (!custom_assets_.empty())
                diagnostics.custom_asset_status = "Using a custom asset pack: " + custom_assets_ +
                                                  ". Cache controls below only manage the default packs. "
                                                  "Switching uses the selected game's default cache.";
            if (panel_->visible()) {
                diagnostics.cpu_state = session.cpu_state;
                diagnostics.spc_state = session.audio_cpu_state;
                diagnostics.controller = input_->controller_snapshot();
            }
            const bool was_visible = panel_->visible();
            panel_->draw(settings_, diagnostics);
            if (panel_->controller_settings() != controller_settings_) {
                controller_settings_ = panel_->controller_settings();
                input_->configure(controller_settings_);
                try {
                    store_controller_settings(controller_preferences_, controller_settings_);
                } catch (const std::exception &error) {
                    std::cerr << "Could not save controller settings: " << error.what() << '\n';
                }
            }
            if (!was_visible && panel_->visible())
                refresh_asset_cache();
            handle_panel_action();
        }
        gles::drain_errors("after the settings panel");
        // Read back before swap so captures contain this frame and its overlay.
        if (!capture.empty())
            presenter_->capture().write_ppm(capture);
        SDL_GL_SwapWindow(window_);
    }

    void set_background_handler(std::function<void()> handler) { background_handler_ = std::move(handler); }

  private:
    // Port/eame: the kit's on-screen SNES controller (port-eame-kit,
    // porteame/touch.h), drawn with OpenGL ES over the frame. Fingers come in
    // window-relative (0-1) coordinates; the controller works in drawable
    // pixels.
    //
    // Its bits are JOY1's, as DesktopInput's keyboard map sets them (A 7, X 6,
    // L 5, R 4, B 15, Y 14, Select 13, Start 12, the D-pad 11-8), with one
    // change for thumbs: EarthBound's quick Talk/Check is the SNES L button and
    // its A opens the command menu, while Y does nothing in the game. So the
    // on-screen A sends L (talk or check at once; L also confirms in menus)
    // and the on-screen Y sends A (the command menu). L itself still sends L.
    struct TouchPad {
        PeTouch *touch = pe_touch_create(PE_TOUCH_SNES);
        PeTouchGL *gl = pe_touch_gl_create();
        TouchPad() {
            if (!touch || !gl) {
                pe_touch_gl_destroy(gl);
                pe_touch_destroy(touch);
                throw std::runtime_error("Touch controls: out of memory");
            }
            constexpr std::uint32_t snes_a = 1u << 7, snes_l = 1u << 5;
            pe_touch_map(touch, PE_TOUCH_UP, 1u << 11);
            pe_touch_map(touch, PE_TOUCH_DOWN, 1u << 10);
            pe_touch_map(touch, PE_TOUCH_LEFT, 1u << 9);
            pe_touch_map(touch, PE_TOUCH_RIGHT, 1u << 8);
            pe_touch_map(touch, PE_TOUCH_EAST, snes_l);   // A: talk or check
            pe_touch_map(touch, PE_TOUCH_SOUTH, 1u << 15); // B
            pe_touch_map(touch, PE_TOUCH_NORTH, 1u << 6);  // X
            pe_touch_map(touch, PE_TOUCH_WEST, snes_a);   // Y: the command menu
            pe_touch_map(touch, PE_TOUCH_L1, snes_l);
            pe_touch_map(touch, PE_TOUCH_R1, 1u << 4);
            pe_touch_map(touch, PE_TOUCH_SELECT, 1u << 13);
            pe_touch_map(touch, PE_TOUCH_START, 1u << 12);
        }
        ~TouchPad() {
            pe_touch_gl_destroy(gl); // with the GL context still current
            pe_touch_destroy(touch);
        }
        TouchPad(const TouchPad &) = delete;
        TouchPad &operator=(const TouchPad &) = delete;
    };
    bool touch_event(const SDL_Event &event) {
        int dw = 0, dh = 0;
        SDL_GetWindowSizeInPixels(window_, &dw, &dh);
        switch (event.type) {
        case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED: {
            const PeFinger kind = event.type == SDL_EVENT_FINGER_DOWN     ? PE_FINGER_DOWN
                                  : event.type == SDL_EVENT_FINGER_MOTION ? PE_FINGER_MOVE
                                                                          : PE_FINGER_UP;
            pe_touch_finger(touch_->touch, kind, std::uint64_t(event.tfinger.fingerID), event.tfinger.x * dw,
                            event.tfinger.y * dh);
            return true; // the panel takes touches through the mouse events SDL makes of them
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_MOTION: {
            // Those mouse events are the controls' where a control is.
            const bool button = event.type != SDL_EVENT_MOUSE_MOTION;
            if ((button ? event.button.which : event.motion.which) != SDL_TOUCH_MOUSEID)
                return false;
            const float density = pe_ios_density(window_);
            const float x = (button ? event.button.x : event.motion.x) * density,
                        y = (button ? event.button.y : event.motion.y) * density;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                touch_mouse_on_controls_ = pe_touch_covers(touch_->touch, x, y);
            const bool ours = touch_mouse_on_controls_;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
                touch_mouse_on_controls_ = false;
            return ours;
        }
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            pe_touch_other_input(touch_->touch);
            return false;
        default:
            return false;
        }
    }
    void draw_touch_controls(int drawable_width, int drawable_height, double aspect, int top_inset) {
        // Laid out every frame (nothing changes while nothing moved), so the
        // first touch already lands on the right places.
        pe_touch_block(touch_->touch, panel_ && panel_->visible());
        // The picture's columns, as FramePresenter::begin_draw places it.
        const int game_height = drawable_height - std::clamp(top_inset, 0, std::max(0, drawable_height - 1));
        const double view_height = std::min(double(game_height), double(drawable_width) / aspect);
        const int view_width = std::min(drawable_width, int(std::lround(view_height * aspect)));
        const int picture_left = (drawable_width - view_width) / 2;
        PeTouchLayout layout{};
        layout.width = drawable_width;
        layout.height = drawable_height;
        layout.picture_left = picture_left;
        layout.picture_right = picture_left + view_width;
        layout.density = pe_ios_density(window_);
        pe_ios_safe_area(window_, &layout.inset_left, &layout.inset_right, &layout.inset_bottom);
        pe_touch_layout(touch_->touch, &layout);
        if (!pe_touch_gl_draw(touch_->gl, touch_->touch))
            gles::drain_errors("drawing the touch controls");
    }
    // Port/eame: iOS may not draw from the background (it refuses the GPU
    // work), so the game and its drawing wait (pe_ios_wait_for_foreground),
    // with the controls let go, until the app is back; the save and settings
    // are written first. False when the app is closed meanwhile.
    bool wait_for_foreground() {
        pe_touch_block(touch_->touch, 1);
        const bool back = pe_ios_wait_for_foreground(
            [](void *self) { static_cast<Impl *>(self)->save_before_background(); }, this);
        pe_touch_block(touch_->touch, 0);
        return back;
    }
    void save_before_background() {
        if (!background_handler_)
            return;
        try {
            background_handler_();
        } catch (const std::exception &error) {
            std::cerr << "Could not save before the background: " << error.what() << '\n';
        }
    }
    std::function<void()> background_handler_;
    std::unique_ptr<TouchPad> touch_;
    bool touch_mouse_on_controls_ = false;
#ifdef __linux__
    std::optional<int> previous_nice_;
#endif
    bool vsync_requested_ = true;
    int requested_swap_interval_ = -2;
    int menu_height_pixels(int drawable_height) const {
        // Fullscreen uses the entire window. The hover bar overlays the picture
        // without resizing it whenever the pointer enters/leaves the top edge.
        if (!panel_ || fullscreen() || drawable_height <= 0)
            return 0;
        int logical_height = 0;
        SDL_GetWindowSize(window_, nullptr, &logical_height);
        if (logical_height <= 0)
            return 0;
        // ImGui sizes are logical window units; OpenGL viewports use drawable
        // pixels. Reserve the bar above the complete game picture at any DPI.
        return std::clamp(int(std::ceil(panel_->menu_height() * drawable_height / logical_height)), 0,
                          std::max(0, drawable_height - 1));
    }

    void toggle_fullscreen() {
        if (!SDL_SetWindowFullscreen(window_, !fullscreen())) {
            const std::string error = std::string("Fullscreen unavailable: ") + SDL_GetError();
            std::cerr << error << '\n';
            if (panel_)
                panel_->set_action_status(error);
        }
    }

    void refresh_asset_cache() {
        constexpr std::array versions{eb::GameVersion::US, eb::GameVersion::JP};
        for (std::size_t index = 0; index < versions.size(); ++index) {
            auto &info = cache_info_[index];
            info.title =
                versions[index] == eb::GameVersion::US ? "EarthBound (English)" : "Mother 2 (Japanese)";
            const auto path = eb::cached_asset_path(cache_directory_, versions[index]).u8string();
            info.path.assign(path.begin(), path.end());
            info.active = game_ == versions[index];
            try {
                info.present = eb::cached_assets_present(cache_directory_, versions[index]);
            } catch (const std::exception &error) {
                info.present = false;
                panel_->set_action_status(error.what());
            }
        }
    }

    void handle_panel_action() {
        const auto action = panel_->take_action();
        if (!action)
            return;
        if (*action == eb::PanelAction::ToggleFullscreen) {
            toggle_fullscreen();
            return;
        }
        if (*action == eb::PanelAction::SwitchEarthBound || *action == eb::PanelAction::SwitchMother2) {
            next_game_ =
                *action == eb::PanelAction::SwitchEarthBound ? eb::GameVersion::US : eb::GameVersion::JP;
            return;
        }
        const auto game =
            *action == eb::PanelAction::ClearEarthBoundAssets ? eb::GameVersion::US : eb::GameVersion::JP;
        try {
            const bool removed = eb::clear_cached_assets(cache_directory_, game);
            refresh_asset_cache();
            panel_->set_action_status(
                removed ? "Imported assets cleared. ROMs and saves are unchanged. The current game can "
                          "continue; switch to this version to import again."
                        : "No imported cache was present. ROMs and saves are unchanged.");
        } catch (const std::exception &error) {
            panel_->set_action_status(std::string("Could not clear imported assets: ") + error.what());
        }
    }

    void cleanup() {
#ifdef __linux__
        if (previous_nice_) {
            setpriority(PRIO_PROCESS, 0, *previous_nice_);
            previous_nice_.reset();
        }
#endif
        // Destroy GL clients before deleting their context, then SDL last. The
        // same path handles both normal destruction and partial construction.
        input_.reset();
        touch_.reset();
        panel_.reset();
        presenter_.reset();
        if (context_)
            SDL_GL_DestroyContext(context_);
        if (window_)
            SDL_DestroyWindow(window_);
        if (initialized_)
            SDL_Quit();
        context_ = nullptr;
        window_ = nullptr;
        initialized_ = false;
    }
    SDL_Window *window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    std::unique_ptr<DesktopInput> input_;
    std::unique_ptr<eb::FramePresenter> presenter_;
    std::unique_ptr<eb::DebugPanel> panel_;
    eb::GameDebugSnapshot debug_snapshot_;
    eb::DisplaySettings &settings_;
    std::string controller_preferences_;
    ControllerSettings controller_settings_;
    std::string game_title_;
    std::filesystem::path cache_directory_;
    std::array<eb::AssetCacheInfo, 2> cache_info_;
    std::vector<eb::SaveStateSnapshotInfo> snapshots_;
    std::string snapshot_status_;
    bool snapshots_available_{};
    std::string custom_assets_;
    eb::GameVersion game_ = eb::GameVersion::US;
    std::optional<eb::GameVersion> next_game_;
    bool initialized_ = false;
};

DesktopDisplay::DesktopDisplay(const LaunchOptions &options, DisplaySettings &settings)
    : impl_(std::make_unique<Impl>(options, settings)) {}
DesktopDisplay::~DesktopDisplay() = default;
GameAssets DesktopDisplay::import_assets(std::string &path, const std::string &error,
                                         const std::string &directory, bool lock_game, GameVersion game,
                                         const std::string &found_rom) {
    return impl_->import_assets(path, error, directory, lock_game, game, found_rom);
}
void DesktopDisplay::start_panel(bool visible, const std::string &title, const std::string &preferences,
                                 GameVersion game, const std::string &custom_assets) {
    impl_->start_panel(visible, title, preferences, game, custom_assets);
}
bool DesktopDisplay::poll_events(std::uint16_t &physical_buttons) {
    return impl_->events(physical_buttons);
}
void DesktopDisplay::set_background_handler(std::function<void()> handler) {
    impl_->set_background_handler(std::move(handler));
}
void DesktopDisplay::update_debug(GameDebug &debug) {
    impl_->update_debug(debug);
}
void DesktopDisplay::adopt_debug(const GameDebug &debug) { impl_->adopt_debug(debug); }
std::optional<GameVersion> DesktopDisplay::take_game_request() {
    return impl_->take_game_request();
}
std::optional<SaveStateSnapshotRequest> DesktopDisplay::take_snapshot_action() {
    return impl_->take_snapshot_action();
}
void DesktopDisplay::set_snapshot_state(std::vector<SaveStateSnapshotInfo> snapshots, std::string status,
                                       bool available) {
    impl_->set_snapshot_state(std::move(snapshots), std::move(status), available);
}
unsigned DesktopDisplay::render_width() const {
    return impl_->render_width();
}
bool DesktopDisplay::fullscreen() const {
    return impl_->fullscreen();
}
bool DesktopDisplay::wants_register_diagnostics() const {
    return impl_->wants_register_diagnostics();
}
double DesktopDisplay::frame_rate() const {
    return impl_->frame_rate();
}
double DesktopDisplay::presentation_rate() const {
    return impl_->presentation_rate();
}
void DesktopDisplay::update_swap_interval() {
    impl_->update_swap_interval();
}
void DesktopDisplay::present(const SessionDiagnostics &diagnostics, PresentationPicture picture,
                             const std::string &capture) {
    impl_->present(diagnostics, picture, capture);
}
} // namespace eb
