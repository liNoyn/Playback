#include "Settings.h"

#include "playback/Playback.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace playback::settings {

namespace {

using Json = nlohmann::ordered_json;

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view text) {
    auto const begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    auto const end = text.find_last_not_of(" \t");
    return std::string(text.substr(begin, end - begin + 1));
}

struct NamedKey {
    std::string_view name;
    int              code;
};

constexpr std::array NamedKeys{
    NamedKey{"Insert",      0x2D},
    NamedKey{"Delete",      0x2E},
    NamedKey{"Home",        0x24},
    NamedKey{"End",         0x23},
    NamedKey{"PageUp",      0x21},
    NamedKey{"PageDown",    0x22},
    NamedKey{"Pause",       0x13},
    NamedKey{"ScrollLock",  0x91},
    NamedKey{"Backquote",   0xC0},
    NamedKey{"Minus",       0xBD},
    NamedKey{"Equals",      0xBB},
    NamedKey{"LeftBracket", 0xDB},
    NamedKey{"RightBracket",0xDD},
};

std::optional<int> keyCode(std::string_view name) {
    auto const key = lower(name);
    if (key.size() >= 2 && key[0] == 'f') {
        int number{};
        auto const [ptr, ec] = std::from_chars(key.data() + 1, key.data() + key.size(), number);
        if (ec == std::errc{} && ptr == key.data() + key.size() && number >= 1 && number <= 24) return 0x70 + number - 1;
    }
    if (key.size() == 1 && std::isalpha(static_cast<unsigned char>(key[0]))) return std::toupper(key[0]);
    if (key.size() == 1 && std::isdigit(static_cast<unsigned char>(key[0]))) return key[0];
    if (key.starts_with("numpad") && key.size() == 7 && std::isdigit(static_cast<unsigned char>(key[6]))) {
        return 0x60 + (key[6] - '0');
    }
    for (auto const& named : NamedKeys) {
        if (lower(named.name) == key) return named.code;
    }
    return std::nullopt;
}

std::string keyName(int code) {
    if (code >= 0x70 && code <= 0x87) return "F" + std::to_string(code - 0x70 + 1);
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::string(1, static_cast<char>(code));
    if (code >= 0x60 && code <= 0x69) return "Numpad" + std::to_string(code - 0x60);
    for (auto const& named : NamedKeys) {
        if (named.code == code) return std::string(named.name);
    }
    return "Key" + std::to_string(code);
}

std::string_view cornerName(HudCorner corner) {
    switch (corner) {
    case HudCorner::TopLeft:
        return "topLeft";
    case HudCorner::TopRight:
        return "topRight";
    case HudCorner::BottomLeft:
        return "bottomLeft";
    case HudCorner::BottomRight:
        return "bottomRight";
    }
    return "topRight";
}

std::optional<HudCorner> parseCorner(std::string_view text) {
    for (auto corner : {HudCorner::TopLeft, HudCorner::TopRight, HudCorner::BottomLeft, HudCorner::BottomRight}) {
        if (cornerName(corner) == text) return corner;
    }
    return std::nullopt;
}

// Reads `key` from `json` into `target` when present and of the right type; anything else keeps the default.
template <class T>
void read(Json const& json, char const* key, T& target) {
    auto const it = json.find(key);
    if (it == json.end()) return;
    try {
        target = it->get<T>();
    } catch (std::exception const&) {
        getLogger().warn("settings.json: ignoring invalid value for \"{}\"", key);
    }
}

std::string readString(Json const& json, char const* key) {
    std::string value;
    read(json, key, value);
    return value;
}

Json const& section(Json const& json, char const* key) {
    static Json const empty = Json::object();
    auto const        it    = json.find(key);
    return it != json.end() && it->is_object() ? *it : empty;
}

