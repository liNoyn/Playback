#include "RecordingOverlay.h"

#include "playback/Playback.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/record/RecordControl.h"
#include "playback/record/RecordingStatus.h"
#include "playback/settings/Settings.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace playback::editor::ui::recording {

using namespace ll::i18n_literals;
using record::RecordCommand;
using record::RecordingPhase;
using record::ToastKind;
using settings::ConvertTiming;
using settings::OutputMode;

namespace {

// Design tokens (docs/design/recording-ux-and-time-normalization.md §6.2).
constexpr ImU32 kGround   = IM_COL32(0x14, 0x16, 0x1A, 0xff);
constexpr ImU32 kSurface  = IM_COL32(0x1B, 0x1E, 0x23, 0xff);
constexpr ImU32 kRaised   = IM_COL32(0x23, 0x27, 0x2E, 0xff);
constexpr ImU32 kEdge     = IM_COL32(0x2E, 0x33, 0x3B, 0xff);
constexpr ImU32 kInk      = IM_COL32(0xE6, 0xE9, 0xEE, 0xff);
constexpr ImU32 kInkDim   = IM_COL32(0x9A, 0xA3, 0xAF, 0xff);
constexpr ImU32 kBlue     = IM_COL32(0x4C, 0x8D, 0xFF, 0xff);
constexpr ImU32 kRed      = IM_COL32(0xE5, 0x48, 0x4D, 0xff);
constexpr ImU32 kGreen    = IM_COL32(0x46, 0xC3, 0x7B, 0xff);
constexpr ImU32 kAmber    = IM_COL32(0xF2, 0xA3, 0x3C, 0xff);

ImVec4 color(ImU32 value, float alpha = 1.0f) {
    auto c = ImGui::ColorConvertU32ToFloat4(value);
    c.w *= alpha;
    return c;
}

std::string elapsedText(double seconds) {
    auto const total = static_cast<long long>(seconds);
    if (total >= 3600) return std::format("{}:{:02}:{:02}", total / 3600, total / 60 % 60, total % 60);
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

ImU32 toastColor(ToastKind kind) {
    switch (kind) {
    case ToastKind::Success:
        return kGreen;
    case ToastKind::Warning:
        return kAmber;
    case ToastKind::Error:
        return kRed;
    case ToastKind::Info:
    default:
        return kBlue;
    }
}

// The converter line of the HUD and the panel; empty when nothing is converted.
std::string converterText(record::RecordingStatusSnapshot const& status, ImU32& tone) {
    tone = kInkDim;
    if (status.phase == RecordingPhase::Buffering || status.mode == OutputMode::Playback) return {};
    if (status.convert == ConvertTiming::AfterStop && status.phase != RecordingPhase::Idle) {
        return "playback.record.converter.afterStop"_tr();
    }
    auto const& converter = status.converter;
    if (!converter.connected) {
        if (status.phase == RecordingPhase::Idle) return {};
        tone = kAmber;
        return "playback.record.converter.waiting"_tr();
    }
    if (converter.state == "converting") {
        tone = kGreen;
        return "playback.record.converter.converting"_tr();
    }
    if (converter.state == "convertingFile") return "playback.record.converter.convertingFile"_tr();
    if (converter.state == "behind") {
        tone = kAmber;
        return "playback.record.converter.behind"_tr();
    }
    if (converter.state == "done") {
        tone = kGreen;
        return "playback.record.converter.done"_tr();
    }
    if (converter.state == "failed") {
        tone = kRed;
        return "playback.record.converter.failed"_tr();
    }
    return converter.state;
}

void openPath(std::filesystem::path const& path, bool select) {
    std::wstring arguments = select ? L"/select,\"" + path.wstring() + L"\"" : L"\"" + path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

// A row of mutually exclusive buttons; returns true when the selection changed.
template <class T>
bool segmented(char const* id, T& value, std::vector<std::pair<T, std::string>> const& options, bool enabled = true) {
    bool changed = false;
    ImGui::PushID(id);
    if (!enabled) ImGui::BeginDisabled();
    for (size_t i = 0; i < options.size(); ++i) {
        if (i > 0) ImGui::SameLine(0, 2);
        bool const selected = options[i].first == value;
        ImGui::PushStyleColor(ImGuiCol_Button, color(selected ? kBlue : kRaised));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(selected ? kBlue : kEdge));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, color(kBlue));
        if (ImGui::Button(options[i].second.c_str()) && !selected) {
            value   = options[i].first;
            changed = true;
        }
        ImGui::PopStyleColor(3);
    }
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

bool primaryButton(char const* label, ImU32 tone, ImVec2 size = {0, 0}) {
    ImGui::PushStyleColor(ImGuiCol_Button, color(tone));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(tone, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color(tone, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_Text, color(IM_COL32_WHITE));
    bool const pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void label(std::string const& text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(color(kInkDim), "%s", text.c_str());
    ImGui::SameLine(ImGui::GetFontSize() * 8.5f);
}

void heading(std::string const& text) {
    ImGui::Spacing();
    ImGui::TextColored(color(kInk), "%s", text.c_str());
    ImGui::Separator();
}

std::vector<std::pair<OutputMode, std::string>> outputOptions() {
    return {
        {OutputMode::Playback,  "playback.record.output.playback"_tr() },
        {OutputMode::Both,      "playback.record.output.both"_tr()     },
        {OutputMode::Flashback, "playback.record.output.flashback"_tr()},
    };
}

std::vector<std::pair<ConvertTiming, std::string>> convertOptions() {
    return {
        {ConvertTiming::Live,      "playback.record.convert.live"_tr()     },
        {ConvertTiming::AfterStop, "playback.record.convert.afterStop"_tr()},
    };
}

// Output choice for the next recording; starts from the defaults each time the panel opens.
struct PanelChoice {
    bool          initialized{};
    OutputMode    mode{OutputMode::Both};
    ConvertTiming convert{ConvertTiming::Live};
};
PanelChoice gChoice;
int         gTab{};

void drawRecordTab(record::RecordingStatusSnapshot const& status, settings::Settings const& config) {
    auto& control = record::RecordControl::getInstance();
    float const width = ImGui::GetContentRegionAvail().x;
    bool const  flashBridge = config.liveStream.enabled && !config.liveStream.launchCommand.empty();

    bool const buffering = status.phase == RecordingPhase::Buffering;
    if (status.phase == RecordingPhase::Idle || buffering) {
        auto const text = std::format("{}  {}   ({})", ICON_TRACK_ACTIVE, "playback.record.panel.start"_tr(), config.hotkeys.record);
        if (primaryButton(text.c_str(), kRed, {width, ImGui::GetFontSize() * 2.4f})) {
            control.submit({RecordCommand::Start, record::RecordingOptions{gChoice.mode, gChoice.convert}});
        }
        if (buffering) {
            ImGui::TextColored(color(kInkDim), "%s", "playback.record.panel.startReplacesBuffer"_tr().c_str());
        }
    } else {
        float const third = (width - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
        ImVec2 const size{third, ImGui::GetFontSize() * 2.4f};
        bool const   saving = status.phase == RecordingPhase::Saving;
        if (saving) ImGui::BeginDisabled();
        if (primaryButton(std::format("{}  {}", ICON_STOP, "playback.record.panel.stop"_tr()).c_str(), kRed, size)) {
            control.submit({RecordCommand::Stop});
        }
        ImGui::SameLine();
        bool const paused = status.phase == RecordingPhase::Paused;
        auto const pauseText = paused ? std::format("{}  {}", ICON_PLAY, "playback.record.panel.resume"_tr())
                                      : std::format("{}  {}", ICON_PAUSE, "playback.record.panel.pause"_tr());
        if (primaryButton(pauseText.c_str(), kRaised, size)) control.submit({RecordCommand::TogglePause});
        ImGui::SameLine();
        if (primaryButton(std::format("{}  {}", ICON_ADD_MARKER, "playback.record.panel.marker"_tr()).c_str(), kRaised, size)) {
            control.submit({RecordCommand::Marker});
        }
        if (saving) ImGui::EndDisabled();
    }

    ImGui::Spacing();
    bool const idle = status.phase == RecordingPhase::Idle || buffering;
    OutputMode    mode    = idle ? gChoice.mode : status.mode;
    ConvertTiming convert = idle ? gChoice.convert : status.convert;

    label("playback.record.panel.output"_tr());
    if (segmented("output", mode, outputOptions(), idle && flashBridge)) gChoice.mode = mode;
    label("playback.record.panel.convert"_tr());
    if (segmented("convert", convert, convertOptions(), idle && flashBridge && mode != OutputMode::Playback)) {
        gChoice.convert = convert;
    }
    if (idle && flashBridge && (gChoice.mode != config.output.mode || gChoice.convert != config.output.convert)) {
        ImGui::Dummy({ImGui::GetFontSize() * 8.5f - ImGui::GetStyle().ItemSpacing.x, 0});
        ImGui::SameLine();
        if (ImGui::SmallButton("playback.record.panel.saveAsDefault"_tr().c_str())) {
            settings::SettingsStore::getInstance().update([](settings::Settings& s) {
                s.output.mode    = gChoice.mode;
                s.output.convert = gChoice.convert;
            });
        }
    }
    if (mode == OutputMode::Flashback) {
        ImGui::TextColored(color(kInkDim), "%s", "playback.record.panel.flashbackOnlyNote"_tr(config.output.sourceKeep.days).c_str());
    }

    if (config.instantReplay.enabled || buffering) {
        heading("playback.record.panel.instantReplay"_tr());
        if (buffering) {
            ImGui::TextColored(color(kBlue), "%s %s", ICON_TRACK_ACTIVE, "playback.record.panel.bufferingNote"_tr(config.instantReplay.minutes).c_str());
            float const half = (width - ImGui::GetStyle().ItemSpacing.x) / 2;
            auto const  save = std::format("{}  {}   ({})", ICON_SAVE, "playback.record.panel.saveInstant"_tr(config.instantReplay.minutes), config.hotkeys.saveInstant);
            if (primaryButton(save.c_str(), kBlue, {half, ImGui::GetFontSize() * 2.0f})) control.submit({RecordCommand::SaveInstant});
            ImGui::SameLine();
            if (primaryButton(std::format("{}  {}", ICON_STOP, "playback.record.panel.stopBuffer"_tr()).c_str(), kRaised, {half, ImGui::GetFontSize() * 2.0f})) {
                control.submit({RecordCommand::StopBuffer});
            }
        } else if (status.phase == RecordingPhase::Idle) {
            if (primaryButton("playback.record.panel.startBuffer"_tr().c_str(), kRaised)) control.submit({RecordCommand::StartBuffer});
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    label("playback.record.panel.state"_tr());
    switch (status.phase) {
    case RecordingPhase::Buffering:
        ImGui::TextColored(color(kBlue), "%s %s  %s", ICON_TRACK_ACTIVE, "playback.record.state.buffering"_tr().c_str(), elapsedText(status.elapsedSeconds).c_str());
        break;
    case RecordingPhase::Idle:
        ImGui::TextColored(color(kInkDim), "%s", "playback.record.state.idle"_tr().c_str());
        break;
    case RecordingPhase::Recording:
        ImGui::TextColored(color(kRed), "%s %s  %s", ICON_TRACK_ACTIVE, "playback.record.state.recording"_tr().c_str(), elapsedText(status.elapsedSeconds).c_str());
        break;
    case RecordingPhase::Paused:
        ImGui::TextColored(color(kAmber), "%s %s  %s", ICON_PAUSE, "playback.record.state.paused"_tr().c_str(), elapsedText(status.elapsedSeconds).c_str());
        break;
    case RecordingPhase::Saving:
        ImGui::TextColored(color(kBlue), "%s", "playback.record.state.saving"_tr().c_str());
        break;
    }
    if (status.phase != RecordingPhase::Idle) {
        label("playback.record.panel.markers"_tr());
        ImGui::Text("%d", status.markers);
        label("playback.record.panel.tps"_tr());
        if (status.tps) {
            ImGui::TextColored(color(*status.tps < config.timeNormalization.lagThresholdTps ? kAmber : kInk), "%.1f", *status.tps);
        } else {
            ImGui::TextColored(color(kInkDim), "-");
        }
    }
    label("playback.record.panel.flashBridge"_tr());
    if (!flashBridge) {
        ImGui::TextColored(color(kAmber), "%s", "playback.record.panel.flashBridgeMissing"_tr().c_str());
    } else {
        ImU32      tone = kInkDim;
        auto const text = converterText(status, tone);
        ImGui::TextColored(color(tone), "%s", text.empty() ? "playback.record.panel.flashBridgeReady"_tr().c_str() : text.c_str());
    }
    label("playback.record.panel.freeSpace"_tr());
    if (status.freeGB) ImGui::Text("%.1f GB", *status.freeGB);
    else ImGui::TextColored(color(kInkDim), "-");
}

// Rules are checked in order; the first that matches the world joined decides.
void drawAutoRecordRules(settings::Settings const& config) {
    auto& store = settings::SettingsStore::getInstance();
    auto  edit  = [&store](auto&& change) { store.update(change); };

    int  remove = -1;
    auto actions = std::vector<std::pair<settings::AutoRecordAction, std::string>>{
        {settings::AutoRecordAction::Record, "playback.record.auto.record"_tr()},
        {settings::AutoRecordAction::Ask,    "playback.record.auto.ask"_tr()   },
        {settings::AutoRecordAction::Ignore, "playback.record.auto.ignore"_tr()},
    };
    for (size_t i = 0; i < config.autoRecord.rules.size(); ++i) {
        auto const& rule = config.autoRecord.rules[i];
        ImGui::PushID(static_cast<int>(i));
        std::string summary = rule.name.empty() ? "playback.record.auto.unnamed"_tr() : rule.name;
        summary += "  ";
        if (!rule.world.empty()) summary += "playback.record.auto.worldIs"_tr(rule.world) + " ";
        if (!rule.server.empty()) summary += "playback.record.auto.serverIs"_tr(rule.server) + " ";
        if (!rule.dimension.empty()) summary += "playback.record.auto.dimensionIs"_tr(rule.dimension);
        if (rule.world.empty() && rule.server.empty() && rule.dimension.empty()) summary += "playback.record.auto.everywhere"_tr();
        ImGui::TextColored(color(kInk), "%s", summary.c_str());
        auto action = rule.action;
        if (segmented<settings::AutoRecordAction>("action", action, actions)) {
            edit([i, action](settings::Settings& s) {
                if (i < s.autoRecord.rules.size()) s.autoRecord.rules[i].action = action;
            });
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_TRASH)) remove = static_cast<int>(i);
        if (rule.action == settings::AutoRecordAction::Record) {
            // Output of the automatic recording: the default, or one mode for this rule
            std::vector<std::pair<int, std::string>> outputs{
                {-1, "playback.record.auto.defaultOutput"_tr()},
                {0, "playback.record.output.playback"_tr()},
                {1, "playback.record.output.both"_tr()},
                {2, "playback.record.output.flashback"_tr()},
            };
            int selected = rule.output ? static_cast<int>(*rule.output) : -1;
            if (segmented<int>("output", selected, outputs)) {
                edit([i, selected](settings::Settings& s) {
                    if (i >= s.autoRecord.rules.size()) return;
                    if (selected < 0) s.autoRecord.rules[i].output.reset();
                    else s.autoRecord.rules[i].output = static_cast<settings::OutputMode>(selected);
                });
            }
        }
        ImGui::PopID();
        ImGui::Separator();
    }
    if (remove >= 0) {
        edit([remove](settings::Settings& s) {
            if (static_cast<size_t>(remove) < s.autoRecord.rules.size()) s.autoRecord.rules.erase(s.autoRecord.rules.begin() + remove);
        });
    }

    auto const where = record::RecordControl::getInstance().currentWorld();
    if (where.world.empty() && where.server.empty()) return;
    auto add = [&](bool useServer) {
        settings::AutoRecordRule rule;
        if (useServer && !where.server.empty()) {
            rule.server = where.server;
            rule.name   = where.server;
        } else {
            rule.world = where.world;
            rule.name  = where.world;
        }
        edit([rule](settings::Settings& s) { s.autoRecord.rules.push_back(rule); });
    };
    if (!where.world.empty() && ImGui::Button("playback.record.auto.addWorld"_tr(where.world).c_str())) add(false);
    if (!where.server.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("playback.record.auto.addServer"_tr(where.server).c_str())) add(true);
    }
}

void drawSettingsTab(settings::Settings const& config) {
    auto& store = settings::SettingsStore::getInstance();
    auto  edit  = [&store](auto&& change) { store.update(change); };

    heading("playback.record.settings.recording"_tr());
    bool compatible = config.recording.compatibleFormat;
    if (ImGui::Checkbox("playback.record.settings.compatibleFormat"_tr().c_str(), &compatible)) {
        edit([compatible](settings::Settings& s) { s.recording.compatibleFormat = compatible; });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "playback.record.settings.compatibleFormatHelp"_tr().c_str());

    heading("playback.record.settings.output"_tr());
    OutputMode mode = config.output.mode;
    label("playback.record.settings.defaultOutput"_tr());
    if (segmented("defaultOutput", mode, outputOptions())) edit([mode](settings::Settings& s) { s.output.mode = mode; });
    ConvertTiming convert = config.output.convert;
    label("playback.record.settings.defaultConvert"_tr());
    if (segmented("defaultConvert", convert, convertOptions())) {
        edit([convert](settings::Settings& s) { s.output.convert = convert; });
    }
    using Keep = settings::SourceKeep::Kind;
    Keep keep  = config.output.sourceKeep.kind;
    label("playback.record.settings.sourceKeep"_tr());
    if (segmented<Keep>(
            "sourceKeep",
            keep,
            {{Keep::Days, "playback.record.settings.keepDays"_tr()},
             {Keep::Always, "playback.record.settings.keepAlways"_tr()},
             {Keep::AfterVerified, "playback.record.settings.keepNone"_tr()}}
        )) {
        edit([keep](settings::Settings& s) {
            s.output.sourceKeep.kind = keep;
            if (s.output.sourceKeep.days < 1) s.output.sourceKeep.days = 7;
        });
    }
    if (keep == Keep::Days) {
        int days = config.output.sourceKeep.days;
        label("");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::SliderInt("##keepDays", &days, 1, 90, "playback.record.settings.daysFormat"_tr().c_str())) {
            edit([days](settings::Settings& s) { s.output.sourceKeep.days = days; });
        }
    }

    heading("playback.record.settings.instantReplay"_tr());
    bool instantEnabled = config.instantReplay.enabled;
    if (ImGui::Checkbox("playback.record.settings.instantEnabled"_tr().c_str(), &instantEnabled)) {
        edit([instantEnabled](settings::Settings& s) { s.instantReplay.enabled = instantEnabled; });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "playback.record.settings.instantHelp"_tr().c_str());
    int instantMinutes = config.instantReplay.minutes;
    label("playback.record.settings.instantMinutes"_tr());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderInt("##instantMinutes", &instantMinutes, 1, 10, "playback.record.settings.minutesFormat"_tr().c_str())) {
        edit([instantMinutes](settings::Settings& s) { s.instantReplay.minutes = instantMinutes; });
    }
    int instantMegabytes = config.instantReplay.maxMegabytes;
    label("playback.record.settings.instantMegabytes"_tr());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderInt("##instantMegabytes", &instantMegabytes, 256, 8192, "%d MB")) {
        edit([instantMegabytes](settings::Settings& s) { s.instantReplay.maxMegabytes = instantMegabytes; });
    }

    heading("playback.record.settings.autoRecord"_tr());
    bool autoEnabled = config.autoRecord.enabled;
    if (ImGui::Checkbox("playback.record.settings.autoEnabled"_tr().c_str(), &autoEnabled)) {
        edit([autoEnabled](settings::Settings& s) { s.autoRecord.enabled = autoEnabled; });
    }
    ImGui::TextColored(color(kInkDim), "%s", "playback.record.settings.autoHelp"_tr().c_str());
    drawAutoRecordRules(config);

    heading("playback.record.settings.time"_tr());
    std::string timeMode = config.timeNormalization.mode;
    label("playback.record.settings.timeMode"_tr());
    if (segmented<std::string>(
            "timeMode",
            timeMode,
            {{"game", "playback.record.settings.timeGame"_tr()},
             {"wall", "playback.record.settings.timeWall"_tr()},
             {"raw", "playback.record.settings.timeRaw"_tr()}}
        )) {
        edit([timeMode](settings::Settings& s) { s.timeNormalization.mode = timeMode; });
    }
    bool lagMarkers = config.timeNormalization.lagMarkers;
    if (ImGui::Checkbox("playback.record.settings.lagMarkers"_tr().c_str(), &lagMarkers)) {
        edit([lagMarkers](settings::Settings& s) { s.timeNormalization.lagMarkers = lagMarkers; });
    }

    heading("playback.record.settings.hud"_tr());
    bool hudEnabled = config.hud.enabled;
    if (ImGui::Checkbox("playback.record.settings.hudEnabled"_tr().c_str(), &hudEnabled)) {
        edit([hudEnabled](settings::Settings& s) { s.hud.enabled = hudEnabled; });
    }
    bool hide = config.hud.hideIndicator;
    if (ImGui::Checkbox("playback.record.settings.hideIndicator"_tr().c_str(), &hide)) {
        edit([hide](settings::Settings& s) { s.hud.hideIndicator = hide; });
    }
    auto corner = config.hud.corner;
    label("playback.record.settings.corner"_tr());
    if (segmented<settings::HudCorner>(
            "corner",
            corner,
            {{settings::HudCorner::TopLeft, "playback.record.settings.topLeft"_tr()},
             {settings::HudCorner::TopRight, "playback.record.settings.topRight"_tr()},
             {settings::HudCorner::BottomLeft, "playback.record.settings.bottomLeft"_tr()},
             {settings::HudCorner::BottomRight, "playback.record.settings.bottomRight"_tr()}}
        )) {
        edit([corner](settings::Settings& s) { s.hud.corner = corner; });
    }
    float opacity = config.hud.opacity;
    label("playback.record.settings.opacity"_tr());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderFloat("##opacity", &opacity, 0.2f, 1.0f, "%.2f")) {
        edit([opacity](settings::Settings& s) { s.hud.opacity = opacity; });
    }
    struct Toggle {
        std::string        text;
        bool               value;
        bool settings::Settings::Hud::* member;
    };
    std::vector<Toggle> toggles{
        {"playback.record.settings.showElapsed"_tr(),   config.hud.showElapsed,   &settings::Settings::Hud::showElapsed  },
        {"playback.record.settings.showFreeSpace"_tr(), config.hud.showFreeSpace, &settings::Settings::Hud::showFreeSpace},
        {"playback.record.settings.showTps"_tr(),       config.hud.showTps,       &settings::Settings::Hud::showTps      },
        {"playback.record.settings.showConverter"_tr(), config.hud.showConverter, &settings::Settings::Hud::showConverter},
        {"playback.record.settings.showMarkers"_tr(),   config.hud.showMarkers,   &settings::Settings::Hud::showMarkers  },
    };
    for (size_t i = 0; i < toggles.size(); ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        auto& toggle = toggles[i];
        if (ImGui::Checkbox(toggle.text.c_str(), &toggle.value)) {
            edit([member = toggle.member, value = toggle.value](settings::Settings& s) { s.hud.*member = value; });
        }
    }

    heading("playback.record.settings.hotkeys"_tr());
    ImGui::TextColored(
        color(kInkDim),
        "%s",
        "playback.record.settings.hotkeyList"_tr(
            config.hotkeys.record,
            config.hotkeys.pause,
            config.hotkeys.marker,
            config.hotkeys.panel,
            config.hotkeys.recordWithMode,
            config.hotkeys.saveInstant
        )
            .c_str()
    );

    heading("playback.record.settings.storage"_tr());
    float warn = static_cast<float>(config.storage.warnFreeGB);
    float stop = static_cast<float>(config.storage.stopFreeGB);
    label("playback.record.settings.warnFree"_tr());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderFloat("##warn", &warn, 1.0f, 50.0f, "%.0f GB")) {
        edit([warn](settings::Settings& s) { s.storage.warnFreeGB = std::max<double>(warn, s.storage.stopFreeGB); });
    }
    label("playback.record.settings.stopFree"_tr());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::SliderFloat("##stop", &stop, 0.0f, 20.0f, "%.1f GB")) {
        edit([stop](settings::Settings& s) {
            s.storage.stopFreeGB = stop;
            s.storage.warnFreeGB = std::max<double>(s.storage.warnFreeGB, stop);
        });
    }

    heading("playback.record.settings.language"_tr());
    std::string language = config.language;
    if (segmented<std::string>(
            "language",
            language,
            {{"auto", "playback.record.settings.languageAuto"_tr()},
             {"ja_JP", "日本語"},
             {"en_US", "English"},
             {"zh_CN", "简体中文"}}
        )) {
        edit([language](settings::Settings& s) { s.language = language; });
    }
    ImGui::TextColored(color(kInkDim), "%s", "playback.record.settings.languageNote"_tr().c_str());

    ImGui::Spacing();
    if (ImGui::Button("playback.record.settings.openFile"_tr().c_str())) openPath(store.file(), true);
}

