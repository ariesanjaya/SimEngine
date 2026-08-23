#!/usr/bin/env python3
"""Menyusun Cornell box dari **whitebox**, bukan dari mesh impor.

**Kembarannya `cornell.simlevel`, dan bedanya justru gunanya.** Yang itu menyusun
ruangannya dari satu mesh kubus bawaan yang diskalakan tujuh kali; yang ini
memberi tiap permukaan geometri whitebox-nya sendiri — bisa diextrude, digeser
sisinya, dan dipecah lebih lanjut di dalam editor. Bentuk, posisi, dan lampunya
sengaja sama persis, supaya keduanya bisa ditumpuk untuk membandingkan jalur
render whitebox dengan jalur mesh.

**Ukurannya dipanggang ke dalam geometrinya, bukan ke skala entity.** Blok
whitebox dibentuk dengan mendorong sisinya, dan entity yang diskalakan 5x
membuat setiap dorongan berikutnya ikut terskalakan — satu meter yang diketik
orang menjadi lima meter di layar, tanpa satu pun angka di panel yang
menyebutkannya. Transform di sini karena itu hanya membawa posisi.

**GUID-nya diturunkan, bukan diacak.** Menjalankan skrip ini dua kali harus
menghasilkan level yang menunjuk aset yang sama; GUID acak akan meninggalkan
level lama menunjuk berkas yang masih ada tapi sudah punya identitas baru, dan
yang terlihat adalah ruangan yang hilang separuh.

Pemakaian:
    python3 Tools/cornell-whitebox.py
    python3 Tools/cornell-whitebox.py --project ~/Documents/SimEngine/FarmSim
"""

import argparse
import json
import pathlib
import uuid

# Ruang nama tersendiri: aset ini milik adegan ini, dan berbagi ruang nama dengan
# adegan lain berarti dua skrip bisa menghasilkan GUID yang sama untuk berkas
# yang berbeda.
NAMESPACE = uuid.UUID("c0f1e11b-0000-4000-8000-000000000000")

META = (
    '{{\n  "version": 1,\n  "guid": "{}",\n'
    '  "note": "Keep this file next to its asset and commit it. Losing it gives the asset a '
    'new identity and breaks every level that references it."\n}}\n'
)

# Nama, pusat, dan setengah-ukuran tiap permukaan — angka yang sama persis dengan
# `cornell.simlevel`, hanya dipindahkan dari skala entity ke geometrinya.
#
# Dindingnya setebal 0,2 m dan berdiri di LUAR ruangan 5x5x5, jadi permukaan
# dalamnya tepat di ±2,5. Ruangan yang dindingnya menjorok ke dalam akan
# memantulkan cahaya dari tempat yang sedikit berbeda, dan Cornell box adalah
# adegan yang justru dipakai membandingkan pantulan.
SURFACES = [
    ("Floor", (0.0, -0.1, 0.0), (2.5, 0.1, 2.5)),
    ("Ceiling", (0.0, 5.1, 0.0), (2.5, 0.1, 2.5)),
    ("Back", (0.0, 2.5, -2.6), (2.5, 2.5, 0.1)),
    ("Left", (-2.6, 2.5, 0.0), (0.1, 2.5, 2.5)),
    ("Right", (2.6, 2.5, 0.0), (0.1, 2.5, 2.5)),
    ("Tall Block", (-0.85, 1.5, -0.7), (0.65, 1.5, 0.65)),
    ("Short Block", (0.95, 0.75, 0.75), (0.75, 0.75, 0.75)),
]


def guid(kind: str, name: str) -> str:
    return str(uuid.uuid5(NAMESPACE, kind + "/" + name))


