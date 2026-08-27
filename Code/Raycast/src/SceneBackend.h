#pragma once

// Memilih backend yang ikut dibangun — R6 di docs/PLAN-EMBREE.md.
//
// **Dipilih saat kompilasi, bukan lewat tabel virtual.** `Raycast` dipanggil
// jutaan kali per gambar acuan; sebuah panggilan tak-langsung di sana adalah
// ongkos yang dibayar terus-menerus demi fleksibilitas yang tidak dipakai
// siapa pun saat berjalan. Yang memilih backend adalah orang yang
// mengonfigurasi build, sekali.
//
// **Kedua berkas mendefinisikan kelas dengan nama yang sama, dan itu
// disengaja.** Hanya satu yang pernah ikut dibangun, jadi tidak ada dua
// definisi yang bertemu — dan `RayScene` di header publik bisa tetap
// mendeklarasikan `class SceneBackend;` tanpa tahu mana yang dipakai. Sebuah
// alias `using` tidak bisa dideklarasikan maju, dan mengubah header publik
// supaya bisa berarti membocorkan pilihan backend ke seluruh mesin.

#if SIM_WITH_EMBREE
#include "EmbreeBackend.h"
#else
#include "BvhBackend.h"
#endif