std::string diagnostics(record::RecordingStatusSnapshot const& status, settings::Settings const& config) {
    auto const& version = Playback::getInstance().getSelf().getManifest().version;
    std::string text    = std::format(
        "Playback {}\nGame {}\noutput {} / convert {} / sourceKeep {}\ntime {} lagMarkers {}\ncompatibleFormat {}\n"
        "liveStream enabled {} launchCommand {}\nhud enabled {} hideIndicator {}\n",
        version ? version->to_string() : "?",
        record::PlaybackMeta::currentGameVersion(),
        settings::toString(config.output.mode),
        settings::toString(config.output.convert),
        config.output.sourceKeep.toString(),
        config.timeNormalization.mode,
        config.timeNormalization.lagMarkers,
        config.recording.compatibleFormat,
        config.liveStream.enabled,
        config.liveStream.launchCommand.empty() ? "(none)" : "(set)",
        config.hud.enabled,
        config.hud.hideIndicator
    );
    auto const& converter = status.converter;
    text += std::format(
        "FlashBridge connected {} state {} version {} ticks {} queued {:.1f} MB\n",
        converter.connected,
        converter.state,
        converter.version,
        converter.ticks,
        converter.queuedMB
    );
    if (converter.lastResult) {
        text += std::format(
            "last result ok {} verified {} fromExportedFile {} outputs {} error {}\n",
            converter.lastResult->ok,
            converter.lastResult->verified,
            converter.lastResult->fromExportedFile,
            converter.lastResult->outputs.size(),
            converter.lastResult->error
        );
    }
    return text;
}

