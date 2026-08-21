#include "ui/fwApp.h"

#include "ui/fwTheme.h"
#include "ui/fwFonts.h"
#include "ui/fwDeviceBar.h"
#include "ui/fwTabAppExplorer.h"
#include "ui/fwTabDefaultFirmware.h"
#include "ui/fwTabRecovery.h"
#include "ui/fwTabSettings.h"
#include "ui/fwBoardImage.h"
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  // Guarded on the same condition CMakeLists.txt uses to decide whether this
  // file is compiled at all -- on Windows the icon is wired up by linking
  // resources/app.rc and nothing calls into here. See fwWindowIcon.h.
  #include "ui/fwWindowIcon.h"
#endif
#include "ui/fwFlashDialog.h"
#include "device/fwFinderManager.h"
#include "device/fwDeviceModel.h"
#include "platform/fwPaths.h"
#include "catalog/fwCatalogEmbedded.h"
#include "catalog/fwCatalogLocal.h"
#include "catalog/fwCatalogRemote.h"
#include "catalog/fwCatalogMerge.h"
#include "catalog/fwCatalogFilter.h"  // excludeSlug
#include "flash/fwFlashPlan.h"        // kEraseMainCpuSlug
#include "core/fwSettingsIo.h"        // formatSettingsLine / parseSettingsLine / normalizeRemoteCatalogUrl

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <IconsMaterialDesign.h>

#if defined(__EMSCRIPTEN__)
  #include <emscripten.h>
#endif