def box_whitebox(half: tuple[float, float, float]) -> dict:
    """Kotak sebagai `WhiteboxData`, berpusat di titik asalnya sendiri.

    Urutan simpul tiap sisi berlawanan arah jarum jam **dilihat dari luar** —
    sama persis dengan `MakeUnitCube` di `HalfEdgeMesh.cpp`. Satu sisi yang
    terbalik tidak terlihat sebagai galat melainkan sebagai lubang di dinding.
    """
    hx, hy, hz = half
    vertices = [
        [-hx, -hy, -hz],  # 0
        [hx, -hy, -hz],  # 1
        [hx, hy, -hz],  # 2
        [-hx, hy, -hz],  # 3
        [-hx, -hy, hz],  # 4
        [hx, -hy, hz],  # 5
        [hx, hy, hz],  # 6
        [-hx, hy, hz],  # 7
    ]
    faces = [
        [4, 5, 6, 7],  # +Z depan
        [1, 0, 3, 2],  # -Z belakang
        [5, 1, 2, 6],  # +X kanan
        [0, 4, 7, 3],  # -X kiri
        [7, 6, 2, 3],  # +Y atas
        [0, 1, 5, 4],  # -Y bawah
    ]
    return {
        "schemaVersion": 1,
        "vertices": vertices,
        "faces": faces,
        "hiddenEdges": [],
        # -1 berarti "berkasnya tidak menyebut material untuk sisi ini", dan
        # penyunting mengisinya dengan material bawaan. Nol bukan penggantinya —
        # nol adalah slot pertama yang sah.
        "faceMaterials": [-1] * len(faces),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", default="~/Documents/SimEngine/FarmSim")
    parser.add_argument("--level", default="cornell-whitebox")
    args = parser.parse_args()

    project = pathlib.Path(args.project).expanduser()
    assets = project / "Assets" / "Whitebox" / "Cornell"
    assets.mkdir(parents=True, exist_ok=True)

    root_guid = guid("entity", "root")
    entities = [
        {
            "guid": root_guid,
            "components": {
                "Name": {"name": "Cornell Box (Whitebox)"},
                "Transform": {
                    "position": [0.0, 0.0, 0.0],
                    "rotation": [1.0, 0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                },
            },
        }
    ]

    for name, centre, half in SURFACES:
        file_name = name.replace(" ", "") + ".simwhitebox"
        asset_guid = guid("asset", name)
        (assets / file_name).write_text(json.dumps(box_whitebox(half), indent=2) + "\n")
        (assets / (file_name + ".meta")).write_text(META.format(asset_guid))

        entities.append(
            {
                "guid": guid("entity", name),
                "parent": root_guid,
                "components": {
                    "Name": {"name": name},
                    "Transform": {
                        "position": list(centre),
                        "rotation": [1.0, 0.0, 0.0, 0.0],
                        "scale": [1.0, 1.0, 1.0],
                    },
                    # **Whitebox saja, tanpa MeshRenderer.** Salah satu sudah
                    # cukup untuk tergambar: whitebox memberi bentuknya, dan slot
                    # material kosong digambar material bawaan. Menambahkan
                    # MeshRenderer yang tidak menunjuk mesh apa pun hanya
                    # menambah komponen yang harus dijelaskan.
                    "Whitebox": {"whitebox": asset_guid, "showEdges": True},
                    "Static": {"isStatic": True},
                },
            }
        )

    # Lampunya disalin apa adanya dari `cornell.simlevel`: yang dibandingkan
    # kedua adegan itu adalah geometrinya, jadi pencahayaannya tidak boleh ikut
    # berbeda.
    entities.append(
        {
            "guid": guid("entity", "light"),
            "parent": root_guid,
            "components": {
                "Name": {"name": "Room Light"},
                "Transform": {
                    "position": [0.0, 4.6, 0.0],
                    "rotation": [1.0, 0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                },
                "Light": {
                    "type": "Point",
                    "color": [1.0, 0.95, 0.88],
                    "intensity": 40.0,
                    "range": 12.0,
                    "sourceRadius": 0.1,
                    "innerAngle": 0.436,
                    "outerAngle": 0.611,
                    "castShadows": True,
                },
            },
        }
    )

    levels = project / "Levels"
    levels.mkdir(parents=True, exist_ok=True)
    path = levels / (args.level + ".simlevel")
    path.write_text(json.dumps({"schemaVersion": 3, "entities": entities}, indent=2) + "\n")

    print(f"{len(SURFACES)} aset whitebox di {assets}")
    print(f"level {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
