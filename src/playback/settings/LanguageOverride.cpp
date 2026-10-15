#include "LanguageOverride.h"

#include "playback/Playback.h"
#include "playback/settings/Settings.h"

#include "ll/api/i18n/I18n.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace playback::settings {

namespace {

void flatten(nlohmann::json const& json, std::string const& prefix, std::string_view locale) {
    for (auto it = json.begin(); it != json.end(); ++it) {
        auto const key = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) flatten(*it, key, locale);
        else if (it->is_string()) ll::i18n::getInstance().set(locale, key, it->get<std::string>());
    }
}

} // namespace

void applyLanguage(std::filesystem::path const& langDir, std::string_view language) {
    if (language.empty() || language == "auto") return;
    std::ifstream in(langDir / (std::string(language) + ".json"), std::ios::binary);
    auto          json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        Playback::getInstance().getSelf().getLogger().warn("No language file for \"{}\"; using the game language", language);
        return;
    }
    flatten(json, "", ll::i18n::getDefaultLocaleCode());
}

std::string effectiveLanguage() {
    auto const language = SettingsStore::getInstance().get().language;
    if (language != "auto") return language;
    return std::string(ll::i18n::getDefaultLocaleCode());
}

} // namespace playback::settings
