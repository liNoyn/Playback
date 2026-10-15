#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace playback::settings {

// Strings follow the game's language unless settings.json "language" names one: then that language file is
// loaded over the game's locale, so every translated string (and the UI font) uses it.
void applyLanguage(std::filesystem::path const& langDir, std::string_view language);

// The language the UI is shown in: the setting, or the game's locale for "auto".
[[nodiscard]] std::string effectiveLanguage();

} // namespace playback::settings