void drawStatusTab(record::RecordingStatusSnapshot const& status, settings::Settings const& config) {
    auto& control = record::RecordControl::getInstance();
    auto  data    = control.panelData();

    heading("FlashBridge");
    auto const& converter = status.converter;
    label("playback.record.status.setup"_tr());
    bool const configured = config.liveStream.enabled && !config.liveStream.launchCommand.empty();
    ImGui::TextColored(color(configured ? kGreen : kAmber), "%s", (configured ? "playback.record.status.configured"_tr() : "playback.record.status.notConfigured"_tr()).c_str());
    label("playback.record.status.version"_tr());
    ImGui::Text("%s", converter.version.empty() ? "-" : converter.version.c_str());
    label("playback.record.status.lastResult"_tr());
    if (!converter.lastResult) {
        ImGui::TextColored(color(kInkDim), "-");
    } else {
        auto const& result = *converter.lastResult;
        if (result.ok && result.verified) ImGui::TextColored(color(kGreen), "%s %s", ICON_CHECK, "playback.record.status.verified"_tr().c_str());
        else if (result.ok) ImGui::TextColored(color(kAmber), "%s", "playback.record.status.unverified"_tr().c_str());
        else ImGui::TextColored(color(kRed), "%s", "playback.record.status.failed"_tr(result.error).c_str());
        for (auto const& output : result.outputs) {
            ImGui::PushID(output.c_str());
            label("");
            if (ImGui::SmallButton(ICON_OPEN)) openPath(std::filesystem::path(std::u8string(output.begin(), output.end())), true);
            ImGui::SameLine();
            ImGui::TextColored(color(kInkDim), "%s", output.c_str());
            ImGui::PopID();
        }
    }

    heading("playback.record.status.recovery"_tr());
    if (data.recoverable.empty()) {
        ImGui::TextColored(color(kInkDim), "%s", "playback.record.status.noRecovery"_tr().c_str());
    }
    for (auto const& recording : data.recoverable) {
        ImGui::PushID(recording.dir.string().c_str());
        ImGui::Text(
            "%s",
            "playback.record.status.recoverableItem"_tr(
                recording.worldName.empty() ? "?" : recording.worldName,
                elapsedText(recording.ticks / 20.0),
                recording.chunks
            )
                .c_str()
        );
        ImGui::SameLine();
        if (ImGui::SmallButton("playback.record.status.recover"_tr().c_str())) {
            control.submit({RecordCommand::Recover, std::nullopt, {}, recording.dir});
        }
        ImGui::PopID();
    }

    heading("playback.record.status.trash"_tr());
    if (data.trash.empty()) ImGui::TextColored(color(kInkDim), "%s", "playback.record.status.trashEmpty"_tr().c_str());
    for (auto const& entry : data.trash) {
        ImGui::PushID(entry.name.c_str());
        ImGui::Text("%s", entry.name.c_str());
        ImGui::SameLine();
        if (entry.daysLeft >= 0) {
            ImGui::TextColored(color(kInkDim), "%s", "playback.record.status.daysLeft"_tr(entry.daysLeft).c_str());
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("playback.record.status.restore"_tr().c_str())) {
            control.submit({RecordCommand::RestoreFromTrash, std::nullopt, entry.name});
        }
        ImGui::PopID();
    }

    heading("playback.record.status.diagnostics"_tr());
    if (ImGui::Button("playback.record.status.copyDiagnostics"_tr().c_str())) {
        ImGui::SetClipboardText(diagnostics(status, config).c_str());
        record::RecordingStatus::getInstance().toast("playback.record.toast.copied"_tr(), ToastKind::Info, std::chrono::seconds(3));
    }
    ImGui::SameLine();
    if (ImGui::Button("playback.record.status.openLog"_tr().c_str())) {
        wchar_t profile[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
            openPath(std::filesystem::path(profile) / ".flashbridge" / "logs", false);
        }
    }
}

