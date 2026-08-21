#!/usr/bin/env python3
"""Memasang Sponza FBX sebagai adegan uji yang **hanya bertekstur base color**.

**Kembarannya `sponza-assets.py`, dan bedanya justru gunanya.** Yang itu memasang
adegan selengkapnya dari glTF: normal, kekasaran, kelogaman, dan mode alfa. Yang
ini memasang jalur yang sesempit mungkin — satu tekstur per material, langsung ke
`baseColor`, tanpa satu pun sambungan lain — dari berkas mesh yang lain pula.

Ia ada untuk menjawab satu pertanyaan dengan mengurangi, bukan menambah: kalau
sebuah bercak tetap muncul di adegan sesederhana ini, ia tidak mungkin datang
dari peta normal, dari kelogaman, dari uji alfa, maupun dari importir glTF.

**Dan FBX menyebut berkas decal yang berbeda dari glTF-nya.** glTF menunjuk
`dirt_decal_01_..._Opacity.png` — RGBA, 28,1% beralfa di bawah 10; FBX menunjuk
`dirt_decal_01.png` — RGB tanpa alfa. Keduanya membawa warna kotoran yang sama
persis, luminansi rata 74,9, dan tidak satu piksel hitam pun di seluruh 4096².
Yang membedakan apa yang terjadi pada keduanya adalah jalur beralfa di baker,
bukan isinya.

Daftar materialnya tidak diurai di sini: FBX bukan teks. Yang membacanya
importir engine lewat `SimHeadless --dump-mesh`, dan skrip ini memakai
keluarannya. Larik `materials` di level diindeks **nomor ruas**, dan berkas dump
itulah yang menyebutkan ruas ke berapa memakai material yang mana.

Pemakaian:
    build/.../SimHeadless --project P --no-render \\
        --dump-mesh P/Assets/Sponza/NewSponza_Main_Yup_003.fbx --dump-out fbx.json
    python3 Tools/sponza-fbx-basecolor.py --dump fbx.json
"""

import argparse
import json
import pathlib
import uuid

# Ruang nama tersendiri, terpisah dari adegan glTF: dua adegan yang memakai
# tekstur yang sama tetap harus punya material yang berbeda, karena yang
# membedakannya justru apa yang **tidak** disambungkan.
NAMESPACE = uuid.UUID("5900f2a4-0000-4000-8000-000000000100")
MESH = "NewSponza_Main_Yup_003.fbx"
META = (
    '{{\n  "version": 1,\n  "guid": "{}",\n'
    '  "note": "Keep this file next to its asset and commit it. Losing it gives the asset a '
    'new identity and breaks every level that references it."\n}}\n'
)

# Ruang nama tekstur adegan glTF. Berkas teksturnya benda yang sama persis, dan
# `.meta`-nya sudah ada di sebelahnya — memberi identitas kedua pada berkas yang
# sama akan membuat dua GUID menunjuk satu berkas, dan indeks aset menolak yang
# kedua tanpa menyebut kenapa.
TEXTURE_NAMESPACE = uuid.UUID("5900f2a4-0000-4000-8000-000000000000")


def guid(kind: str, name: str) -> str:
    return str(uuid.uuid5(NAMESPACE, kind + "/" + name))