Settings fromJson(Json const& json) {
    Settings settings;
    read(json, "version", settings.version);
    read(json, "language", settings.language);
    if (settings.language != "auto" && settings.language != "ja_JP" && settings.language != "en_US"
        && settings.language != "zh_CN") {
        settings.language = "auto";
    }

    read(section(json, "recording"), "compatibleFormat", settings.recording.compatibleFormat);

    auto const& output = section(json, "output");
    settings.output.mode       = parseOutputMode(readString(output, "mode")).value_or(settings.output.mode);
    settings.output.convert    = parseConvertTiming(readString(output, "convert")).value_or(settings.output.convert);
    settings.output.sourceKeep = SourceKeep::parse(readString(output, "sourceKeep")).value_or(settings.output.sourceKeep);
    read(output, "askOnStart", settings.output.askOnStart);

    auto const& time = section(json, "timeNormalization");
    read(time, "mode", settings.timeNormalization.mode);
    if (settings.timeNormalization.mode != "game" && settings.timeNormalization.mode != "wall"
        && settings.timeNormalization.mode != "raw") {
        settings.timeNormalization.mode = "game";
    }
    read(time, "lagMarkers", settings.timeNormalization.lagMarkers);
    read(time, "lagThresholdTps", settings.timeNormalization.lagThresholdTps);
    read(time, "maxGapSeconds", settings.timeNormalization.maxGapSeconds);
    settings.timeNormalization.lagThresholdTps = std::clamp(settings.timeNormalization.lagThresholdTps, 1.0, 20.0);
    settings.timeNormalization.maxGapSeconds   = std::clamp(settings.timeNormalization.maxGapSeconds, 1.0, 600.0);

    auto const& hotkeys = section(json, "hotkeys");
    read(hotkeys, "record", settings.hotkeys.record);
    read(hotkeys, "recordWithMode", settings.hotkeys.recordWithMode);
    read(hotkeys, "pause", settings.hotkeys.pause);
    read(hotkeys, "marker", settings.hotkeys.marker);
    read(hotkeys, "panel", settings.hotkeys.panel);
    read(hotkeys, "saveInstant", settings.hotkeys.saveInstant);

    auto const& hud = section(json, "hud");
    read(hud, "enabled", settings.hud.enabled);
    settings.hud.corner = parseCorner(readString(hud, "corner")).value_or(settings.hud.corner);
    read(hud, "opacity", settings.hud.opacity);
    read(hud, "scale", settings.hud.scale);
    read(hud, "hideIndicator", settings.hud.hideIndicator);
    settings.hud.opacity = std::clamp(settings.hud.opacity, 0.2f, 1.0f);
    settings.hud.scale   = std::clamp(settings.hud.scale, 0.5f, 2.0f);
    if (auto const it = hud.find("show"); it != hud.end() && it->is_array()) {
        auto has = [&](std::string_view item) {
            return std::ranges::any_of(*it, [&](Json const& v) { return v.is_string() && v.get<std::string>() == item; });
        };
        settings.hud.showElapsed   = has("elapsed");
        settings.hud.showFreeSpace = has("freeSpace");
        settings.hud.showTps       = has("tps");
        settings.hud.showConverter = has("converter");
        settings.hud.showMarkers   = has("markers");
    }

    auto const& storage = section(json, "storage");
    read(storage, "warnFreeGB", settings.storage.warnFreeGB);
    read(storage, "stopFreeGB", settings.storage.stopFreeGB);
    settings.storage.stopFreeGB = std::max(0.0, settings.storage.stopFreeGB);
    settings.storage.warnFreeGB = std::max(settings.storage.stopFreeGB, settings.storage.warnFreeGB);

    auto const& live = section(json, "liveStream");
    read(live, "enabled", settings.liveStream.enabled);
    read(live, "host", settings.liveStream.host);
    read(live, "port", settings.liveStream.port);
    read(live, "launchCommand", settings.liveStream.launchCommand);
    read(live, "maxQueuedMegabytes", settings.liveStream.maxQueuedMegabytes);

    auto const& instant = section(json, "instantReplay");
    read(instant, "enabled", settings.instantReplay.enabled);
    read(instant, "minutes", settings.instantReplay.minutes);
    read(instant, "maxMegabytes", settings.instantReplay.maxMegabytes);
    read(instant, "chunkSeconds", settings.instantReplay.chunkSeconds);
    settings.instantReplay.minutes      = std::clamp(settings.instantReplay.minutes, 1, 30);
    settings.instantReplay.maxMegabytes = std::clamp(settings.instantReplay.maxMegabytes, 256, 16384);
    settings.instantReplay.chunkSeconds = std::clamp(settings.instantReplay.chunkSeconds, 10, 120);

    auto const& autoRecord = section(json, "autoRecord");
    read(autoRecord, "enabled", settings.autoRecord.enabled);
    if (auto const it = autoRecord.find("rules"); it != autoRecord.end() && it->is_array()) {
        for (auto const& entry : *it) {
            if (!entry.is_object()) continue;
            AutoRecordRule rule;
            read(entry, "name", rule.name);
            read(entry, "world", rule.world);
            read(entry, "server", rule.server);
            read(entry, "dimension", rule.dimension);
            rule.action = parseAutoRecordAction(readString(entry, "action")).value_or(rule.action);
            if (auto const mode = parseOutputMode(readString(entry, "output"))) rule.output = *mode;
            settings.autoRecord.rules.push_back(std::move(rule));
        }
    }

    read(section(json, "ui"), "reduceMotion", settings.ui.reduceMotion);
    return settings;
}

