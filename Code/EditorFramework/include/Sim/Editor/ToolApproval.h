#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace sim::editor {

/// Pertanyaan "boleh?" yang menyeberang dari thread jaringan ke orang di depan
/// editor, dan jawabannya kembali.
///
/// **Menyeberangi thread dengan menahan yang bertanya, bukan dengan
/// mengantrikan pekerjaannya.** Tool call MCP adalah satu permintaan HTTP yang
/// harus dijawab; ia tidak bisa dijawab "nanti". Jadi thread jaringan menunggu
/// di sini sementara main thread terus menggambar frame dan memunculkan
/// dialognya. Itu aman justru karena urutannya: persetujuan diminta **sebelum**
/// handler diantrikan ke `MainThreadQueue`, jadi tidak ada frame yang tertahan
/// menunggu jawaban atas dirinya sendiri.
///
/// **Satu pertanyaan pada satu waktu.** Dua permintaan yang datang bersamaan
/// akan memunculkan dua dialog yang saling menimpa, dan yang diklik orang
/// menjadi tidak jelas milik yang mana. Yang kedua menunggu gilirannya.
///
/// **Tenggatnya panjang, dan habisnya berarti menolak.** Orang butuh waktu untuk
/// membaca; lima detik yang dipakai `mainThreadTimeout` adalah tenggat untuk
/// menunggu mesin, bukan untuk menunggu orang. Tapi menunggu selamanya berarti
/// satu koneksi tertahan sampai seseorang kembali ke mejanya — jadi ada
/// batasnya, dan yang lewat batas ditolak. Menyetujui sesuatu karena tidak ada
/// yang menjawab adalah kebalikan dari yang diminta mode `ask`.
class ToolApprovalGate {
public:
    /// Apa yang sedang ditanyakan.
    struct Question {
        std::string tool;
        std::string permission;
        std::string arguments;
    };

    /// Dipanggil dari thread jaringan. Menahan sampai dijawab atau tenggat habis.
    bool Ask(Question question, std::chrono::milliseconds timeout);

    /// Dipanggil dari main thread tiap frame. `false` berarti tidak ada yang
    /// menunggu.
    bool Pending(Question& out) const;

    /// Dipanggil dari main thread saat orang menjawab. Diabaikan bila tidak ada
    /// yang menunggu.
    void Answer(bool approved);

    /// Menolak apa pun yang sedang menunggu dan tidak menerima pertanyaan baru.
    ///
    /// Dipanggil saat editor ditutup: thread jaringan yang menunggu jawaban dari
    /// main thread yang sudah berhenti menggambar akan menunggu sampai tenggat
    /// penuh, dan penutupan editor tidak seharusnya memakan waktu selama itu.
    void Shutdown();

private:
    mutable std::mutex mutex_;
    /// Menandai giliran selesai, untuk penanya berikutnya yang sedang antre.
    std::condition_variable free_;
    /// Menandai jawaban sudah masuk, untuk penanya yang sedang menunggu.
    std::condition_variable answered_;
    /// Menjaga "satu pertanyaan pada satu waktu". Penanya berikutnya antre di
    /// `free_`, bukan menahan `mutex_` — kalau ditahan, main thread tidak bisa
    /// membaca pertanyaan yang sedang berlaku.
    bool busy_ = false;
    bool hasAnswer_ = false;
    bool approved_ = false;
    bool stopped_ = false;
    Question question_;
};

}  // namespace sim::editor