void pushPanelStyle() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color(kSurface, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, color(kEdge));
    ImGui::PushStyleColor(ImGuiCol_Text, color(kInk));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, color(kGround));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, color(kRaised));
    ImGui::PushStyleColor(ImGuiCol_Button, color(kRaised));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(kEdge));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, color(kBlue));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, color(kBlue));
    ImGui::PushStyleColor(ImGuiCol_Separator, color(kEdge));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16.0f, 14.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f, 8.0f});
}

void popPanelStyle() {
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(10);
}

} // namespace

bool hudWanted() {
    if (Playback::getInstance().isReplayMode()) return false;
    auto const config = settings::SettingsStore::getInstance().get();
    if (!config.hud.enabled) return false;
    auto const status = record::RecordingStatus::getInstance().snapshot();
    return !status.toasts.empty() || (status.phase != RecordingPhase::Idle && !config.hud.hideIndicator);
}

bool panelWanted() { return record::RecordControl::getInstance().panelVisible(); }

void drawHud() {
    auto const config = settings::SettingsStore::getInstance().get();
    auto const status = record::RecordingStatus::getInstance().snapshot();
    auto const& io    = ImGui::GetIO();
    float const margin = 12.0f;
    bool const  right  = config.hud.corner == settings::HudCorner::TopRight || config.hud.corner == settings::HudCorner::BottomRight;
    bool const  bottom = config.hud.corner == settings::HudCorner::BottomLeft || config.hud.corner == settings::HudCorner::BottomRight;
    ImVec2 const pivot{right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f};
    ImVec2 const position{right ? io.DisplaySize.x - margin : margin, bottom ? io.DisplaySize.y - margin : margin};

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
                                     | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings
                                     | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(position, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 6});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    if (ImGui::Begin("##playbackRecordingHud", nullptr, flags)) {
        ImGui::SetWindowFontScale(config.hud.scale);
        float const opacity = config.hud.opacity;
        auto        chip    = [&](auto&& body) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, color(kGround, 0.82f * opacity));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 6});
            ImGui::BeginChild(ImGui::GetID(&body), {0, 0}, ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoInputs);
            body();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        };

        if (status.phase != RecordingPhase::Idle && !config.hud.hideIndicator) {
            auto line = [&] {
                auto separator = [&] {
                    ImGui::SameLine(0, 10);
                    ImGui::TextColored(color(kEdge, opacity), "|");
                    ImGui::SameLine(0, 10);
                };
                switch (status.phase) {
                case RecordingPhase::Recording: {
                    // ~1.2 s breathing dot, unless motion is reduced
                    float pulse = 1.0f;
                    if (!config.ui.reduceMotion) pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::cos(static_cast<float>(ImGui::GetTime()) * 5.2f));
                    ImGui::TextColored(color(kRed, opacity * pulse), "%s", ICON_TRACK_ACTIVE);
                    ImGui::SameLine(0, 6);
                    ImGui::TextColored(color(kInk, opacity), "%s", "playback.record.hud.rec"_tr().c_str());
                    break;
                }
                case RecordingPhase::Paused:
                    ImGui::TextColored(color(kAmber, opacity), "%s %s", ICON_PAUSE, "playback.record.state.paused"_tr().c_str());
                    break;
                case RecordingPhase::Saving:
                    ImGui::TextColored(color(kBlue, opacity), "%s", "playback.record.state.saving"_tr().c_str());
                    break;
                case RecordingPhase::Buffering:
                    ImGui::TextColored(color(kBlue, opacity), "%s", ICON_TRACK_ACTIVE);
                    ImGui::SameLine(0, 6);
                    ImGui::TextColored(color(kInk, opacity), "%s", "playback.record.hud.buffer"_tr().c_str());
                    break;
                case RecordingPhase::Idle:
                    break;
                }
                if (config.hud.showElapsed && status.phase != RecordingPhase::Saving) {
                    ImGui::SameLine(0, 8);
                    ImGui::TextColored(color(kInk, opacity), "%s", elapsedText(status.elapsedSeconds).c_str());
                }
                if (config.hud.showFreeSpace && status.freeGB) {
                    separator();
                    bool const low = *status.freeGB < config.storage.warnFreeGB;
                    ImGui::TextColored(color(low ? kAmber : kInkDim, opacity), "%s", "playback.record.hud.free"_tr(std::format("{:.1f}", *status.freeGB)).c_str());
                }
                if (config.hud.showTps && status.tps) {
                    bool const lag = *status.tps < config.timeNormalization.lagThresholdTps;
                    if (lag || *status.tps < 19.5) {
                        separator();
                        ImGui::TextColored(color(lag ? kAmber : kInkDim, opacity), "%s%s", lag ? ICON_WARNING " " : "", std::format("TPS {:.1f}", *status.tps).c_str());
                    }
                }
                if (config.hud.showConverter) {
                    ImU32      tone = kInkDim;
                    auto const text = converterText(status, tone);
                    if (!text.empty()) {
                        separator();
                        ImGui::TextColored(color(tone, opacity), "%s", text.c_str());
                    }
                }
                if (config.hud.showMarkers && status.markers > 0) {
                    separator();
                    ImGui::TextColored(color(kInkDim, opacity), "%s %d", ICON_MARKER, status.markers);
                }
            };
            chip(line);
        }

        for (auto const& toast : status.toasts) {
            auto body = [&] {
                ImGui::TextColored(color(toastColor(toast.kind), opacity), "%s", ICON_INFO);
                ImGui::SameLine(0, 8);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 24);
                ImGui::TextColored(color(kInk, opacity), "%s", toast.text.c_str());
                ImGui::PopTextWrapPos();
            };
            ImGui::PushID(&toast);
            chip(body);
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void drawPanel() {
    auto const config = settings::SettingsStore::getInstance().get();
    auto const status = record::RecordingStatus::getInstance().snapshot();
    if (!gChoice.initialized || (status.phase != RecordingPhase::Idle && status.phase != RecordingPhase::Buffering)) {
        gChoice = {true, config.output.mode, config.output.convert};
    }

    auto const& io = ImGui::GetIO();
    // Dim the game a little so the panel reads as the active surface.
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, io.DisplaySize, IM_COL32(0, 0, 0, 0x55));

    float const width = std::min(io.DisplaySize.x - 32.0f, ImGui::GetFontSize() * 34.0f);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({width, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, io.DisplaySize.y - 32.0f});
    pushPanelStyle();
    bool open = true;
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize;
    auto const title = std::format("{}  Playback###playbackRecordPanel", ICON_TRACK_ACTIVE);
    if (ImGui::Begin(title.c_str(), &open, flags)) {
        auto tab = [](int index, std::string const& text) {
            bool const selected = gTab == index;
            ImGui::PushStyleColor(ImGuiCol_Button, color(selected ? kRaised : kSurface));
            ImGui::PushStyleColor(ImGuiCol_Text, color(selected ? kInk : kInkDim));
            if (ImGui::Button(text.c_str())) gTab = index;
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
        };
        tab(0, std::format("{} {}", ICON_TRACK_ACTIVE, "playback.record.panel.tabRecord"_tr()));
        tab(1, std::format("{} {}", ICON_SETTINGS, "playback.record.panel.tabSettings"_tr()));
        tab(2, std::format("{} {}", ICON_INFO, "playback.record.panel.tabStatus"_tr()));
        ImGui::NewLine();
        ImGui::Separator();

        ImGui::BeginChild("##panelBody", {0, std::min(io.DisplaySize.y * 0.7f, ImGui::GetFontSize() * 34.0f)}, ImGuiChildFlags_AutoResizeY);
        switch (gTab) {
        case 1:
            drawSettingsTab(config);
            break;
        case 2:
            drawStatusTab(status, config);
            break;
        default:
            drawRecordTab(status, config);
            break;
        }
        ImGui::EndChild();
        ImGui::TextColored(color(kInkDim), "%s", "playback.record.panel.closeHint"_tr(config.hotkeys.panel).c_str());
    }
    ImGui::End();
    popPanelStyle();
    if (!open) record::RecordControl::getInstance().submit({RecordCommand::ClosePanel});
}

} // namespace playback::editor::ui::recording
