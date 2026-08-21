#!/usr/bin/env python3
"""Memasang Intel Sponza sebagai aset project, beserta 28 material dan 72 teksturnya.

**Kenapa sebuah skrip dan bukan berkas yang di-commit.** Geometrinya 140 MB dan
teksturnya 2,6 GB, sementara `Resources/` disalin utuh ke direktori binary setiap
build — menaruhnya di sana berarti membayar salinan itu setiap kali. Yang
di-commit hanya levelnya (`Resources/Levels/gi-sponza.simlevel`) dan skrip ini;
asetnya dipasang sebagai symlink ke tempat modelnya diunduh.

**Kenapa GUID-nya diturunkan, bukan diacak.** Level menyebut material menurut
GUID. GUID acak berarti menjalankan skrip ini dua kali menghasilkan dua identitas
untuk aset yang sama, dan level yang sudah menyebut yang lama berhenti menemukan
apa pun. Di sini seluruhnya `uuid5` dari nama berkasnya, jadi menjalankannya ulang
menghasilkan angka yang sama persis.

Pemakaian:
    python3 Tools/sponza-assets.py [--source DIR] [--project DIR]
"""

import argparse
import json
import pathlib
import uuid

# Ruang nama GUID adegan ini. Angkanya sembarang; yang penting ia tidak pernah
# berubah, karena setiap GUID di bawah diturunkan darinya.
NAMESPACE = uuid.UUID("5900f2a4-0000-4000-8000-000000000000")
MESH = "NewSponza_Main_glTF_003.gltf"
META = (
    '{{\n  "version": 1,\n  "guid": "{}",\n'
    '  "note": "Keep this file next to its asset and commit it. Losing it gives the asset a '
    'new identity and breaks every level that references it."\n}}\n'
)


def guid(kind: str, name: str) -> str:
    return str(uuid.uuid5(NAMESPACE, kind + "/" + name))


