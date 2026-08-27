#pragma once

#include <mutex>

namespace sim {

/// Kunci tunggal untuk seluruh pemakaian FBX SDK di dalam proses ini.
///
/// **FBX SDK tidak thread-safe, dan satu `FbxManager` per thread tidak cukup.**
/// Itu tebakan yang wajar dan salah — dan ia salah dengan cara yang paling mahal:
/// bukan galat, melainkan korupsi heap. `FbxObject::Construct` menelusuri dan
/// menyunting `FbxPropertyPage`, tempat SDK menyimpan pendaftaran kelas dan
/// properti bawaannya, dan halaman itu milik proses — bukan milik manajernya.
/// Dua thread yang masing-masing membuat manajernya sendiri tetap mengaduk
/// struktur yang sama.
///
/// Ia ditemukan sebagai `--bench` yang crash: main thread memuat `shaderBall.fbx`
/// lewat `VulkanRenderer::AcquireMesh` sementara sebuah worker `TaskPool` memuat
/// `unitCylinder.obj` untuk `MeshSdfBakery`. Keduanya berada di dalam
/// `FbxPropertyPage` pada saat yang sama; yang satu SIGSEGV, dan pada jalan lain
/// yang muncul "double free or corruption". Gejalanya berpindah-pindah karena
/// yang rusak heap, bukan sebuah pointer tertentu.
///
/// **`.obj` ikut lewat sini.** Importir memakai FBX SDK untuk seluruh format
/// yang bukan glTF maupun USD, jadi yang harus diserialkan bukan "dua berkas
/// FBX" melainkan setiap dua pemuatan mesh.
///
/// **Di `Sim::Core`, dan itu bukan pilihan yang disukai melainkan yang tersisa.**
/// Dua modul menautkan SDK-nya secara privat — `Sim::Assets` untuk mesh dan
/// `Sim::Animation` untuk klip — dan keduanya tidak saling bergantung. Core
/// satu-satunya tempat yang dilihat keduanya. Ia tidak menyebut satu pun tipe
/// FBX; yang dimilikinya sebuah mutex, dan namanya yang mengatakan untuk apa.
///
/// **Yang dikunci seluruh umur `FbxSceneHandle`, bukan hanya `Open()`.**
/// Menelusuri panggung membaca halaman properti yang sama yang bisa disunting
/// `Construct` milik thread lain, jadi melepasnya sesudah berkas terbaca hanya
/// memindahkan tabrakannya ke tempat yang lebih sulit dilihat.
///
/// Harganya impor mesh menjadi berurutan. Itu nyata dan diterima: bake SDF —
/// bagian yang mahal — tetap paralel, karena ia berjalan sesudah handle-nya
/// dilepas. Menukar throughput impor dengan heap yang tidak rusak adalah
/// pertukaran yang tidak perlu diperdebatkan.
std::mutex& FbxSdkMutex();

}  // namespace sim
