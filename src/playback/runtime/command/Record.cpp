#include "Command.h"

#include "playback/configuration/Config.h"
#include "playback/record/RecordControl.h"
#include "playback/record/Recorder.h"
#include "playback/record/RecordingStatus.h"
#include "playback/settings/Settings.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

#include <format>
#include <string>

namespace playback::runtime::command {

// Command parameters need external linkage (they are reflected by the command registrar).
// /record start <playback|both|flashback>
enum class RecordOutput { playback, both, flashback };

struct RecordStartParams {
    RecordOutput output;
};

struct RecordMarkerParams {
    std::string name;
};

namespace {

settings::OutputMode toMode(RecordOutput output) {
    switch (output) {
    case RecordOutput::playback:
        return settings::OutputMode::Playback;
    case RecordOutput::flashback:
        return settings::OutputMode::Flashback;
    case RecordOutput::both:
    default:
        return settings::OutputMode::Both;
    }
}

} // namespace

void registerRecordCommand(configuration::CommandConfigStruct& config) {
    using namespace ll::i18n_literals;

    if (!config.enabled) {
        return;
    }

    auto& recordCommand = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(
        config.command,
        "playback.command.record.description"_tr()
    );

    recordCommand.overload().text("start").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = record::Recorder::getInstance();
        recorder.start();
        if (recorder.isActive()) output.success("playback.command.record.started"_tr());
    });

    recordCommand.overload<RecordStartParams>().text("start").required("output").execute(
        [](CommandOrigin const&, CommandOutput& output, RecordStartParams const& params) {
            auto& recorder = record::Recorder::getInstance();
            auto  options  = recorder.options();
            options.mode   = toMode(params.output);
            options.convert = settings::SettingsStore::getInstance().get().output.convert;
            recorder.start(options);
            if (recorder.isActive()) output.success("playback.command.record.started"_tr());
        }
    );

    recordCommand.overload().text("pause").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = record::Recorder::getInstance();
        recorder.pause();

        output.success("playback.command.record.paused"_tr());
    });

    recordCommand.overload().text("stop").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = record::Recorder::getInstance();
        recorder.stop();

        output.success("playback.command.record.stopped"_tr());
    });

    recordCommand.overload<RecordMarkerParams>().text("marker").optional("name").execute(
        [](CommandOrigin const&, CommandOutput& output, RecordMarkerParams const& params) {
            auto& recorder = record::Recorder::getInstance();
            if (!recorder.isActive()) {
                output.error("playback.record.toast.markerNotRecording"_tr());
                return;
            }
            auto const name = params.name.empty() ? "playback.record.markerName"_tr(recorder.markerCount() + 1) : params.name;
            recorder.addMarker(name, 0x46C37B);
            output.success("playback.record.toast.marker"_tr(name));
        }
    );

    recordCommand.overload().text("panel").execute([](CommandOrigin const&, CommandOutput&) {
        record::RecordControl::getInstance().submit({record::RecordCommand::TogglePanel});
    });

    recordCommand.overload().text("status").execute([](CommandOrigin const&, CommandOutput& output) {
        auto const& recorder = record::Recorder::getInstance();
        auto const  status   = record::RecordingStatus::getInstance().snapshot();
        if (!recorder.isActive()) {
            output.success("playback.command.record.statusIdle"_tr());
        } else {
            auto const seconds = recorder.recordedTicks() / 20;
            output.success(
                "playback.command.record.statusActive"_tr(
                    recorder.isPaused() ? "playback.record.state.paused"_tr() : "playback.record.state.recording"_tr(),
                    std::format("{:02}:{:02}", seconds / 60, seconds % 60),
                    std::string(settings::toString(recorder.options().mode)),
                    recorder.markerCount(),
                    status.tps ? std::format("{:.1f}", *status.tps) : std::string("-")
                )
            );
        }
        auto const& converter = status.converter;
        if (converter.connected) {
            output.success("playback.command.record.statusConverter"_tr(converter.state, converter.version, converter.ticks));
        }
        if (converter.lastResult) {
            output.success(
                "playback.command.record.statusLastResult"_tr(
                    converter.lastResult->ok && converter.lastResult->verified ? "OK" : "NG",
                    converter.lastResult->outputs.empty() ? std::string("-") : converter.lastResult->outputs.front()
                )
            );
        }
    });
}

} // namespace playback::runtime::command
