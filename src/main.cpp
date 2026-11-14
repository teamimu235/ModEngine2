#include "modengine/crash_handler.h"
#include "modengine/logging.h"
#include "modengine/mod_engine.h"
#include "modengine/version.h"

#include <optional>
#include <atomic>
#include <windows.h>
#include <iostream>
#include <detours/detours.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <modengine/settings_loader.h>

using namespace modengine;
using namespace spdlog;

namespace fs = std::filesystem;

static HMODULE modengine_instance;
static fs::path modengine_path;
static fs::path game_path;

typedef int(WINAPI* fnEntry)(void);

std::shared_ptr<ModEngine> modengine::mod_engine_global;
Hook<fnEntry> hooked_entrypoint;
HookSet entry_hook_set;

static void initialize_modengine()
{
    wchar_t dll_filename[MAX_PATH + 1];

    // Grab the path to the modengine2.dll file, so we can locate the global
    // configuration from here if it exists.
    if (!GetModuleFileNameW(modengine_instance, dll_filename, MAX_PATH)) {
        return;
    }

    modengine_path = fs::path(dll_filename).parent_path();
    if (modengine_path.filename() == "bin") {
        modengine_path = modengine_path.parent_path();
    }

    wchar_t game_filename[MAX_PATH + 1];

    // Also get the path to the game executable, to support legacy use-cases of putting
    // mods in the game folder.
    if (!GetModuleFileNameW(nullptr, game_filename, MAX_PATH)) {
        return;
    }

    game_path = fs::path(game_filename).parent_path();

    start_crash_handler(modengine_path, game_path);


    auto is_debugger_enabled = std::getenv("MODENGINE_DEBUG_GAME") != nullptr;
    if (is_debugger_enabled) {
        while (!IsDebuggerPresent()) {
            Sleep(100);
        }

        DebugBreak();
    }

    /* We need to restore any changes to entrypoint code.
     * Steam checks the signature of this */
    entry_hook_set.unhook_all();

    SettingsLoader settings_loader(modengine_path, game_path);
    Settings settings;

    auto logs_path = modengine_path / "logs";
    if (!fs::exists(logs_path)) {
        (void) fs::create_directory(logs_path);
    }

    auto logger = logging::setup_logger(logs_path, is_debugger_enabled);
    spdlog::set_default_logger(logger);

    auto settings_status = settings_loader.load(settings);
    auto config = settings.get_config_reader().read_config_object<ModEngineConfig>({ "modengine" });

    if (config.debug && !is_debugger_enabled) {
        // Create debug console
        AllocConsole();
        FILE* stream;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONIN$", "r", stdin);

        logger->set_level(spdlog::level::trace);
        logger->sinks().push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    const auto game_info = GameInfo::from_current_module();
    if (!game_info) {
        error("Unable to detect a supported game");
        return;
    }

    info("ModEngine version {} initializing for {}", modengine::g_version, game_info->description(), game_info->version);
    info("Local settings loaded: {}, Global settings loaded: {}",
        settings_status.found_local_config,
        settings_status.found_global_config);
    info("Main thread ID: {}", GetCurrentThreadId());

    mod_engine_global.reset(new ModEngine { *game_info, settings, config });

    try {
        mod_engine_global->attach();
    } catch (std::exception& e) {
        error("Failed to attach modengine: {}", e.what());
    }
}

static HANDLE(WINAPI* real_create_file_w)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) = nullptr;
static std::atomic<bool> g_modengine_initialized { false };

static HANDLE WINAPI create_file_w_initializer(LPCWSTR file_name, DWORD desired_access, DWORD share_mode,
    LPSECURITY_ATTRIBUTES security_attributes, DWORD creation_disposition, DWORD flags, HANDLE template_file)
{
    if (!g_modengine_initialized.exchange(true)) {
        // Drop this one-shot hook before initializing, so mod_loader can install its own
        // CreateFileW hook without chaining two detours on the same function.
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(reinterpret_cast<PVOID*>(&real_create_file_w), create_file_w_initializer);
        DetourTransactionCommit();

        initialize_modengine();
    }

    return real_create_file_w(file_name, desired_access, share_mode, security_attributes, creation_disposition, flags, template_file);
}

static DWORD WINAPI install_create_file_w_hook(LPVOID)
{
    // DllMain cannot install detours (loader lock); do it from this thread instead.
    Sleep(50);

    real_create_file_w = reinterpret_cast<decltype(real_create_file_w)>(
        DetourFindFunction("kernel32.dll", "CreateFileW"));
    if (real_create_file_w != nullptr) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&real_create_file_w), create_file_w_initializer);
        DetourTransactionCommit();
    }

    return 0;
}

static bool attach(HMODULE module)
{
    modengine_instance = module;

    // Never touch the game entry point: SteamStub and Seamless Co-op own it. The worker
    // thread installs a one-shot CreateFileW hook, and ModEngine initializes on the first
    // file access (outside the loader lock).
    CreateThread(nullptr, 0, install_create_file_w_hook, nullptr, 0, nullptr);

    return true;
}

static bool detach()
{
    if (mod_engine_global != nullptr) {
        mod_engine_global->detach();
    }

    return true;
}BOOL APIENTRY DllMain(HMODULE module, DWORD dwReason, LPVOID)
{
    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        DetourRestoreAfterWith();
        return attach(module);
    case DLL_PROCESS_DETACH:
        return detach();
    }
    return TRUE;
}
