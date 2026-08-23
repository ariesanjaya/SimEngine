#!/usr/bin/env python3
"""Mengganti GUID entity yang bukan UUID sungguhan di sebuah berkas level.

**Gejalanya terlihat di Inspector, bukan di berkasnya.** Entity yang GUID-nya
ditulis tangan sebagai pencacah — `00000000-0000-0000-0000-00000000000a` —
tampil di baris "Entity ID" sebagai deretan nol yang sama untuk setiap entity di
adegan itu. Nomornya memang ada di dua digit terakhir, tapi tidak ada yang bisa
dibaca sekilas dari tiga puluh empat karakter nol yang mendahuluinya; yang
terlihat orang adalah "ID-nya kosong".

**Yang diganti hanya yang memang bukan UUID.** Nibble versi dan varian sebuah
UUID v4 selalu `4` dan `8..b`; GUID pencacah punya nol di keduanya, dan itu yang
dipakai di sini sebagai syaratnya. GUID sungguhan — termasuk yang berbentuk
awalan-plus-pencacah seperti `bec00000-0000-4000-8000-...` di `bench.simlevel` —
lolos apa adanya, jadi menjalankan skrip ini dua kali tidak mengubah apa pun
pada jalan kedua.

**GUID barunya diturunkan, bukan diacak**, dengan alasan yang sama seperti di
`cornell-whitebox.py`: menjalankan skrip ini dua kali di dua mesin harus
menghasilkan berkas yang sama, kalau tidak setiap orang yang menjalankannya
menghasilkan diff. Turunannya dari nama level dan nama entity, jadi dua level
tidak pernah berbagi GUID walaupun nama entity-nya sama.

**Referensi ikut diperbarui, dan hanya referensi antar-entity.** Di skema level
`"parent"` satu-satunya medan yang menunjuk entity lain; `"mesh"` dan
`"material"` menunjuk aset, dan memetakannya di sini akan memutus setiap
material yang benar-benar dipakai. Karena itu daftarnya disebut satu per satu di
`ENTITY_REFERENCES` alih-alih "ganti setiap GUID yang cocok di seluruh berkas".

Pemakaian:
    python3 Tools/relabel-level-guids.py Resources/Levels/cornell.simlevel
    python3 Tools/relabel-level-guids.py --dry-run ~/Documents/SimEngine/FarmSim/Levels/*.simlevel
"""

import argparse
import json
import pathlib
import re
import uuid

# Ruang nama akar. Ruang nama tiap level diturunkan darinya bersama nama
# berkasnya, sehingga dua level dengan entity bernama "Floor" tetap mendapat
# GUID yang berbeda.
ROOT_NAMESPACE = uuid.UUID("5e1abe10-0000-4000-8000-000000000000")

NULL_GUID = "00000000-0000-0000-0000-000000000000"

# Bentuk baku 8-4-4-4-12, huruf kecil. Yang tidak cocok bukan urusan skrip ini:
# berkasnya rusak, dan menambalnya diam-diam menyembunyikan kerusakan itu.
GUID_PATTERN = re.compile(r"^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$")

# Medan yang isinya GUID **entity**. Bukan daftar semua medan ber-GUID — lihat
# catatan di atas.
ENTITY_REFERENCES = ("parent",)


def is_placeholder(guid: str) -> bool:
    """True bila `guid` sah bentuknya tapi bukan UUID sungguhan."""
    if not GUID_PATTERN.match(guid) or guid == NULL_GUID:
        return False
    # Nibble versi (digit pertama grup ketiga) dan varian (digit pertama grup
    # keempat). Keduanya nol hanya pada GUID yang dikarang sebagai angka.
    version, variant = guid[14], guid[19]
    return version == "0" and variant == "0"


def derive(namespace: uuid.UUID, name: str, fallback: str) -> str:
    # Nama entity yang kosong tidak bisa dipakai sebagai kunci — dua entity tanpa
    # nama akan mendapat GUID yang sama, dan yang kedua menimpa yang pertama di
    # peta `byGuid_` milik World. GUID lamanya sudah unik di dalam berkas ini,
    # jadi ia yang menjadi kunci cadangan.
    return str(uuid.uuid5(namespace, name or fallback))


def relabel(path: pathlib.Path, dry_run: bool, namespace_key: str | None = None) -> int:
    document = json.loads(path.read_text())
    entities = document.get("entities", [])
    namespace = uuid.uuid5(ROOT_NAMESPACE, namespace_key or path.stem)

    mapping: dict[str, str] = {}
    for entity in entities:
        old = entity.get("guid", "")
        if not is_placeholder(old):
            continue
        name = entity.get("components", {}).get("Name", {}).get("name", "")
        mapping[old] = derive(namespace, name, old)

    if not mapping:
        print(f"{path}: tidak ada GUID entity yang perlu diganti")
        return 0

    # Dua entity yang menghasilkan GUID yang sama berarti kuncinya bertabrakan —
    # berhenti, jangan tulis berkas yang kehilangan satu entity saat dimuat.
    if len(set(mapping.values())) != len(mapping):
        raise SystemExit(f"{path}: GUID turunan bertabrakan; periksa nama entity yang kembar")

    for entity in entities:
        if entity.get("guid") in mapping:
            entity["guid"] = mapping[entity["guid"]]
        for field in ENTITY_REFERENCES:
            if entity.get(field) in mapping:
                entity[field] = mapping[entity[field]]

    name_of = {
        entity["guid"]: entity.get("components", {}).get("Name", {}).get("name", "?")
        for entity in entities
    }
    for old, new in mapping.items():
        print(f"  {old} -> {new}  ({name_of.get(new, '?')})")

    if dry_run:
        print(f"{path}: {len(mapping)} GUID (dry-run, berkas tidak ditulis)")
        return 0

    # Tanpa newline di ujung, sama dengan berkas yang sudah ada: yang berubah
    # harus hanya baris GUID-nya, supaya diff-nya bisa dibaca.
    path.write_text(json.dumps(document, indent=2))
    print(f"{path}: {len(mapping)} GUID diganti")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("levels", nargs="+", help="berkas .simlevel")
    parser.add_argument("--dry-run", action="store_true", help="tampilkan saja, jangan tulis")
    # **Dipakai berkas yang menyalin isi level lain**, dan yang paling sering
    # begitu adalah `autosave.simlevel`: ia potret level yang sedang dibuka.
    # Tanpa ini ia mendapat ruang nama sendiri dari nama berkasnya, jadi sembilan
    # entity yang sama persis berakhir dengan dua himpunan identitas — dan
    # memulihkan dari autosave menghasilkan entity yang bukan lagi entity yang
    # sama dengan yang ada di levelnya.
    parser.add_argument(
        "--namespace",
        default=None,
        help="turunkan GUID dari nama level ini, bukan dari nama berkasnya",
    )
    args = parser.parse_args()

    for name in args.levels:
        relabel(pathlib.Path(name).expanduser(), args.dry_run, args.namespace)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