#include <filesystem>
#include <iterator>   // std::size
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace fwog {
namespace {

// Declared up here rather than beside the tab-bar loop because loadSettings()
// clamps a restored lastTab against kTabCount, and that runs long before the
// loop does.
constexpr const char* kTabLabels[] = { "App Explorer", "OG Bootloader Installer", "Recovery", "Settings" };
constexpr int kTabCount = int(std::size(kTabLabels));

// ---------------------------------------------------------------------------
// Settings: theme, last tab, window geometry, remote catalog URL. Persisted
// to userDataDir()/"settings.ini" as flat key=value lines -- no external
// dependency needed for a handful of scalars.
// ---------------------------------------------------------------------------

struct Settings {
    Theme theme = Theme::Wili;
    int lastTab = 0;
    int windowX = -1;  // -1 => let SDL pick the initial position
    int windowY = -1;
    int windowW = 1280;
    int windowH = 800;
    std::string remoteCatalogUrl;
    /// Whether this settings.ini has ever had the shipped default catalog URL
    /// seeded into it. Always written from the first save onward; the only
    /// thing that reads it is remoteCatalogUrlAtStartup(), whose header
    /// explains why an empty URL alone cannot be trusted to mean "off".
    bool remoteCatalogDefaultSeeded = false;
};

Theme themeFromName(const std::string& name)
{
    for (int i = 0; i < kThemeCount; ++i) {
        Theme t = static_cast<Theme>(i);
        if (name == themeName(t)) return t;
    }
    return Theme::Wili;
}

std::filesystem::path settingsPath()
{
    return userDataDir() / "settings.ini";
}

Settings loadSettings()
{
    Settings s;
    std::ifstream in(settingsPath());

    // A MISSING file is parsed as an empty one rather than returned early, and
    // that is why this is a loop guard instead of a `return` above. The
    // seeding at the bottom is the whole reason a first launch reads settings
    // at all, and a first launch is exactly the case with no file to open. An
    // early return skipped it, then saveSettings() wrote an empty URL beside
    // the marker saying the default had already been offered -- which is the
    // one combination remoteCatalogUrlAtStartup() reads as "the user cleared
    // this on purpose", so the default could never be seeded again. Keeping a
    // SINGLE exit means the rule cannot be bypassed by a path that returns
    // before reaching it.
    std::string line;
    while (std::getline(in, line)) {
        // Splitting, CR tolerance and trimming all live in parseSettingsLine()
        // (fwSettingsIo.h) so the exact shape a line is written in and the
        // shape it is read back in are defined by one tested pair of
        // functions rather than by two hand-rolled halves that can drift.
        const auto parsed = parseSettingsLine(line);
        if (!parsed) continue;
        const std::string& key = parsed->first;
        const std::string& value = parsed->second;

        if (key == "theme") s.theme = themeFromName(value);
        else if (key == "lastTab") { try { s.lastTab = std::stoi(value); } catch (...) {} }
        else if (key == "windowX") { try { s.windowX = std::stoi(value); } catch (...) {} }
        else if (key == "windowY") { try { s.windowY = std::stoi(value); } catch (...) {} }
        else if (key == "windowW") { try { s.windowW = std::stoi(value); } catch (...) {} }
        else if (key == "windowH") { try { s.windowH = std::stoi(value); } catch (...) {} }
        // Re-validated on the way IN, not only on the way out. settings.ini is
        // a plain text file: a user can hand-edit it, and one written by an
        // earlier build of this app can legitimately hold an http:// address
        // that normalizeRemoteCatalogUrl() no longer accepts (see its header
        // comment -- a catalog decides which CPU gets written, so it is fetched
        // over HTTPS only). Without this check that address would be fetched
        // anyway, every launch, having passed through no validation at all.
        // A rejected value is dropped rather than reported: the App Explorer
        // tab's status line then reads "No remote catalog is configured, so
        // nothing is being fetched", which is exactly what is true.
        else if (key == "remoteCatalogUrl") {
            if (auto normalized = normalizeRemoteCatalogUrl(value))
                s.remoteCatalogUrl = *normalized;
        }
        // Any value at all counts as "the marker is present". It is written as
        // "1" and never read back as a number: what matters is that an earlier
        // run of a build that HAS a default got as far as saving, which is
        // precisely what distinguishes a user who cleared the field from one
        // who was never offered anything to clear.
        else if (key == "remoteCatalogDefaultSeeded") s.remoteCatalogDefaultSeeded = true;
    }

    // Seeding happens HERE, after the whole file has been read, not inside the
    // loop: the rule needs both the URL and the marker, and settings.ini has no
    // guaranteed key order -- deciding at the moment `remoteCatalogUrl` is
    // parsed would read a marker that may not have been reached yet.
    s.remoteCatalogUrl = remoteCatalogUrlAtStartup(s.remoteCatalogUrl,
                                                   s.remoteCatalogDefaultSeeded);
    // Set unconditionally, so the marker lands on disk at the next save
    // whichever branch above ran. From then on an empty URL means "off" and
    // stays meaning it.
    s.remoteCatalogDefaultSeeded = true;

    // Clamped to the tab count. A settings.ini written by a build with MORE
    // tabs than this one has must not select a tab that does not exist here.
    if (s.lastTab < 0 || s.lastTab >= kTabCount) s.lastTab = 0;
    if (s.windowW <= 0) s.windowW = 1280;
    if (s.windowH <= 0) s.windowH = 800;
    return s;
}

void saveSettings(const Settings& s)
{
    std::ofstream out(settingsPath(), std::ios::trunc);
    if (!out) return; // best-effort: a read-only user data dir loses geometry, nothing else
    out << formatSettingsLine("theme", themeName(s.theme));
    out << formatSettingsLine("lastTab", std::to_string(s.lastTab));
    out << formatSettingsLine("windowX", std::to_string(s.windowX));
    out << formatSettingsLine("windowY", std::to_string(s.windowY));
    out << formatSettingsLine("windowW", std::to_string(s.windowW));
    out << formatSettingsLine("windowH", std::to_string(s.windowH));
    // Written verbatim, and safe to write verbatim: the only two ways this
    // field acquires a value are loadSettings() (which read it from a line
    // like this one) and the App Explorer tab's control, which runs every
    // typed URL through normalizeRemoteCatalogUrl() and so cannot hand a
    // newline down here to split the file.
    out << formatSettingsLine("remoteCatalogUrl", s.remoteCatalogUrl);
    // Deliberately not conditional on the field: loadSettings() forces it true,
    // so every file this app writes carries it, including the one written by
    // the very first run that seeded the default.
    out << formatSettingsLine("remoteCatalogDefaultSeeded", "1");
}

// Two instances of this app can run at once -- a user opening it twice, a
// leftover test process, nothing exotic -- each with its own in-memory
// Settings loaded at its own startup. Naively writing this instance's whole
// snapshot at exit would silently discard every field the OTHER instance
// changed in the meantime: whichever instance's process happens to exit
// last wins outright, and the earlier instance's change to theme, tab,
// geometry or catalog URL vanishes with no error and no trace.
//
// Fixed as a read-modify-write, not a lock (a lock would break the
// legitimate two-instance case instead of protecting it): re-read the file
// immediately before writing -- picking up whatever the other instance
// wrote, if anything, since this instance started -- and apply onto that
// fresh read only the fields THIS instance actually changed. A field counts
// as "changed by this instance" when its current value differs from what
// this same instance loaded at its own startup; there is no separate dirty
// flag to keep in sync, so a field this instance never touched -- a run in
// which the user never opened the theme menu, never resized the window, or
// never touched the App Explorer tab's remote catalog control -- is never
// blindly overwritten with its own stale copy.
//
// This narrows the race to the gap between the re-read here and the write
// inside saveSettings() -- effectively closing the window this project's
// two instances actually hit in practice (one save completing before the
// other starts) without pretending a single-process lock could ever be
// airtight against a second instance that is legitimately allowed to run.
void saveSettingsMerged(const Settings& loadedAtStartup, const Settings& current)
{
    Settings onDisk = loadSettings();
    if (current.theme            != loadedAtStartup.theme)            onDisk.theme            = current.theme;
    if (current.lastTab          != loadedAtStartup.lastTab)          onDisk.lastTab          = current.lastTab;
    if (current.windowX          != loadedAtStartup.windowX)          onDisk.windowX          = current.windowX;
    if (current.windowY          != loadedAtStartup.windowY)          onDisk.windowY          = current.windowY;
    if (current.windowW          != loadedAtStartup.windowW)          onDisk.windowW          = current.windowW;
    if (current.windowH          != loadedAtStartup.windowH)          onDisk.windowH          = current.windowH;
    if (current.remoteCatalogUrl != loadedAtStartup.remoteCatalogUrl) onDisk.remoteCatalogUrl = current.remoteCatalogUrl;
    saveSettings(onDisk);
}


// Requires SDL_Init(SDL_INIT_VIDEO) to already have run (display queries are
// only valid once the video subsystem is up), so this cannot live inside
// loadSettings() and must be called after SDL_Init succeeds, before
// SDL_CreateWindow.
//
// A saved window rect can go bad between sessions in an entirely routine
// way -- unplug a monitor, and a position that was on-screen last time can
// now be nowhere any attached display can show it, with no in-app way back
// short of deleting settings.ini. Clamp size to the primary display's
// bounds (so a corrupt ini can't produce an absurd window either) and, if
// the saved position doesn't land on any currently-attached display, fall
// back to SDL's default placement instead of opening an unreachable window.
void clampGeometryToDisplays(Settings& s)
{
    SDL_Rect primary{};
    const bool havePrimary = SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &primary);
    const int maxW = (havePrimary && primary.w > 0) ? primary.w : 1920;
    const int maxH = (havePrimary && primary.h > 0) ? primary.h : 1080;

    if (s.windowW < 320 || s.windowW > maxW) s.windowW = (1280 < maxW) ? 1280 : maxW;
    if (s.windowH < 240 || s.windowH > maxH) s.windowH = (800 < maxH) ? 800 : maxH;

    if (s.windowX < 0 || s.windowY < 0) return; // already "let SDL pick a position"

    const SDL_Rect saved{ s.windowX, s.windowY, s.windowW, s.windowH };
    int displayCount = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
    bool onScreen = false;
    if (displays) {
        for (int i = 0; i < displayCount && !onScreen; ++i) {
            SDL_Rect bounds{};
            if (SDL_GetDisplayBounds(displays[i], &bounds)) {
                SDL_Rect intersection{};
                if (SDL_GetRectIntersection(&saved, &bounds, &intersection)) onScreen = true;
            }
        }
        SDL_free(displays);
    }
    if (!onScreen) {
        s.windowX = -1;
        s.windowY = -1;
    }
}

} // namespace

