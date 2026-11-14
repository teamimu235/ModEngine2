#define UNICODE 1

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include <detours/detours.h>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <toml++/toml.h>

#include "steam_app_path.h"

namespace fs = std::filesystem;

enum LauncherError : int {
    E_OK = 0,
    E_OS_ERROR = -1,
    E_APP_NOT_FOUND = -2,
    E_MODENGINE_NOT_FOUND = -3,
    E_CREATE_PROCESS_FAILED = -4,
};

struct LaunchTargetParams {
    std::wstring app_id;
    std::wstring executable_path;
    std::wstring default_config;
};

enum LaunchTarget {
    AUTODETECT,
    DS2,
    DS3,
    ELDEN_RING,
    ARMORED_CORE_6,
};

static std::map<LaunchTarget, LaunchTargetParams> launch_targets {
    { DS2, { L"335300", L"Game/DarkSoulsII.exe", L"config_darksouls2.toml" } },
    { DS3, { L"374320", L"Game/DarkSoulsIII.exe", L"config_darksouls3.toml" } },
    { ELDEN_RING, { L"1245620", L"Game/eldenring.exe", L"config_eldenring.toml" } },
    { ARMORED_CORE_6, { L"1888160", L"Game/armoredcore6.exe", L"config_armoredcore6.toml" } }
};

static std::map<std::string, LaunchTarget> launch_target_names {
    { "ds2", DS2 },
    { "ds3", DS3 },
    { "er", ELDEN_RING },
    { "ac6", ARMORED_CORE_6 },
};

static std::map<std::string, LaunchTarget> exe_names {
    { "DarkSoulsII.exe", DS2 },
    { "DarkSoulsIII.exe", DS3 },
    { "eldenring.exe", ELDEN_RING },
    { "armoredcore6.exe", ARMORED_CORE_6 },
};

namespace platform {
std::wstring get_env_var(const std::wstring& name)
{
    size_t buffer_size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    auto* buffer = new wchar_t[buffer_size + 1];

    std::wstring value;

    if (buffer_size > 0) {
        size_t len = GetEnvironmentVariableW(name.c_str(), &buffer[0], buffer_size + 1);
        value.append(buffer, len);
    }

    delete[] buffer;

    return value;
}

void set_env_var(const std::wstring& name, const std::wstring& value)
{
    SetEnvironmentVariableW(name.c_str(), value.c_str());
}

fs::path get_launcher_directory()
{
    wchar_t buffer[MAX_PATH];
    size_t buffer_size = GetModuleFileNameW(nullptr, &buffer[0], MAX_PATH);

    fs::path path = fs::current_path();

    if (buffer_size > 0) {
        fs::path launcher_path(std::wstring_view { &buffer[0], buffer_size });
        path = launcher_path.parent_path();
    }

    return path;
}

}

namespace {

bool inject_dll(HANDLE process, const fs::path& dll_path)
{
    const auto path = fs::absolute(dll_path).wstring();
    const auto byte_count = (path.size() + 1) * sizeof(wchar_t);

    auto remote = VirtualAllocEx(process, nullptr, byte_count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        return false;
    }

    auto ok = WriteProcessMemory(process, remote, path.c_str(), byte_count, nullptr) != FALSE;

    if (ok) {
        auto load_library = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        auto thread = CreateRemoteThread(process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library), remote, 0, nullptr);

        if (thread != nullptr) {
            WaitForSingleObject(thread, 30000);
            CloseHandle(thread);
        } else {
            ok = false;
        }
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

std::vector<fs::path> read_external_dlls(const fs::path& config_path)
{
    std::vector<fs::path> dlls;

    try {
        const auto table = toml::parse_file(config_path.string());
        const auto* modengine = table["modengine"].as_table();
        if (modengine == nullptr) {
            return dlls;
        }

        const auto* list = modengine->get_as<toml::array>("external_dlls");
        if (list == nullptr) {
            return dlls;
        }

        for (const auto& node : *list) {
            if (const auto value = node.value<std::string>()) {
                fs::path path(*value);
                if (path.is_relative()) {
                    path = config_path.parent_path() / path;
                }
                if (fs::exists(path)) {
                    dlls.push_back(path);
                }
            }
        }
    } catch (...) {
    }

    return dlls;
}

}

int main(int argc, char** argv)
{
    auto logger = spdlog::stderr_color_mt("stderr");
    auto launcher_directory = platform::get_launcher_directory();


    CLI::App app { "ModEngine Launcher" };

    LaunchTarget target = AUTODETECT;
    auto target_option = app.add_option("-t,--launch-target", target, "Launch target")
                             ->transform(CLI::CheckedTransformer(launch_target_names, CLI::ignore_case));

    std::string target_path_string;
    auto target_path_option = app.add_option("-p,--game-path", target_path_string, "Path to game executable. Will autodetect if not specified.")
                                  ->transform(CLI::ExistingFile);

    std::string config_path_string;
    auto config_option = app.add_option("-c,--config", config_path_string, "ModEngine configuration file path")
                             ->transform(CLI::ExistingFile);

    bool suspend = false;
    app.add_option("-s,--suspend", suspend, "Start the game in a suspended state");

    auto modengine_dll_path = launcher_directory / L"modengine2" / L"bin" / L"modengine2.dll";

    app.add_option("--modengine-dll", modengine_dll_path, "ModEngine DLL file path (modengine2.dll)");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }


    std::optional<fs::path> app_path = std::nullopt;

