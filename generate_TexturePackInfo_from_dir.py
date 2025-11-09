import os
import json

# Root folder containing textures
ROOT = "Textures"

# Output JSON file
OUTPUT_FILE = "TexturePackInfo.json"

# Metadata you want to include
DESCRIPTION = {
    "id": "ngg.mw.packs.palmont_4k",
    "game": "NFS Carbon",
    "name": "PALMONT_4K",
    "description": "palmont_4k",
    "author": "sh2"
}

def collect_textures(root_folder):
    texture_list = []

    for root, dirs, files in os.walk(root_folder):
        for file in files:
            if file.lower().endswith(".dds"):
                rel_path = os.path.join(root, file).replace("\\", "/")

                # Remove leading "Textures/"
                rel_path_inside = rel_path[len(root_folder) + 1:]

                game_id = os.path.splitext(file)[0]

                texture_list.append({
                    "gameId": game_id,
                    "texturePath": rel_path_inside
                })

    return texture_list


def build_json():
    textures = collect_textures(ROOT)

    data = {
        "description": DESCRIPTION,
        "rootDirectory": ROOT,
        "textureMappings": textures
    }

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

    print(f"✅ Generated {OUTPUT_FILE} with {len(textures)} textures.")


if __name__ == "__main__":
    build_json()
