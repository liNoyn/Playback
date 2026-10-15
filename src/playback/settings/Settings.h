#pragma once

#include "playback/io/LiveStream.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback::settings {

// What a recording leaves behind (docs/design/recording-ux-and-time-normalization.md §5.7).
enum class OutputMode { Playback, Both, Flashback };
// When the Flashback replay is made: streamed while recording, or from the exported file after it stops.
enum class ConvertTiming { Live, AfterStop };
enum class HudCorner { TopLeft, TopRight, BottomLeft, BottomRight };
// What an auto-record rule does when the world you joined matches it.
enum class AutoRecordAction { Record, Ask, Ignore };

// Case-insensitive match with `*` (any text) and `?` (one character); an empty pattern matches anything.
[[nodiscard]] bool globMatch(std::string_view pattern, std::string_view text);

[[nodiscard]] std::string_view                toString(AutoRecordAction action);
[[nodiscard]] std::optional<AutoRecordAction> parseAutoRecordAction(std::string_view text);

// Joining a world whose name / server / dimension match all non-empty fields applies the first matching rule.
struct AutoRecordRule {
    std::string                name;
    std::string                world;     // level name
    std::string                server;    // server name as the game shows it
    std::string                dimension; // dimension name, e.g. Overworld
    AutoRecordAction           action = AutoRecordAction::Record;
    std::optional<OutputMode>  output;    // none: the default output
};

[[nodiscard]] std::string_view           toString(OutputMode mode);
[[nodiscard]] std::optional<OutputMode>  parseOutputMode(std::string_view text);
[[nodiscard]] std::string_view           toString(ConvertTiming timing);
[[nodiscard]] std::optional<ConvertTiming> parseConvertTiming(std::string_view text);

// What happens to the Playback recording of a "Flashback only" recording once its conversion was verified.
struct SourceKeep {
    enum class Kind { Days, Always, AfterVerified };
    Kind kind = Kind::Days;
    int  days = 7;

    [[nodiscard]] std::string                      toString() const;
    [[nodiscard]] static std::optional<SourceKeep> parse(std::string_view text);
};

// "F8", "Ctrl+F8", "Shift+F8"; key codes are Windows virtual keys (the codes MCBE's key events carry).
struct Hotkey {
    int  key{};
    bool ctrl{};
    bool shift{};
    bool alt{};

    [[nodiscard]] bool                         valid() const { return key != 0; }
    [[nodiscard]] std::string                  toString() const;
    [[nodiscard]] static std::optional<Hotkey> parse(std::string_view text);
};

struct Settings {
    int         version  = 1;
    std::string language = "auto"; // auto | ja_JP | en_US | zh_CN

    struct Recording {
        bool compatibleFormat = false; // true: no tick_info, readable by upstream Playback
    } recording;

    struct Output {
        OutputMode    mode    = OutputMode::Both;
        ConvertTiming convert = ConvertTiming::Live;
        SourceKeep    sourceKeep;
        bool          askOnStart = false;
    } output;

    struct TimeNormalization {
        std::string mode            = "game"; // game | wall | raw (applied by FlashBridge)
        bool        lagMarkers      = true;
        double      lagThresholdTps = 15;
        double      maxGapSeconds   = 10;
    } timeNormalization;

    struct Hotkeys {
        std::string record         = "F8";
        std::string recordWithMode = "Ctrl+F8";
        std::string pause          = "Shift+F8";
        std::string marker         = "F7";
        std::string panel          = "F6";
        std::string saveInstant    = "F9";
        std::string toggleInstant  = "Shift+F9";
    } hotkeys;

    struct Hud {
        bool      enabled       = true;
        HudCorner corner        = HudCorner::TopRight;
        float     opacity       = 0.9f;
        float     scale         = 1.0f;
        bool      hideIndicator = false; // keeps the indicator out of screen captures (OBS)
        bool      showElapsed   = true;
        bool      showFreeSpace = true;
        bool      showTps       = true;
        bool      showConverter = true;
        bool      showMarkers   = true;
    } hud;

    struct Storage {
        double warnFreeGB = 3;
        double stopFreeGB = 1;
    } storage;

    // Like NVIDIA Instant Replay: the last stretch is always kept (also while recording) and the hotkey saves it.
    struct InstantReplay {
        bool enabled         = false;
        int  seconds         = 120; // how much is kept and saved (15 s - 20 min)
        int  maxMegabytes    = 2048;
        int  chunkSeconds    = 30;
    } instantReplay;

    struct AutoRecord {
        bool                        enabled = true;
        std::vector<AutoRecordRule> rules;
    } autoRecord;

    io::LiveStreamConfig liveStream;

    struct Ui {
        bool reduceMotion = false;
    } ui;
};

// settings.json in the mod's data folder. Reads are cheap copies; every change is saved at once (temp file +
// rename). Keys this version does not know are kept, so an older build never drops a newer build's settings.
class SettingsStore {
public:
    [[nodiscard]] static SettingsStore& getInstance();

    // Reads the file (bringing in an existing live.json the first time) and writes it back normalized.
    void load(std::filesystem::path const& dataDir);

    [[nodiscard]] Settings              get() const;
    [[nodiscard]] std::filesystem::path file() const;

    void update(std::function<void(Settings&)> const& change);

    // Called after every change (on the thread that made it).
    void setListener(std::function<void(Settings const&)> listener);

private:
    SettingsStore() = default;
    void saveLocked();

    mutable std::mutex                     mMutex;
    Settings                               mSettings;
    std::string                            mRawJson; // last file contents, for unknown keys
    std::filesystem::path                  mFile;
    std::function<void(Settings const&)> mListener;
};

} // namespace playback::settings