// Writes the known keys over `json`, leaving every other key in place.
void toJson(Settings const& settings, Json& json) {
    if (!json.is_object()) json = Json::object();
    auto sectionOf = [&json](char const* key) -> Json& {
        auto& value = json[key];
        if (!value.is_object()) value = Json::object();
        return value;
    };
    json["version"]  = settings.version;
    json["language"] = settings.language;

    sectionOf("recording")["compatibleFormat"] = settings.recording.compatibleFormat;

    auto& output         = sectionOf("output");
    output["mode"]       = toString(settings.output.mode);
    output["convert"]    = toString(settings.output.convert);
    output["sourceKeep"] = settings.output.sourceKeep.toString();
    output["askOnStart"] = settings.output.askOnStart;

    auto& time              = sectionOf("timeNormalization");
    time["mode"]            = settings.timeNormalization.mode;
    time["lagMarkers"]      = settings.timeNormalization.lagMarkers;
    time["lagThresholdTps"] = settings.timeNormalization.lagThresholdTps;
    time["maxGapSeconds"]   = settings.timeNormalization.maxGapSeconds;

    auto& hotkeys             = sectionOf("hotkeys");
    hotkeys["record"]         = settings.hotkeys.record;
    hotkeys["recordWithMode"] = settings.hotkeys.recordWithMode;
    hotkeys["pause"]          = settings.hotkeys.pause;
    hotkeys["marker"]         = settings.hotkeys.marker;
    hotkeys["panel"]          = settings.hotkeys.panel;
    hotkeys["saveInstant"]    = settings.hotkeys.saveInstant;

    auto& hud            = sectionOf("hud");
    hud["enabled"]       = settings.hud.enabled;
    hud["corner"]        = cornerName(settings.hud.corner);
    hud["opacity"]       = settings.hud.opacity;
    hud["scale"]         = settings.hud.scale;
    hud["hideIndicator"] = settings.hud.hideIndicator;
    Json show            = Json::array();
    if (settings.hud.showElapsed) show.push_back("elapsed");
    if (settings.hud.showFreeSpace) show.push_back("freeSpace");
    if (settings.hud.showTps) show.push_back("tps");
    if (settings.hud.showConverter) show.push_back("converter");
    if (settings.hud.showMarkers) show.push_back("markers");
    hud["show"] = std::move(show);

    auto& storage         = sectionOf("storage");
    storage["warnFreeGB"] = settings.storage.warnFreeGB;
    storage["stopFreeGB"] = settings.storage.stopFreeGB;

    auto& live                 = sectionOf("liveStream");
    live["enabled"]            = settings.liveStream.enabled;
    live["host"]               = settings.liveStream.host;
    live["port"]               = settings.liveStream.port;
    live["launchCommand"]      = settings.liveStream.launchCommand;
    live["maxQueuedMegabytes"] = settings.liveStream.maxQueuedMegabytes;

    auto& instant           = sectionOf("instantReplay");
    instant["enabled"]      = settings.instantReplay.enabled;
    instant["minutes"]      = settings.instantReplay.minutes;
    instant["maxMegabytes"] = settings.instantReplay.maxMegabytes;
    instant["chunkSeconds"] = settings.instantReplay.chunkSeconds;

    auto& autoRecord      = sectionOf("autoRecord");
    autoRecord["enabled"] = settings.autoRecord.enabled;
    Json rules            = Json::array();
    for (auto const& rule : settings.autoRecord.rules) {
        Json entry{
            {"name",      rule.name                     },
            {"world",     rule.world                    },
            {"server",    rule.server                   },
            {"dimension", rule.dimension                },
            {"action",    toString(rule.action)         }
        };
        if (rule.output) entry["output"] = toString(*rule.output);
        rules.push_back(std::move(entry));
    }
    autoRecord["rules"] = std::move(rules);

    sectionOf("ui")["reduceMotion"] = settings.ui.reduceMotion;
}

