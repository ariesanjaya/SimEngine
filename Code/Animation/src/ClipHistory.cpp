#include "Sim/Animation/ClipHistory.h"

#include <algorithm>

namespace sim::animation {
namespace {

std::size_t CurveBytes(const Curve& curve) {
    return curve.Keys().capacity() * sizeof(CurveKey);
}

}  // namespace

void ClipHistory::Begin(std::string label) {
    if (inStep_) {
        return;
    }
    inStep_ = true;
    current_ = Step{};
    current_.label = std::move(label);
}

void ClipHistory::Capture(const Clip& clip, int track) {
    if (!inStep_ || track < 0 || track >= clip.TrackCount()) {
        return;
    }
    for (const TrackImage& image : current_.tracks) {
        if (image.index == track) {
            // Salinan pertama yang berlaku. Salinan kedua di dalam langkah yang
            // sama akan menyimpan keadaan di tengah suntingan, dan undo-nya
            // berhenti separuh jalan.
            return;
        }
    }
    current_.tracks.push_back(TrackImage{track, clip.TrackAt(track)});
}

void ClipHistory::CaptureEvents(const Clip& clip) {
    if (!inStep_ || current_.hasEvents) {
        return;
    }
    current_.events = clip.Events();
    current_.hasEvents = true;
}

void ClipHistory::CaptureSyncMarkers(const Clip& clip) {
    if (!inStep_ || current_.hasSync) {
        return;
    }
    current_.sync = clip.SyncMarkers();
    current_.hasSync = true;
}

void ClipHistory::CaptureAllTracks(const Clip& clip) {
    if (!inStep_ || current_.hasAllTracks) {
        return;
    }
    current_.allTracks = clip.Tracks();
    current_.hasAllTracks = true;
    // Salinan per-track menjadi mubazir begitu seluruh daftarnya tersalin, dan
    // menyisakannya berarti dua sumber kebenaran yang harus dipulihkan berurutan
    // dengan benar.
    current_.tracks.clear();
}

void ClipHistory::End() {
    if (!inStep_) {
        return;
    }
    inStep_ = false;
    if (current_.Empty()) {
        return;
    }
    current_.bytes = SizeOf(current_);
    bytes_ += current_.bytes;
    undo_.push_back(std::move(current_));
    current_ = Step{};
    // Cabang redo dibuang begitu ada suntingan baru: riwayat yang bisa dimasuki
    // kembali setelah menyunting tidak bisa ditebak siapa pun.
    redo_.clear();
    Trim();
}

std::size_t ClipHistory::SizeOf(const Step& step) {
    std::size_t bytes = step.label.capacity();
    for (const TrackImage& image : step.tracks) {
        bytes += CurveBytes(image.track.curve) + image.track.bone.capacity() + sizeof(TrackImage);
    }
    for (const Track& track : step.allTracks) {
        bytes += CurveBytes(track.curve) + track.bone.capacity() + sizeof(Track);
    }
    bytes += step.events.capacity() * sizeof(Event);
    bytes += step.sync.capacity() * sizeof(SyncMarker);
    return bytes;
}

void ClipHistory::Apply(Step& step, Clip& clip) {
    // Ditukar, bukan disalin: satu salinan melayani undo dan redo sekaligus —
    // pola yang sama dengan jurnal blok terrain.
    if (step.hasAllTracks) {
        std::vector<Track> live = clip.Tracks();
        clip.SetTracks(step.allTracks);
        step.allTracks = std::move(live);
    }
    for (TrackImage& image : step.tracks) {
        if (image.index < 0 || image.index >= clip.TrackCount()) {
            continue;
        }
        Track live = clip.TrackAt(image.index);
        clip.TrackAt(image.index) = image.track;
        image.track = std::move(live);
    }
    if (step.hasEvents) {
        std::vector<Event> live = clip.Events();
        clip.SetEvents(step.events);
        step.events = std::move(live);
    }
    if (step.hasSync) {
        std::vector<SyncMarker> live = clip.SyncMarkers();
        clip.SetSyncMarkers(step.sync);
        step.sync = std::move(live);
    }
}

bool ClipHistory::Undo(Clip& clip) {
    if (inStep_ || undo_.empty()) {
        return false;
    }
    Step step = std::move(undo_.back());
    undo_.pop_back();
    bytes_ -= std::min(bytes_, step.bytes);
    Apply(step, clip);
    redo_.push_back(std::move(step));
    return true;
}

bool ClipHistory::Redo(Clip& clip) {
    if (inStep_ || redo_.empty()) {
        return false;
    }
    Step step = std::move(redo_.back());
    redo_.pop_back();
    Apply(step, clip);
    bytes_ += step.bytes;
    undo_.push_back(std::move(step));
    return true;
}

void ClipHistory::Clear() {
    inStep_ = false;
    current_ = Step{};
    undo_.clear();
    redo_.clear();
    bytes_ = 0;
}

std::string_view ClipHistory::UndoLabel() const {
    return undo_.empty() ? std::string_view{} : std::string_view(undo_.back().label);
}

std::string_view ClipHistory::RedoLabel() const {
    return redo_.empty() ? std::string_view{} : std::string_view(redo_.back().label);
}

void ClipHistory::Trim() {
    while (bytes_ > budgetBytes && undo_.size() > 1) {
        bytes_ -= std::min(bytes_, undo_.front().bytes);
        undo_.erase(undo_.begin());
    }
}

}  // namespace sim::animation
