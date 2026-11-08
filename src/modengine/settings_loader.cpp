#include "settings_loader.h"

#include <spdlog/spdlog.h>
#include <fstream>

namespace modengine {

using namespace spdlog;

bool SettingsLoader::load_toml_into(Settings& settings, const fs::path& path)
{
    try {
        auto config = toml::parse_file(path.string());
        settings.m_config = config;
    } catch (const toml::parse_error& e) {
        error("Failed to load config (from `{}`): {}", path.string(), e.what());
        return false;
    }

    return true;
}

SettingsLoadResult SettingsLoader::load(modengine::Settings& settings)
{
    SettingsLoadResult result;

    fs::path appdata_path(getenv("APPDATA"));
    settings.m_modengine_data_path = appdata_path / "modengine";
    settings.m_modengine_install_path = m_installation;
    settings.m_game_path = m_game ;

    result.inline_install = m_installation == m_game;

    if (!result.inline_install) {
        const auto global_settings_path = m_installation / "config.toml";
        info("Attempting to load global config at {}", global_settings_path.string());
        if (fs::exists(global_settings_path)) {
            result.found_global_config = load_toml_into(settings, global_settings_path);
            if (result.found_global_config)
                info("Global config loaded");
        }
    }

    const auto settings_path_env = std::getenv("MODENGINE_CONFIG");

    if (settings_path_env != nullptr) {
        auto path = fs::path(settings_path_env);
        auto local_modengine_path = path.parent_path();
        info("Attempting to load mod settings config at {}", path.string());

        result.found_local_config = load_toml_into(settings, path);
        settings.m_modengine_local_path = local_modengine_path;
        if (result.found_local_config)
            info("Local config loaded");
    } else {
        // Mod Engine can be sideloaded (for Dark Souls 2 it has to be, because the game is
        // protected by SteamStub). In that case there is no launcher to pass MODENGINE_CONFIG,
        // so fall back to a config file that lives next to the DLL.
        for (const auto& config_name : { "config_darksouls2.toml", "config.toml" }) {
            const auto settings_path = m_installation / config_name;
            if (!fs::exists(settings_path)) {
                continue;
            }

            info("Attempting to load config at {}", settings_path.string());
            result.found_local_config = load_toml_into(settings, settings_path);
            settings.m_modengine_local_path = m_installation;
            if (result.found_local_config) {
                info("Local config loaded");
            }
            break;
        }
    }

    return result;
}

}