def link(target: pathlib.Path, at: pathlib.Path) -> None:
    if at.is_symlink() or at.exists():
        at.unlink()
    at.symlink_to(target)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default="/home/arie/SDK/main_sponza")
    parser.add_argument("--project", default="~/Documents/SimEngine/FarmSim")
    parser.add_argument("--level", default="Resources/Levels/gi-sponza.simlevel")
    args = parser.parse_args()

    source = pathlib.Path(args.source).expanduser()
    assets = pathlib.Path(args.project).expanduser() / "Assets" / "Sponza"
    gltf = json.loads((source / MESH).read_text())

    images = [image.get("uri", "") for image in gltf["images"]]
    textures = [images[t["source"]] if "source" in t else "" for t in gltf["textures"]]

    def named(view):
        return textures[view["index"]] if view else None

    # Urutan ruas mesh adalah urutan material **saat pertama ditemui** menelusuri
    # node lalu primitive — sama persis dengan yang dilakukan importir glTF, dan
    # `GroupByMaterial` mengurutkan ruasnya menurut indeks itu. Larik `materials`
    # di level diindeks nomor ruas, jadi kedua urutan ini harus sama.
    order, seen = [], {}
    for node in gltf["nodes"]:
        if "mesh" not in node:
            continue
        for primitive in gltf["meshes"][node["mesh"]].get("primitives", []):
            material = primitive.get("material")
            if material is not None and material not in seen:
                seen[material] = len(order)
                order.append(material)

    assets.mkdir(parents=True, exist_ok=True)
    link(source / MESH, assets / MESH)
    link(source / MESH.replace(".gltf", ".bin"), assets / MESH.replace(".gltf", ".bin"))
    (assets / (MESH + ".meta")).write_text(META.format(guid("mesh", MESH)))

    # --- tekstur ---------------------------------------------------------
    #
    # Satu symlink per berkas, bukan satu symlink untuk seluruh direktori:
    # `.meta`-nya harus berada di sebelah asetnya, dan menulisnya lewat symlink
    # direktori berarti menulis ke dalam folder unduhan aslinya.
    texture_dir = assets / "textures"
    if texture_dir.is_symlink():
        texture_dir.unlink()
    texture_dir.mkdir(exist_ok=True)
    needed = set()
    for index in order:
        material = gltf["materials"][index]
        pbr = material.get("pbrMetallicRoughness", {})
        for view in (
            pbr.get("baseColorTexture"),
            pbr.get("metallicRoughnessTexture"),
            material.get("normalTexture"),
        ):
            name = named(view)
            if name:
                needed.add(name)
    for relative in sorted(needed):
        name = pathlib.Path(relative).name
        link(source / relative, texture_dir / name)
        (texture_dir / (name + ".meta")).write_text(META.format(guid("texture", name)))

    # --- material --------------------------------------------------------
    material_dir = assets / "Materials"
    material_dir.mkdir(exist_ok=True)
    slots = []
    for index in order:
        material = gltf["materials"][index]
        name = material.get("name", f"material_{index}")
        pbr = material.get("pbrMetallicRoughness", {})
        output = guid("node/out", name)
        nodes, links, pins = [], [], {}

        def sample(relative: str, label: str, y: float) -> str:
            base = pathlib.Path(relative).name
            texture_node = guid("node/tex/" + label, name)
            sample_node = guid("node/smp/" + label, name)
            nodes.append(
                {
                    "guid": texture_node,
                    "type": "input.texture",
                    "position": [-420.0, y],
                    "settings": {
                        "name": pathlib.Path(base).stem,
                        "texture": guid("texture", base),
                    },
                }
            )
            nodes.append({"guid": sample_node, "type": "input.sample", "position": [-160.0, y]})
            links.append(
                {
                    "guid": guid("link/t" + label, name),
                    "from": [texture_node, "texture"],
                    "to": [sample_node, "texture"],
                }
            )
            return sample_node

        base_color = named(pbr.get("baseColorTexture"))
        if base_color:
            node = sample(base_color, "base", 40.0)
            links.append(
                {"guid": guid("link/base", name), "from": [node, "rgb"], "to": [output, "baseColor"]}
            )
        else:
            factor = pbr.get("baseColorFactor", [0.8, 0.8, 0.8, 1.0])
            pins["baseColor"] = f"float3({factor[0]:.4f}, {factor[1]:.4f}, {factor[2]:.4f})"

        normal = named(material.get("normalTexture"))
        if normal:
            node = sample(normal, "normal", 300.0)
            links.append(
                {"guid": guid("link/normal", name), "from": [node, "rgb"], "to": [output, "normal"]}
            )

        rough_metal = named(pbr.get("metallicRoughnessTexture"))
        if rough_metal:
            # Konvensi glTF: kekasaran di kanal G, kelogaman di B. Satu tekstur,
            # dua jawaban — dan itulah sebabnya berkasnya bernama gabungan
            # keduanya.
            node = sample(rough_metal, "mr", 560.0)
            links.append(
                {
                    "guid": guid("link/rough", name),
                    "from": [node, "g"],
                    "to": [output, "specularRoughness"],
                }
            )
            links.append(
                {
                    "guid": guid("link/metal", name),
                    "from": [node, "b"],
                    "to": [output, "baseMetalness"],
                }
            )
        else:
            pins["specularRoughness"] = f"{pbr.get('roughnessFactor', 0.5):.4f}"
            pins["baseMetalness"] = f"{pbr.get('metallicFactor', 0.0):.4f}"

        surface = {"guid": output, "type": "output.surface", "position": [240.0, 80.0]}
        if pins:
            surface["pins"] = pins
        nodes.insert(0, surface)

        path = material_dir / f"{name}.simmat"
        path.write_text(
            json.dumps({"version": 1, "parameters": [], "nodes": nodes, "links": links}, indent=2)
            + "\n"
        )
        (material_dir / f"{name}.simmat.meta").write_text(META.format(guid("material", name)))
        slots.append(guid("material", name))

    level_path = pathlib.Path(args.level)
    level = json.loads(level_path.read_text())
    for entity in level["entities"]:
        if entity["components"].get("Name", {}).get("name") == "Sponza":
            entity["components"]["MeshRenderer"]["materials"] = slots
    level_path.write_text(json.dumps(level, indent=2) + "\n")

    print(f"{len(needed)} tekstur, {len(slots)} material, level diperbarui")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