std::string readFile(std::filesystem::path const& file) {
    std::ifstream      in(file, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

} // namespace

std::string_view toString(OutputMode mode) {
    switch (mode) {
    case OutputMode::Playback:
        return "playback";
    case OutputMode::Both:
        return "both";
    case OutputMode::Flashback:
        return "flashback";
    }
    return "both";
}

std::optional<OutputMode> parseOutputMode(std::string_view text) {
    auto const value = lower(trim(text));
    if (value == "playback") return OutputMode::Playback;
    if (value == "both") return OutputMode::Both;
    if (value == "flashback") return OutputMode::Flashback;
    return std::nullopt;
}

std::string_view toString(AutoRecordAction action) {
    switch (action) {
    case AutoRecordAction::Record:
        return "record";
    case AutoRecordAction::Ask:
        return "ask";
    case AutoRecordAction::Ignore:
        return "ignore";
    }
    return "record";
}

std::optional<AutoRecordAction> parseAutoRecordAction(std::string_view text) {
    auto const value = lower(trim(text));
    if (value == "record") return AutoRecordAction::Record;
    if (value == "ask") return AutoRecordAction::Ask;
    if (value == "ignore") return AutoRecordAction::Ignore;
    return std::nullopt;
}

bool globMatch(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) return true;
    auto const p = lower(pattern);
    auto const t = lower(text);
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::string_view toString(ConvertTiming timing) { return timing == ConvertTiming::Live ? "live" : "afterStop"; }

std::optional<ConvertTiming> parseConvertTiming(std::string_view text) {
    auto const value = lower(trim(text));
    if (value == "live") return ConvertTiming::Live;
    if (value == "afterstop") return ConvertTiming::AfterStop;
    return std::nullopt;
}

std::string SourceKeep::toString() const {
    switch (kind) {
    case Kind::Days:
        return "days:" + std::to_string(days);
    case Kind::Always:
        return "always";
    case Kind::AfterVerified:
        return "afterVerified";
    }
    return "days:7";
}

std::optional<SourceKeep> SourceKeep::parse(std::string_view text) {
    auto const value = trim(text);
    if (value == "always") return SourceKeep{Kind::Always, 0};
    if (value == "afterVerified") return SourceKeep{Kind::AfterVerified, 0};
    if (value.starts_with("days:")) {
        int        days{};
        auto const digits = std::string_view(value).substr(5);
        auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), days);
        if (ec == std::errc{} && ptr == digits.data() + digits.size() && days >= 1 && days <= 3650) {
            return SourceKeep{Kind::Days, days};
        }
    }
    return std::nullopt;
}

