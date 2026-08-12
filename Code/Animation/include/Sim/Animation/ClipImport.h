#pragma once

#include "Sim/Animation/Clip.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sim::animation {

/// Memuat seluruh klip animasi dari sebuah berkas FBX.
///
/// **Impor FBX ada di modul ini, bukan di `Sim::Assets`.** Preseden yang
/// diikuti adalah `Sim::Terrain`, yang mengurus sendiri baca-tulis heightmap
/// PNG-nya: modul yang memiliki bentuk datanya juga yang memiliki penerjemahan
/// dari berkasnya. Alternatifnya — bentuk klip yang netral di sisi aset lalu
/// diterjemahkan di sini — berarti seluruh model track, kunci, dan kurva
/// dituliskan dua kali, untuk satu-satunya pembaca yang akan pernah ada.
///
/// **Kunci dipanggang, bukan dibaca sebagai kurva mentah FBX.** FBX menyimpan
/// rotasi sebagai kurva Euler beserta urutan rotasinya, pre-rotation,
/// post-rotation, dan pivot yang semuanya ikut menentukan hasilnya. Membaca
/// kurvanya langsung berarti menyusun ulang seluruh rantai itu tangan — dan
/// setiap potong yang terlewat menghasilkan animasi yang *hampir* benar, yang
/// jauh lebih sulit dilacak daripada yang jelas-jelas salah. `ufbx_bake_anim`
/// mengevaluasi rantainya dan mengembalikan transform lokal per node.
///
/// Rotasi karena itu masuk sebagai `RotationTrack` berkunci kuaternion,
/// sementara translasi dan skala masuk sebagai kanal skalar biasa — keduanya
/// memang bentuk yang dihasilkan pemanggangan, dan keduanya bisa disunting
/// penyunting kurva yang sudah ada.
///
/// Mengembalikan daftar kosong pada kegagalan, dengan sebabnya di `error`.
/// **Sebuah daftar, bukan satu klip:** sebuah berkas FBX bisa memuat lebih dari
/// satu take, dan mengambil yang pertama saja adalah cara yang paling mudah
/// untuk diam-diam mengimpor klip yang salah — pada berkas Mixamo, take pertama
/// justru yang kosong.
std::vector<Clip> ImportClipsFromFbx(const std::filesystem::path& path, std::string& error);

}  // namespace sim::animation