    // First if the game path was specified, use that along with the specified target
    if (!target_path_option->empty()) {
        app_path = absolute(CLI::to_path(target_path_string)).parent_path().parent_path();
        if (target == AUTODETECT) {
            logger->error("Game target must be specified when supplying a manual path");
            return E_APP_NOT_FOUND;
        }
    }

    // If the game target was not set, try to find a game exe in the current directory and infer from that
    if (target_option->empty()) {
        for (auto& name_kv : exe_names) {
            auto exepath = launcher_directory / name_kv.first;
            if (fs::exists(exepath)) {
                target = name_kv.second;
                app_path = exepath.parent_path().parent_path(); // app_path is expected to be steam app path not exe path
            }
        }
    }

    // If we still didn't detect the game, error and quit
    if (target == AUTODETECT) {
        logger->error("Couldn't find path to game");
        return E_APP_NOT_FOUND;
    }

    auto launch_params = launch_targets[target];

    // If a config wasn't specified, try to load the default one for the game
    if (config_option->empty()) {
        auto default_config_path = launcher_directory / launch_params.default_config;
        if (!fs::exists(default_config_path)) {
            logger->error("Could not find default config file at {}", default_config_path.string());
        }
        config_path_string = default_config_path.string();
    }

    // If path wasn't already set from detecting the game exe in the launcher directory, use the Steam DB to lookup game path
    if (!app_path) {
        app_path = get_game_path(launch_params.app_id);
    }

    if (!app_path) {
        logger->error("Couldn't find path to game");
        return E_APP_NOT_FOUND;
    }

    auto app_cmd = *app_path / launch_params.executable_path;
    auto app_cwd = fs::absolute(app_cmd.parent_path());

    // Dark Souls 2 is wrapped in SteamStub and refuses to run when started outside of Steam
    // unless the app id can be discovered from a steam_appid.txt in the game directory.
    if (target == DS2) {
        const auto steam_appid_path = app_cmd.parent_path() / L"steam_appid.txt";
        std::error_code steam_appid_error;
        if (!fs::exists(steam_appid_path, steam_appid_error)) {
            std::string app_id(launch_params.app_id.begin(), launch_params.app_id.end());
            const auto handle = CreateFileW(steam_appid_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

            if (handle != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(handle, app_id.data(), static_cast<DWORD>(app_id.size()), &written, nullptr);
                CloseHandle(handle);
                logger->info("Wrote {}", steam_appid_path.string());
            }
        }
    }

    if (!fs::exists(modengine_dll_path)) {
        logger->error("Couldn't find path to modengine2.dll at {}", modengine_dll_path.string());
        return E_MODENGINE_NOT_FOUND;
    }

    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};

    auto kernel32 = LoadLibraryW(L"kernel32.dll");
    auto create_process_addr = GetProcAddress(kernel32, "CreateProcessW");

    auto exec_path = platform::get_env_var(L"PATH");
    exec_path.append(L";");
    exec_path.append(modengine_dll_path.parent_path().native());

    auto config_path = CLI::to_path(config_path_string);
    if (config_path.is_relative()) {
        const auto search_path = launcher_directory / config_path;
        config_path = fs::absolute(search_path);
    }

    // These are inherited by the game process we launch with Detours.
    platform::set_env_var(L"SteamAppId", launch_params.app_id);
    platform::set_env_var(L"MODENGINE_CONFIG", config_path.native());
    platform::set_env_var(L"PATH", exec_path);

    if (suspend || IsDebuggerPresent()) {
        platform::set_env_var(L"MODENGINE_DEBUG_GAME", L"1");
    }

    std::wstring cmd_str = app_cmd.native();
    size_t cmd_len = cmd_str.length();

    auto *cmd = new wchar_t[cmd_len + 1];
    cmd[cmd_len] = 0;

    wcscpy_s(cmd, cmd_len + 1, cmd_str.c_str());

    auto external_dlls = read_external_dlls(config_path);

    // Always use remote injection: DetourCreateProcessWithDllW crashes when the launcher
    // itself lives in a path containing spaces.
    {
        STARTUPINFOW inject_si = {};
        inject_si.cb = sizeof(inject_si);
        PROCESS_INFORMATION inject_pi = {};

        std::wstring inject_cmd = L"\"" + cmd_str + L"\"";
        std::vector<wchar_t> inject_cmd_buffer(inject_cmd.begin(), inject_cmd.end());
        inject_cmd_buffer.push_back(0);

        if (!CreateProcessW(cmd_str.c_str(), inject_cmd_buffer.data(), nullptr, nullptr, false,
                CREATE_SUSPENDED, nullptr, app_cwd.c_str(), &inject_si, &inject_pi)) {
            logger->error("Couldn't create suspended process: {:x}", GetLastError());
            return E_CREATE_PROCESS_FAILED;
        }

        auto inject_status = E_OK;

        for (const auto& dll : external_dlls) {
            if (!inject_dll(inject_pi.hProcess, dll)) {
                logger->error("Couldn't inject {}", dll.string());
                inject_status = E_CREATE_PROCESS_FAILED;
            } else {
                logger->info("Injected {}", dll.string());
            }
        }

        if (!inject_dll(inject_pi.hProcess, modengine_dll_path)) {
            logger->error("Couldn't inject modengine2.dll");
            inject_status = E_CREATE_PROCESS_FAILED;
        }

        ResumeThread(inject_pi.hThread);
        CloseHandle(inject_pi.hThread);
        CloseHandle(inject_pi.hProcess);

        return inject_status;
    }

    return E_OK;
}
