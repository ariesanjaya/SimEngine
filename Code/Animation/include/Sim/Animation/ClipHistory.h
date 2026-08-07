#pragma once

#include "Sim/Animation/Clip.h"

#include <string>
#include <vector>

namespace sim::animation {

/// Riwayat undo untuk suntingan sebuah klip.
///
/// **Menyalin track, bukan menyalin klip.** Sebuah klip 60 detik pada rig 100
/// bone berukuran puluhan megabyte; menyimpan snapshot seluruh klip tiap kali
/// sebuah kunci digeser akan menghabiskan RAM dalam beberapa menit menyunting.
/// Satu track pada klip yang sama berukuran puluhan kilobyte, dan satu suntingan
/// hampir selalu menyentuh satu atau dua track.
///
/// **Menyalin track utuh, bukan menyalin kunci yang berubah saja.** Ini pilihan
/// yang berlawanan arah dengan yang di atas, dan sengaja: memindahkan sebuah
/// kunci bisa mengubah *urutannya* di dalam kurva — `Curve::MoveKey` menjaga
/// urutan waktu, jadi kunci yang diseret melewati tetangganya berpindah indeks.
/// Delta per-kunci karena itu harus ikut mencatat perpindahan indeks, dan
/// riwayat yang mencatat indeks adalah riwayat yang salah begitu ada kunci lain
/// disisipkan. Track utuh tidak punya masalah itu sama sekali.
///
/// **Tidak lewat `CommandHistory`**, sama seperti Terrain dan Vegetation:
/// riwayat utama menjanjikan pembatalan perubahan *scene*, sedangkan klip adalah
/// dokumen yang dibuka dan ditutup.
class ClipHistory {
public:
    /// Batas byte jurnal. Langkah terlama dibuang saat terlampaui.
    std::size_t budgetBytes = 64u * 1024u * 1024u;

    /// Memulai satu langkah undo. `label` muncul di panel.
    void Begin(std::string label);
    /// Menyalin keadaan sebuah track sebelum ia diubah. Aman dipanggil berkali-
    /// kali untuk track yang sama — hanya salinan pertama yang disimpan.
    void Capture(const Clip& clip, int track);
    /// Menyalin daftar event dan penanda fase sebelum diubah.
    void CaptureEvents(const Clip& clip);
    void CaptureSyncMarkers(const Clip& clip);
    /// Menyalin seluruh daftar track — untuk operasi yang menambah atau
    /// menghapus track, ketika indeksnya sendiri bergeser.
    void CaptureAllTracks(const Clip& clip);
    /// Menutup langkah. Langkah yang tidak menyalin apa pun dibuang.
    void End();
    bool InStep() const { return inStep_; }

    bool Undo(Clip& clip);
    bool Redo(Clip& clip);
    void Clear();

    std::size_t UndoDepth() const { return undo_.size(); }
    std::size_t RedoDepth() const { return redo_.size(); }
    /// Nama langkah yang akan dibatalkan berikutnya, atau kosong.
    std::string_view UndoLabel() const;
    std::string_view RedoLabel() const;
    std::size_t Bytes() const { return bytes_; }

private:
    struct TrackImage {
        int index = 0;
        Track track;
    };

    struct Step {
        std::string label;
        std::vector<TrackImage> tracks;
        /// Seluruh daftar track, dipakai saat track ditambah atau dihapus.
        std::vector<Track> allTracks;
        bool hasAllTracks = false;
        std::vector<Event> events;
        bool hasEvents = false;
        std::vector<SyncMarker> sync;
        bool hasSync = false;
        std::size_t bytes = 0;

        bool Empty() const {
            return tracks.empty() && !hasAllTracks && !hasEvents && !hasSync;
        }
    };

    static std::size_t SizeOf(const Step& step);
    void Apply(Step& step, Clip& clip);
    void Trim();

    bool inStep_ = false;
    Step current_;
    std::vector<Step> undo_;
    std::vector<Step> redo_;
    std::size_t bytes_ = 0;
};

}  // namespace sim::animation