int App::run()
{
    // Runs fwFinderManager::instance().shutdown() no matter which of this
    // function's several return statements executes -- MUST run before
    // returning, and must not be left to static-destructor timing (the
    // manager's own header: its worker thread can be killed mid-call into
    // Windows setupapi/cfgmgr while DLLs are unloading). Declared first so
    // its destructor is the LAST thing that runs on every exit path,
    // including the early "SDL failed to initialize" returns below.
    // shutdown() is cheap and safe to call even when nothing was ever
    // started (requestStop = true, then a loop on an atomic _isRunning that
    // is initialised false, so it returns immediately), which is what makes
    // the early "SDL failed to initialize" returns below safe. A scan IS
    // started further down (deviceModel.requestRescan(), just before the main
    // loop), so on every normal run there really is a worker thread to stop,
    // and an early-exit path that skipped shutdown would be exactly the hang
    // the header warns about. That worker now lives for the whole run rather
    // than exiting six seconds in (see fwFinderManager::run()), so "there
    // really is a thread to stop" is no longer merely the usual case -- it is
    // every case, and this guard is what stops it.
    //
    // The other half of that is inside shutdown() itself: it is TERMINAL, so a
    // requestRefresh() arriving from anywhere during or after teardown -- the
    // flash dialog re-arms the scanner every frame while it is open -- cannot
    // resurrect a detached worker that would then outlive this function.
    struct FinderShutdownGuard {
        ~FinderShutdownGuard() { fwFinderManager::instance().shutdown(); }
    } finderShutdownGuard;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Kept for the whole run as the baseline saveSettingsMerged() diffs
    // against at exit, to tell "this instance actually changed this field"
    // apart from "this instance never touched it, so don't clobber whatever
    // another instance wrote in the meantime" -- see saveSettingsMerged()'s
    // comment above. Deliberately NOT clamped: clamping is a startup-only
    // display-geometry transform, not something the user did.
    const Settings loadedAtStartup = loadSettings();
    Settings settings = loadedAtStartup;
    // Needs the video subsystem already up (display queries), so this can't
    // happen inside loadSettings() -- must run after SDL_Init, before
    // SDL_CreateWindow.
    clampGeometryToDisplays(settings);

    // FWOG_DISPLAY_VERSION comes from CMakeLists.txt so the title cannot drift
    // from the build's own idea of its version. The fallback keeps this file
    // compilable on its own (an IDE indexer, a one-off translation unit) rather
    // than failing on a definition only the build system supplies.
#ifndef FWOG_DISPLAY_VERSION
#define FWOG_DISPLAY_VERSION "v?"
#endif
    SDL_Window* window = SDL_CreateWindow("FreeWili OG App Explorer " FWOG_DISPLAY_VERSION,
                                           settings.windowW, settings.windowH,
                                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (settings.windowX >= 0 && settings.windowY >= 0)
        SDL_SetWindowPosition(window, settings.windowX, settings.windowY);

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // Straight after the window exists and before anything can show it: an
    // icon set later would mean a window that appears with the default one
    // and changes a moment afterwards.
    applyWindowIcon(window);
#endif

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // We own persistence (settings.ini above), not ImGui's own imgui.ini.
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Fonts must be added before ImGui_ImplSDLRenderer3_Init(), which bakes
    // the atlas texture from whatever fonts are registered at that point.
    Fonts::initialize(io, 17);
    io.FontDefault = Fonts::ui();

    applyTheme(settings.theme);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    Theme currentTheme = settings.theme;
    // initialTab is read-only: which tab to ask ImGui to select on the first
    // frame. lastTab tracks whatever is actually active right now (updated
    // every frame from BeginTabItem's return). These must NOT be the same
    // variable: earlier tabs in the for loop below run before a later tab
    // whose index matches the restored value, and if that loop wrote its
    // "currently active" result into the same variable the restore target
    // was read from, the restore target would be clobbered by tab 0 before
    // the loop ever reached it -- silently discarding any saved lastTab != 0.
    const int initialTab = settings.lastTab;
    int lastTab = settings.lastTab;
    bool appliedInitialTab = false;
    bool running = true;

    // Set by the flash dialog's "Open Recovery" button (via the
    // onOpenRecovery callback passed to flashDialog.draw() below) and
    // consumed once by the tab-bar loop, the same "request now, apply and
    // clear on the next draw" shape appliedInitialTab/initialTab already
    // use above for restoring the last-active tab on launch.
    std::optional<int> pendingTabSwitch;

    // Owns the device list and selection for the whole app; the device bar
    // and every later tab read through this, never through fwFinderManager
    // directly. This is the first real device scan the app performs (Tasks
    // 1-15 only built the shell) -- kick it off once here so a scan is
    // already in flight before the first frame renders; DeviceBar::draw()
    // polls the cached result every frame via model.refresh().
    DeviceModel deviceModel;
    deviceModel.requestRescan();

    // The App Explorer tab's data source: embedded is compiled in and always
    // available; local scans catalog/ beside the executable; remote is
    // whatever was cached from a previous run (loadCache() is local file
    // I/O, not a network call) plus a live fetch of whatever URL is
    // configured. On a fresh install that is the shipped default
    // (defaultRemoteCatalogUrl(), seeded by loadSettings above), so the app
    // arrives showing the published catalog rather than showing nothing until
    // somebody tells the user an address to paste. A user who clears the field
    // on the Settings tab gets an empty URL and no fetch, and stays that way
    // across launches -- see remoteCatalogUrlAtStartup(). A failed or absent
    // fetch is a status line, not a missing tab: see RemoteCatalog's own
    // header for why it never clears good data on failure.
    //
    // Both the catalog and the setting are handed to the tab below by
    // reference: the control writes the new URL straight into
    // `settings.remoteCatalogUrl` (which saveSettingsMerged() then carries
    // to disk at exit, exactly like the theme and the window geometry) and
    // calls start() itself, so the result of a change is visible in the same
    // session rather than only after a restart.
    //
    // The LOCAL half is a LocalCatalogCache rather than a bare
    // loadLocalCatalog() call in the frame body -- declared out here precisely
    // because it has to survive across frames to be a cache at all. See its
    // header comment (fwCatalogLocal.h) and the tab-bar loop below.
    LocalCatalogCache localCatalog;

    RemoteCatalog remoteCatalog;
    remoteCatalog.loadCache();
    if (!settings.remoteCatalogUrl.empty())
        remoteCatalog.start(settings.remoteCatalogUrl);

    AppExplorerTab appExplorerTab;
    DefaultFirmwareTab defaultFirmwareTab;
    TabRecovery recoveryTab;
    SettingsTab settingsTab;

    // Uploaded ONCE, here, rather than on first draw: it needs the renderer,
    // and creating a texture inside a frame that is already recording draw
    // commands is the kind of thing that works until it does not. Released
    // before the renderer at the bottom of this function.
    BoardImage boardImage = loadBoardRecoveryImage(renderer);
    // Shared by both flash-capable tabs (see fwFlashDialog.h's header
    // comment) so only one flash can ever be in flight across the whole
    // app, and drawn once below, outside the tab bar, so it stays visible
    // (and, while Running, un-Escape-able) no matter which tab is active
    // underneath it.
    //
    // A unique_ptr, not a plain local, so it can be destroyed EXPLICITLY
    // (see the reset() call right after the main loop below) before SDL and
    // ImGui are torn down further down this function: FlashController's
    // destructor joins the flash worker thread (see its own comment), and
    // that join must happen while the window and renderer this app is
    // running under still exist. Relying on flashDialog's normal end-of-
    // scope destruction would run it AFTER SDL_DestroyWindow()/
    // ImGui::DestroyContext() below, leaving the process alive with no
    // window while a flash's last step finishes.
    auto flashDialog = std::make_unique<FlashDialog>();

    // One iteration of the main loop, extracted into a callable so the SAME
    // body can be driven two different ways (Task 21). On the desktop the
    // `while (running)` below calls it exactly as the loop always did. In a
    // browser it cannot be a blocking loop at all: the page has a single
    // event loop and never yielding to it hangs the tab, so Emscripten calls
    // this once per requestAnimationFrame instead.
    //
    // The body is unchanged apart from two mechanical rewrites forced by
    // being a lambda rather than a loop body: `break` and `continue` are not
    // valid here, so both became `return` -- exactly equivalent, since the
    // enclosing `while` re-tests `running` immediately either way.
    //
    // Captured by reference, and everything it touches is declared above it.
    auto frame = [&]() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                       event.window.windowID == SDL_GetWindowID(window)) {
                // The modal works hard to make a running flash un-abortable
                // from inside the UI -- it blocks input, Escape is
                // excluded, the Running branch offers no Close button (see
                // FlashDialog::draw()) -- and the title bar's own close
                // control must not be a back door around all of that: a
                // LegacyDirect plan aborted between any two of its erase and
                // write steps leaves exactly the mixed state
                // RecoveryAnchor::PartialLegacyFlash documents. Ignore the
                // request outright while Running; the modal's own Cancel
                // button is the only way out.
                //
                // The Recovery tab's CPU identification is guarded the same
                // way, and for a reason of the same shape: it writes the CPU
                // prober to one of the two indistinguishable volumes and then
                // reads the answer back, and closing the app in between leaves
                // a CPU running the prober with the only thing that knows
                // which port it is on gone. (The Recovery tab does offer a
                // manual port picker to get out of that -- but not creating
                // the state is better than documenting the way out of it.)
                if (!flashDialog->isFlashRunning() && !recoveryTab.isIdentifyRunning())
                    running = false;
            }
        }
        if (!running) return;

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            return;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        // No ImGuiWindowFlags_MenuBar: the theme picker was this window's only
        // menu, and it now lives in the device bar immediately left of Rescan
        // (see DeviceBar::draw). An empty menu bar would still reserve its row
        // of vertical space, so the flag goes with it.
        ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::Begin("##fwOGAppExplorerMain", nullptr, windowFlags);

        // Drained every frame, OUTSIDE the tab bar, for the same reason
        // flashDialog->draw() is called outside it below: TabRecovery::draw()
        // only runs while the Recovery tab is the active one, and an
        // identification that is writing the CPU prober to a board must keep
        // making progress -- and keep being cancellable -- while the user is
        // looking at something else.
        recoveryTab.poll();

        // The Recovery tab's CPU identification, handed to the device model
        // BEFORE anything reads an identity this frame, so that "which CPU is
        // drive G:" is part of the one CpuIdentity the device bar renders, the
        // disabled-reason judges and the flash engine acts on -- rather than a
        // second source every consumer would have to remember to consult.
        //
        // Re-supplied every single frame, and re-derived from the current
        // volumes inside verifiedVolume() (fwTabRecovery.h) rather than cached:
        // this is a claim about drive letters, and the moment it stops being
        // true the model must stop being told it. There is no expiry path
        // anywhere else -- DeviceModel has no clock and does not watch the
        // volumes -- which is deliberate. One writer, every frame, or nothing.
        deviceModel.setVerifiedVolume(recoveryTab.verifiedVolume());

        // Persistent header, visible above every tab: what is connected,
        // which CPU is which, and how each was identified. See fwDeviceBar.h
        // for why this lives outside the tab bar rather than inside one tab.
        // The identification note travels with it for the same reason: a
        // mapping being DISCARDED changes what every tab's Flash button will
        // do, and must be said somewhere always visible.
        // currentTheme is passed by reference and may be written here; it is
        // still this loop that persists it to settings on exit (see below).
        DeviceBar::draw(deviceModel, recoveryTab.identificationNote(),
                        recoveryTab.identificationDiscarded(), currentTheme,
                        // Installs the bootloader outright rather than sending
                        // the user to the tab that has the button: being told
                        // what is wrong and then having to go and find the fix
                        // is two steps for one decision.
                        //
                        // beginImmediate(), the same no-review path the App
                        // Explorer's Flash button uses. The identity is read
                        // THIS frame and there is no pause before the write, so
                        // there is no window in which the board could be
                        // swapped; the device bar holds the scanner fast while
                        // the banner is up so that snapshot is also recent.
                        [&] {
                            const auto embedded = embeddedEntries();
                            const CatalogEntry* bl = nullptr;
                            for (const auto& e : embedded)
                                if (e.slug == kOgBootloaderSlug) bl = &e;
                            const auto dev = deviceModel.selected();
                            if (bl && dev) flashDialog->beginImmediate(*bl, *dev);
                        });

        // Captured before the loop below overwrites lastTab: whichever tab
        // was active on the PREVIOUS frame, read one last time before this
        // frame's own result replaces it. Used only to detect the Default
        // Firmware tab losing focus (Task 22 fix round) -- see below.
        const int previousTab = lastTab;

        if (ImGui::BeginTabBar("##MainTabs")) {
            // Consumed HERE, at the top, rather than after the loop. A request
            // can now be raised from INSIDE the loop -- the Recovery tab's "you
            // can flash that CPU now" buttons are drawn by recoveryTab.draw(),
            // which runs in this very loop -- and clearing afterwards would
            // discard it in the same frame it was made, before any tab-bar pass
            // could act on it. Taking the request first and clearing it
            // immediately keeps the "apply on the next frame" contract for both
            // callers: this one, and the flash dialog's "Open Recovery" below.
            const std::optional<int> applyTabSwitch = pendingTabSwitch;
            pendingTabSwitch.reset();

            for (int i = 0; i < kTabCount; ++i) {
                ImGuiTabItemFlags tabFlags = ImGuiTabItemFlags_None;
                // Restore the tab that was active last time, once, on the
                // first frame only -- afterwards the user's own clicks own it.
                if (!appliedInitialTab && i == initialTab)
                    tabFlags |= ImGuiTabItemFlags_SetSelected;
                // A flash refusal's "Open Recovery" button, or Recovery's own
                // deep link to a flashing tab, requested this tab -- force it
                // selected this frame, same mechanism.
                if (applyTabSwitch && i == *applyTabSwitch)
                    tabFlags |= ImGuiTabItemFlags_SetSelected;
                if (ImGui::BeginTabItem(kTabLabels[i], nullptr, tabFlags)) {
                    lastTab = i;
                    if (i == 0) {
                        // Rebuilt fresh every frame this tab is visible, same
                        // spirit as DeviceBar's model.refresh(): a file dropped
                        // into catalog/ while the app is running shows up on
                        // the next frame, without a restart.
                        //
                        // The local half goes through LocalCatalogCache rather
                        // than calling loadLocalCatalog() directly. That used
                        // to be a direct call, with a comment calling it "cheap
                        // enough at this catalog's scale" -- true when it
                        // described a directory listing, and false ever since
                        // UF2 PARSING moved into it: loadLocalCatalog() reads
                        // every loose .uf2 into memory in full and walks every
                        // 512-byte block, and a real display image is 16.4 MB /
                        // 32,079 blocks. At 60 Hz on the default tab that is
                        // ~1 GB/s of pointless I/O. The cache stats the
                        // directory instead and re-reads only when something
                        // actually changed, which keeps the drop-in-a-file
                        // behaviour exactly as it was. Remote stays a direct
                        // call: entries() there really is just a mutex-guarded
                        // copy of an already-parsed vector.
                        //
                        // excludeSlug() drops the destructive erase-MAIN
                        // entry (Task 22, kEraseMainCpuSlug) from what App
                        // Explorer browses: that entry still lives in the
                        // embedded catalog (Default Firmware reads it
                        // straight from embeddedEntries(), unfiltered, for
                        // its own danger-styled card), but it must never sit
                        // behind an ordinary "Flash" button here, where
                        // nothing marks it as different from any other app.
                        auto entries = mergeCatalogs(embeddedEntries(),
                                                      localCatalog.entries(catalogDir()),
                                                      remoteCatalog.entries());
                        //
                        // Both erase entries are dropped, for the same reason:
                        // each is a destructive, typed-confirmation-gated
                        // action with its own card on the Default Firmware tab,
                        // and neither may ever sit behind an ordinary "Flash"
                        // button here.
                        entries = excludeSlug(std::move(entries), kEraseMainCpuSlug);
                        entries = excludeSlug(std::move(entries), kEraseDisplayCpuSlug);
                        // ...and then everything that is not a FreeWili OG APP:
                        // the display bootloader and the original deprecated
                        // firmware are board-level operations with their own
                        // cards on the Default Firmware tab, not things to
                        // browse and flash. See onlyOgApps() (fwCatalogFilter.h)
                        // for the wiliOGbsp contract this expresses. It runs
                        // AFTER the erase exclusions because the erase-MAIN
                        // entry is itself an OgApp and would survive it.
                        entries = onlyOgApps(std::move(entries));
                        appExplorerTab.draw(entries, deviceModel, *flashDialog,
                                             remoteCatalog, settings.remoteCatalogUrl);
                    } else if (i == 1) {
                        // The Default Firmware tab runs a CPU identification
                        // ITSELF when an install click needs one, instead of
                        // sending the user here to run it by hand. It is handed
                        // access to the app's ONE prober -- the Recovery tab's
                        // -- rather than being given a controller of its own;
                        // see ProbeAccess (fwTabDefaultFirmware.h) for why a
                        // second one would be a bug rather than a convenience.
                        //
                        // The four lambdas are rebuilt each frame this tab
                        // draws, which costs four small heap allocations at
                        // most and keeps recoveryTab captured by reference for
                        // no longer than the call.
                        ProbeAccess probeAccess;
                        probeAccess.mountedVolumes = [&] { return recoveryTab.mountedVolumeCount(); };
                        probeAccess.running       = [&] { return recoveryTab.isIdentifyRunning(); };
                        probeAccess.lastFailure   = [&] { return recoveryTab.identifyFailure(); };
                        probeAccess.begin         = [&] { recoveryTab.beginIdentify(deviceModel); };
                        defaultFirmwareTab.draw(deviceModel, *flashDialog, probeAccess,
                                                 [&](RecoveryAnchor anchor) {
                                                     recoveryTab.scrollTo(anchor);
                                                     pendingTabSwitch = 2;
                                                 });
                    } else if (i == 2) {
                        // The same pendingTabSwitch mechanism the flash
                        // dialog's "Open Recovery" uses, pointed the other way:
                        // Recovery's "you can flash that CPU now" buttons send
                        // the user to the tab that has a Flash button on it.
                        // Applied on the NEXT frame, since this frame's tab-bar
                        // loop is already running -- identical to every other
                        // "click sets state, next frame applies it" interaction
                        // in this loop.
                        recoveryTab.draw(deviceModel,
                                         [&](int tab) { pendingTabSwitch = tab; },
                                         boardImage);
                    } else if (i == 3) {
                        settingsTab.draw(remoteCatalog, settings.remoteCatalogUrl);
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            appliedInitialTab = true;
        }

        // Task 22 fix round (Critical): the Danger zone card's typed
        // confirmation and its CollapsingHeader's open/closed state both
        // persist by ImGui ID across frames, independent of which tab is
        // currently drawing -- DefaultFirmwareTab::draw() itself is only
        // called while tab 1 is active, so it has no way to notice a gap in
        // visits on its own. Noticing the transition here, where every
        // frame's active tab is known regardless of which tab that is, and
        // clearing right as the Default Firmware tab stops being active
        // guarantees a confirmation typed on a past visit can never still be
        // sitting there, armed, when the user comes back.
        //
        // Final review (Fix 5): the App Explorer tab holds the OTHER control
        // that can point a main-CPU image at the DISPLAY CPU -- the gated
        // Unlisted retarget -- and its armed flag and typed "DISPLAY" lived in
        // AppExplorerTab's own members, cleared only when a DIFFERENT entry was
        // selected. Leaving and returning with the same entry selected left it
        // armed and confirmed. Same mechanism, same place, same guarantee.
        if (previousTab == 0 && lastTab != 0)
            appExplorerTab.onTabHidden();
        if (previousTab == 1 && lastTab != 1)
            defaultFirmwareTab.onTabHidden();

        // Drawn once, outside the tab bar, so it stays visible (and, while
        // Running, un-Escape-able -- see FlashDialog::draw()'s comment) no
        // matter which of the three tabs is active underneath it. Clicking
        // "Open Recovery" calls recoveryTab.scrollTo() immediately and sets
        // pendingTabSwitch for the tab-bar loop above to consume on the
        // NEXT frame (this frame's loop already ran) -- one frame of lag
        // before Recovery is force-selected, same as every other "click
        // sets state, next frame applies it" interaction in this loop
        // (compare initialTab/appliedInitialTab above). scrollTo()'s own
        // request is consumed the first time TabRecovery::draw() actually
        // runs, whichever frame that is, so it cannot go stale in between.
        flashDialog->draw(deviceModel, [&](RecoveryAnchor anchor) {
            recoveryTab.scrollTo(anchor);
            pendingTabSwitch = 2;
        });

        ImGui::End();

        ImGui::Render();
        // ImGui lays out in window points; with SDL_WINDOW_HIGH_PIXEL_DENSITY
        // the renderer's output is in PIXELS, 2x the points on a Retina
        // display -- without this scale the whole UI draws at quarter size in
        // the corner. This is the upstream example's own recipe
        // (example_sdl3_sdlrenderer3). A no-op at scale 1.0, which is why
        // Windows and Linux never showed the defect.
        const ImGuiIO& frameIo = ImGui::GetIO();
        SDL_SetRenderScale(renderer, frameIo.DisplayFramebufferScale.x,
                           frameIo.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 15, 15, 15, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    };

#if defined(__EMSCRIPTEN__)
    // fps = 0 asks Emscripten to drive this from requestAnimationFrame,
    // which is what keeps the page's compositor and this app's frame rate in
    // step (a fixed fps here would either stutter or burn battery).
    //
    // simulate_infinite_loop = 1 means this call does NOT return: Emscripten
    // unwinds to the browser event loop by throwing, deliberately WITHOUT
    // running destructors and WITHOUT reclaiming this function's stack
    // frame. That is precisely what makes capturing `frame`'s enclosing
    // locals by reference safe here -- they stay put for the life of the
    // page -- and it is also why none of the shutdown sequence below ever
    // runs on the web. That is correct rather than merely tolerated: a
    // browser tab has no orderly "the user closed the window" moment to hook,
    // there is no worker thread to join (fwFinderManager is a no-op stub and
    // no flash can ever start -- see flashDisabledReason), and settings.ini
    // lives in MEMFS, which the page discards on unload regardless.
    //
    // The outer lambda is capture-less on purpose so it converts to the plain
    // C function pointer this API takes; `frame` itself is passed as the
    // void* argument.
    emscripten_set_main_loop_arg(
        [](void* arg) { (*static_cast<decltype(frame)*>(arg))(); },
        &frame, /*fps=*/0, /*simulate_infinite_loop=*/1);
    return 0;   // not reached; see above
#else
    while (running)
        frame();
#endif

    // --- Shutdown. Desktop only, for the reason spelled out above; the
    // ordering here is load-bearing and must not be rearranged.

    // Destroyed explicitly, here, before SDL/ImGui teardown below -- see
    // the comment where flashDialog was created.
    flashDialog.reset();

    // Snapshot geometry before tearing the window down, then persist
    // everything in one write.
    {
        int x = settings.windowX, y = settings.windowY;
        int w = settings.windowW, h = settings.windowH;
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);
        settings.theme = currentTheme;
        settings.lastTab = lastTab;
        settings.windowX = x;
        settings.windowY = y;
        settings.windowW = w;
        settings.windowH = h;
        saveSettingsMerged(loadedAtStartup, settings);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    // Before the renderer that owns it, and after the ImGui backend that may
    // still hold its texture id in a draw list.
    destroyBoardImage(boardImage);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    // finderShutdownGuard's destructor runs fwFinderManager shutdown here
    // (see its declaration at the top of this function).
    return 0;
}

} // namespace fwog
