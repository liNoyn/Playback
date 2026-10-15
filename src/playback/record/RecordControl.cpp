#include "RecordControl.h"

#include "playback/Playback.h"
#include "playback/editor/input/EditorInput.h"
#include "playback/record/RecordingStatus.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/i18n/I18n.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/input/Keyboard.h"

#include <Windows.h>

#include <array>
#include <utility>

namespace playback::record {

namespace {

constexpr auto DiskCheckInterval = std::chrono::seconds(5);
constexpr int  DefaultMarkerColor = 0x46C37B;

struct Bindings {
    settings::Hotkey record;
    settings::Hotkey recordWithMode;
    settings::Hotkey pause;
    settings::Hotkey marker;
    settings::Hotkey panel;
};

std::mutex             gBindingsMutex;
Bindings               gBindings;
ll::event::ListenerPtr gKeyListener;
std::array<bool, 256>  gKeysDown{}; // input thread only: ignores key repeat

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

bool matches(settings::Hotkey const& hotkey, int key, bool ctrl, bool shift, bool alt) {
    return hotkey.valid() && hotkey.key == key && hotkey.ctrl == ctrl && hotkey.shift == shift && hotkey.alt == alt;
}

void handleKey(ll::event::KeyInputEvent& event) {
    int const key = event.keyCode();
    if (key < 0 || key >= static_cast<int>(gKeysDown.size())) return;
    if (!event.isDown()) {
        gKeysDown[static_cast<size_t>(key)] = false;
        return;
    }
    if (std::exchange(gKeysDown[static_cast<size_t>(key)], true)) return; // held down: no repeats

    auto&      control = RecordControl::getInstance();
    bool const panel   = control.panelVisible();
    if (!panel && !control.inRecordableWorld()) return;

    bool const ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool const shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool const alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;

    Bindings bindings;
    {
        std::scoped_lock lock(gBindingsMutex);
        bindings = gBindings;
    }

    std::optional<RecordCommand> command;
    if (panel && key == Keyboard::Escape) command = RecordCommand::ClosePanel;
    else if (matches(bindings.panel, key, ctrl, shift, alt)) command = RecordCommand::TogglePanel;
    else if (matches(bindings.recordWithMode, key, ctrl, shift, alt)) command = RecordCommand::TogglePanel;
    else if (matches(bindings.record, key, ctrl, shift, alt)) command = RecordCommand::ToggleRecording;
    else if (matches(bindings.pause, key, ctrl, shift, alt)) command = RecordCommand::TogglePause;
    else if (matches(bindings.marker, key, ctrl, shift, alt)) command = RecordCommand::Marker;
    if (!command) return;

    control.submit({*command});
    event.cancel();
}

std::optional<double> freeGigabytes() {
    std::error_code ec;
    auto const      dir = utils::PathUtils::getReplaysDir();
    std::filesystem::create_directories(dir, ec);
    auto const space = std::filesystem::space(dir, ec);
    if (ec) return std::nullopt;
    return static_cast<double>(space.available) / (1024.0 * 1024.0 * 1024.0);
}

} // namespace

RecordControl& RecordControl::getInstance() {
    static RecordControl instance;
    return instance;
}

void RecordControl::submit(RecordRequest request) {
    std::scoped_lock lock(mQueueMutex);
    mQueue.push_back(std::move(request));
}

RecordPanelData RecordControl::panelData() const {
    std::scoped_lock lock(mPanelDataMutex);
    return mPanelData;
}

void RecordControl::applySettings(settings::Settings const& settings) {
    auto parse = [](std::string const& text, char const* name) {
        auto hotkey = settings::Hotkey::parse(text);
        if (!hotkey) {
            getLogger().warn("settings.json: hotkey \"{}\" for {} is not a key; it is disabled", text, name);
            return settings::Hotkey{};
        }
        return *hotkey;
    };
    Bindings bindings{
        parse(settings.hotkeys.record, "record"),
        parse(settings.hotkeys.recordWithMode, "recordWithMode"),
        parse(settings.hotkeys.pause, "pause"),
        parse(settings.hotkeys.marker, "marker"),
        parse(settings.hotkeys.panel, "panel"),
    };
    std::scoped_lock lock(gBindingsMutex);
    gBindings = bindings;
}

void RecordControl::tick(ClientInstance& client) {
    using namespace ll::i18n_literals;

    auto& playback = Playback::getInstance();
    bool const inWorld = client.getLocalPlayer() != nullptr && playback.getMode() == PlaybackMode::Record;
    // Hotkeys stay quiet in chat, menus and sign editing; the panel takes over the keys while it is open.
    mInWorld.store(inWorld && client.isInWorldAndNotShowingAnyMenuScreens(), std::memory_order_release);
    if (!inWorld && mPanelVisible.load(std::memory_order_acquire)) setPanelVisible(false);

    std::vector<RecordRequest> requests;
    {
        std::scoped_lock lock(mQueueMutex);
        requests.swap(mQueue);
    }
    for (auto const& request : requests) execute(request, client);

    if (inWorld && !mRecoveryAnnounced) {
        mRecoveryAnnounced = true;
        if (auto const count = RecordingFiles::findRecoverable().size(); count > 0 && !Recorder::getInstance().isActive()) {
            RecordingStatus::getInstance().toast("playback.record.toast.recoverable"_tr(count), ToastKind::Warning, std::chrono::seconds(10));
        }
    }

    // Closing the panel leaves the cursor free; take it back once the UI let go of the input.
    if (mRegrabTicks > 0 && --mRegrabTicks == 0) {
        if (!editor::input::isUiVisible() && client.isInWorldAndNotShowingAnyMenuScreens() && !client.getMouseGrabbed()) {
            client.grabMouse();
        }
    }

    guardDisk(Recorder::getInstance().isActive());
}

void RecordControl::execute(RecordRequest const& request, ClientInstance& client) {
    using namespace ll::i18n_literals;
    auto& recorder = Recorder::getInstance();
    auto& status   = RecordingStatus::getInstance();
    bool const inWorld = client.getLocalPlayer() != nullptr && Playback::getInstance().getMode() == PlaybackMode::Record;

    switch (request.command) {
    case RecordCommand::ToggleRecording:
        if (recorder.isActive()) {
            recorder.stop();
        } else if (inWorld) {
            recorder.start();
        }
        break;
    case RecordCommand::Start:
        if (!recorder.isActive() && inWorld) {
            if (request.options) recorder.start(*request.options);
            else recorder.start();
        } else if (recorder.isPaused()) {
            recorder.start(); // resume
        }
        break;
    case RecordCommand::Stop:
        if (recorder.isActive()) recorder.stop();
        break;
    case RecordCommand::TogglePause:
        if (recorder.isPaused()) recorder.start();
        else if (recorder.isActive()) recorder.pause();
        break;
    case RecordCommand::Marker: {
        if (!recorder.isActive()) {
            status.toast("playback.record.toast.markerNotRecording"_tr(), ToastKind::Warning);
            break;
        }
        auto name = request.text.empty() ? "playback.record.markerName"_tr(recorder.markerCount() + 1) : request.text;
        if (recorder.addMarker(name, request.color != 0 ? request.color : DefaultMarkerColor)) {
            status.toast("playback.record.toast.marker"_tr(name), ToastKind::Info, std::chrono::seconds(3));
        }
        break;
    }
    case RecordCommand::TogglePanel:
        setPanelVisible(!mPanelVisible.load(std::memory_order_acquire) && inWorld);
        break;
    case RecordCommand::ClosePanel:
        setPanelVisible(false);
        break;
    case RecordCommand::Recover:
        if (recorder.isActive()) break;
        if (auto const file = RecordingFiles::recover(request.path)) {
            status.toast("playback.record.toast.recovered"_tr(file->filename().string()), ToastKind::Success, std::chrono::seconds(8));
        } else {
            status.toast("playback.record.toast.recoverFailed"_tr(), ToastKind::Error, std::chrono::seconds(8));
        }
        refreshPanelData();
        break;
    case RecordCommand::RestoreFromTrash:
        if (auto const file = RecordingFiles::restoreFromTrash(request.text)) {
            status.toast("playback.record.toast.restored"_tr(file->filename().string()), ToastKind::Success);
        }
        refreshPanelData();
        break;
    }
}

void RecordControl::setPanelVisible(bool visible) {
    bool const was = mPanelVisible.exchange(visible, std::memory_order_acq_rel);
    if (visible && !was) {
        refreshPanelData();
        mNextDiskCheck = {};
    }
    if (!visible && was) mRegrabTicks = 3;
}

void RecordControl::refreshPanelData() {
    RecordPanelData data;
    if (!Recorder::getInstance().isActive()) data.recoverable = RecordingFiles::findRecoverable();
    data.trash = RecordingFiles::listTrash(settings::SettingsStore::getInstance().get().output.sourceKeep);
    std::scoped_lock lock(mPanelDataMutex);
    mPanelData = std::move(data);
}

// Warns when the replay drive runs low and stops (saving what was recorded) before it runs out.
void RecordControl::guardDisk(bool recording) {
    using namespace ll::i18n_literals;
    auto const now = std::chrono::steady_clock::now();
    if (!recording && !mPanelVisible.load(std::memory_order_acquire)) {
        mDiskWarned = false;
        return;
    }
    if (now < mNextDiskCheck) return;
    mNextDiskCheck = now + DiskCheckInterval;

    auto const free = freeGigabytes();
    RecordingStatus::getInstance().setFreeSpace(free);
    if (!free || !recording) return;

    auto const storage = settings::SettingsStore::getInstance().get().storage;
    if (*free < storage.stopFreeGB) {
        getLogger().warn("Only {:.2f} GB free on the replay drive; stopping the recording", *free);
        RecordingStatus::getInstance().toast("playback.record.toast.diskStopped"_tr(), ToastKind::Error, std::chrono::seconds(12));
        Recorder::getInstance().stop();
    } else if (*free < storage.warnFreeGB && !mDiskWarned) {
        mDiskWarned = true;
        RecordingStatus::getInstance().toast("playback.record.toast.diskLow"_tr(*free), ToastKind::Warning, std::chrono::seconds(10));
    }
}

bool hookHotkeys(bool enable) {
    auto& eventBus = ll::event::EventBus::getInstance();
    if (enable) {
        if (gKeyListener) return true;
        // Ahead of the replay UI's key listener, which swallows keys while its own screens are open.
        gKeyListener = eventBus.emplaceListener<ll::event::KeyInputEvent>(&handleKey, ll::event::EventPriority::High);
        return gKeyListener != nullptr;
    }
    if (gKeyListener) {
        eventBus.removeListener(gKeyListener);
        gKeyListener.reset();
    }
    return true;
}

} // namespace playback::record