std::string Hotkey::toString() const {
    if (!valid()) return {};
    std::string text;
    if (ctrl) text += "Ctrl+";
    if (shift) text += "Shift+";
    if (alt) text += "Alt+";
    return text + keyName(key);
}

std::optional<Hotkey> Hotkey::parse(std::string_view text) {
    Hotkey      hotkey;
    std::string part;
    std::string remaining = trim(text);
    if (remaining.empty() || lower(remaining) == "none") return Hotkey{};
    std::stringstream stream(remaining);
    std::vector<std::string> parts;
    while (std::getline(stream, part, '+')) parts.push_back(trim(part));
    if (parts.empty()) return std::nullopt;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        auto const modifier = lower(parts[i]);
        if (modifier == "ctrl" || modifier == "control") hotkey.ctrl = true;
        else if (modifier == "shift") hotkey.shift = true;
        else if (modifier == "alt") hotkey.alt = true;
        else return std::nullopt;
    }
    auto const code = keyCode(parts.back());
    if (!code) return std::nullopt;
    hotkey.key = *code;
    return hotkey;
}

SettingsStore& SettingsStore::getInstance() {
    static SettingsStore instance;
    return instance;
}

void SettingsStore::load(std::filesystem::path const& dataDir) {
    std::scoped_lock lock(mMutex);
    mFile = dataDir / "settings.json";

    Json            json = Json::object();
    std::error_code ec;
    if (std::filesystem::exists(mFile, ec)) {
        auto parsed = Json::parse(readFile(mFile), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            getLogger().warn("settings.json is not valid JSON; using defaults (the broken file is kept as settings.json.bad)");
            std::filesystem::copy_file(mFile, dataDir / "settings.json.bad", std::filesystem::copy_options::overwrite_existing, ec);
        } else {
            json = std::move(parsed);
        }
    }

    // Live-stream builds before settings.json kept the converter settings in live.json (FlashBridge's setup-live
    // still writes it for them); it is only read while settings.json has no liveStream section yet.
    if (!json.contains("liveStream") && std::filesystem::exists(dataDir / "live.json", ec)) {
        auto live = Json::parse(readFile(dataDir / "live.json"), nullptr, false);
        if (!live.is_discarded() && live.is_object()) {
            json["liveStream"] = std::move(live);
            getLogger().info("Imported the live stream settings from live.json into settings.json");
        }
    }

    mSettings = fromJson(json);
    mRawJson  = json.dump();
    saveLocked();
}

Settings SettingsStore::get() const {
    std::scoped_lock lock(mMutex);
    return mSettings;
}

std::filesystem::path SettingsStore::file() const {
    std::scoped_lock lock(mMutex);
    return mFile;
}

void SettingsStore::update(std::function<void(Settings&)> const& change) {
    std::function<void(Settings const&)> listener;
    Settings                             copy;
    {
        std::scoped_lock lock(mMutex);
        change(mSettings);
        saveLocked();
        listener = mListener;
        copy     = mSettings;
    }
    if (listener) listener(copy);
}

void SettingsStore::setListener(std::function<void(Settings const&)> listener) {
    std::scoped_lock lock(mMutex);
    mListener = std::move(listener);
}

void SettingsStore::saveLocked() {
    if (mFile.empty()) return;
    auto json = Json::parse(mRawJson, nullptr, false);
    if (json.is_discarded()) json = Json::object();
    toJson(mSettings, json);
    auto const text = json.dump(4);

    std::error_code ec;
    std::filesystem::create_directories(mFile.parent_path(), ec);
    auto const temp = mFile.parent_path() / "settings.json.tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            getLogger().warn("Unable to write {}", temp.string());
            return;
        }
    }
    std::filesystem::rename(temp, mFile, ec);
    if (ec) {
        getLogger().warn("Unable to replace {}: {}", mFile.string(), ec.message());
        return;
    }
    mRawJson = json.dump();
}

} // namespace playback::settings