def texture_guid(name: str) -> str:
    return str(uuid.uuid5(TEXTURE_NAMESPACE, "texture/" + name))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dump", required=True, help="keluaran SimHeadless --dump-mesh")
    parser.add_argument("--project", default="~/Documents/SimEngine/FarmSim")
    parser.add_argument("--source", default="/home/arie/SDK/main_sponza")
    parser.add_argument("--level", default="Resources/Levels/gi-sponza-fbx.simlevel")
    parser.add_argument("--from-level", default="Resources/Levels/gi-sponza.simlevel")
    parser.add_argument(
        "--flat",
        action="store_true",
        help="warna penanda per material lewat emissive, tanpa tekstur sama sekali",
    )
    args = parser.parse_args()

    dump = json.loads(pathlib.Path(args.dump).read_text())
    assets = pathlib.Path(args.project).expanduser() / "Assets" / "Sponza"
    source = pathlib.Path(args.source).expanduser()

    mesh_path = assets / MESH
    if not mesh_path.is_symlink() and not mesh_path.exists():
        mesh_path.symlink_to(source / MESH)
    (assets / (MESH + ".meta")).write_text(META.format(guid("mesh", MESH)))

    # Tekstur yang belum punya `.meta` — FBX menyebut beberapa berkas yang tidak
    # dipakai glTF, dan `dirt_decal_01.png` yang justru intinya salah satunya.
    texture_dir = assets / "textures"
    texture_dir.mkdir(exist_ok=True)
    added = 0
    for material in dump["materials"]:
        base = material["baseColor"]
        if not base:
            continue
        name = pathlib.Path(base).name
        link = texture_dir / name
        if not link.is_symlink() and not link.exists():
            link.symlink_to(source / "textures" / name)
            added += 1
        meta = texture_dir / (name + ".meta")
        if not meta.exists():
            meta.write_text(META.format(texture_guid(name)))

    material_dir = assets / "MaterialsBaseColor"
    material_dir.mkdir(exist_ok=True)
    slots = []
    for index, material in enumerate(dump["materials"]):
        name = material["name"] or f"material_{index}"
        output = guid("node/out", name)
        nodes = [{"guid": output, "type": "output.surface", "position": [240.0, 80.0]}]
        links = []

        if args.flat:
            # **Lewat `emissive`, bukan `baseColor`.** Yang dicari di sini
            # "ruas ini materialnya yang mana", dan albedo apa pun masih dikali
            # pencahayaan sebelum sampai ke layar — dua material bisa keluar
            # dengan warna yang sama hanya karena yang satu di bayangan. Emissive
            # ditambahkan sesudah shading, jadi ia sampai apa adanya.
            hue = (index * 360.0 / max(len(dump["materials"]), 1)) % 360.0
            sector, frac = divmod(hue / 60.0, 1.0)
            up, down = frac, 1.0 - frac
            wheel = [(1, up, 0), (down, 1, 0), (0, 1, up), (0, down, 1), (up, 0, 1), (1, 0, down)]
            r, g, b = wheel[int(sector) % 6]
            nodes[0]["pins"] = {
                "baseColor": "float3(0.0000, 0.0000, 0.0000)",
                "emissive": f"float3({r:.4f}, {g:.4f}, {b:.4f})",
            }
            path = material_dir / f"{name}.simmat"
            path.write_text(
                json.dumps({"version": 1, "parameters": [], "nodes": nodes, "links": links},
                           indent=2)
                + "\n"
            )
            (material_dir / f"{name}.simmat.meta").write_text(META.format(guid("material", name)))
            slots.append(guid("material", name))
            print(f"  {index:2d} {name:26s} emissive {r:.2f} {g:.2f} {b:.2f}")
            continue

        base = material["baseColor"]
        if base:
            texture_node = guid("node/tex", name)
            sample_node = guid("node/smp", name)
            stem = pathlib.Path(base).name
            nodes.append(
                {
                    "guid": texture_node,
                    "type": "input.texture",
                    "position": [-420.0, 40.0],
                    "settings": {"name": pathlib.Path(stem).stem, "texture": texture_guid(stem)},
                }
            )
            nodes.append({"guid": sample_node, "type": "input.sample", "position": [-160.0, 40.0]})
            links.append(
                {
                    "guid": guid("link/t", name),
                    "from": [texture_node, "texture"],
                    "to": [sample_node, "texture"],
                }
            )
            links.append(
                {
                    "guid": guid("link/base", name),
                    "from": [sample_node, "rgb"],
                    "to": [output, "baseColor"],
                }
            )
        else:
            # Material tanpa tekstur base color tetap dibuat: yang diuji adegan
            # ini adalah ruas mana memakai apa, dan ruas yang materialnya hilang
            # akan digambar jalur mundur — permukaan yang tidak bisa dibedakan
            # dari material yang memang begitu.
            nodes[0]["pins"] = {"baseColor": "float3(0.5000, 0.5000, 0.5000)"}

        path = material_dir / f"{name}.simmat"
        path.write_text(
            json.dumps({"version": 1, "parameters": [], "nodes": nodes, "links": links}, indent=2)
            + "\n"
        )
        (material_dir / f"{name}.simmat.meta").write_text(META.format(guid("material", name)))
        slots.append(guid("material", name))

    # Level disalin dari adegan glTF supaya kamera, matahari, dan langitnya sama
    # persis — yang berbeda hanya mesh dan materialnya, dan itulah yang sedang
    # dibandingkan.
    level = json.loads(pathlib.Path(args.from_level).read_text())
    for entity in level["entities"]:
        if entity["components"].get("Name", {}).get("name") == "Sponza":
            renderer = entity["components"]["MeshRenderer"]
            renderer["mesh"] = guid("mesh", MESH)
            # Diindeks nomor ruas, dan ruas ke-i memakai material `parts[i]`.
            renderer["materials"] = [slots[m] if 0 <= m < len(slots) else slots[0]
                                     for m in dump["parts"]]
        if entity["components"].get("Name", {}).get("name") == "GI Sponza":
            entity["components"]["Name"]["name"] = "GI Sponza FBX"

    pathlib.Path(args.level).write_text(json.dumps(level, indent=2) + "\n")
    print(f"{added} tekstur baru ditautkan, {len(slots)} material base-color, level ditulis")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
