# Texture packs

The port can replace the game's textures with sharper, AI-upscaled versions while it runs. Packs
are made from **your own copy of the game** with
[ps4-texturepack-generator](https://github.com/tabnul/ps4-texturepack-generator) (profile `tlg`),
which reads the game folder without writing to it. Packs are not distributed with the port.

Packs are optional. The game runs exactly as before when no pack is installed or **Texture pack** is
set to *Off* (the default).

## Making a pack

Follow the generator's README: set it up once, then
```
texpack make "D:\Games\CUSA03745" "D:\Packs\UltraSharp 4x"
```
A full pack (colour 4x, normal maps 2x) takes a few hours on a recent NVIDIA card. The pack folder's
name is the name you see in the launcher and in game. Don't play while it builds: both want the GPU.

## Installing and using it

1. Put the pack folder in the texture pack folder: `texpack\` next to the port's exe by default, or
   the folder set under *Texture packs folder* in the launcher (environment variable `TLG_TEXPACK_DIR`).
   ```
   <texture pack folder>\UltraSharp 4x\index.txt
   <texture pack folder>\UltraSharp 4x\game\...
   ```
2. Pick it:
   - **Launcher**: *Image* > *Texture pack* lists the packs in that folder.
   - **In game**: the overlay menu (Insert) > *Image* > *Textures* > *Texture pack*. It switches
     live; textures appear as they load (a second or two).
   - **Presets** store the pack too: *Stock (as shipped)* is the game without any pack.

Several packs can sit side by side (for example a 2x and a 4x one); you pick one at a time.

### Video memory

Pack textures load when the game first draws them and are released again:
- textures not drawn for **30 seconds** are freed (they reload when needed),
- switching packs or setting *Off* frees all of them,
- the pack never takes more than **16 GB**; beyond that, textures stay at the original quality.

On cards with 8 GB or less, a 2x pack (`--scale 2`) is the safer choice (a quarter of the memory
per texture).

## Troubleshooting

| Problem | Fix |
|---|---|
| The pack does not show up | The folder must contain `index.txt` and sit directly in the texture pack folder (no extra folder level) |
| Some textures look unchanged | They are not in the pack (see the generator's *What goes in the pack*), or were not drawn long enough to load |

## For developers

`index.txt` lines are `<key> <width> <height> <dds path>`: the key is XXH3-64 of the texture's
tiled mip 0 as it sits in GPU memory, so a pack matches the game whatever file the texture came
from. The full format is in the generator's README.

The loader is `Vulkan::TexturePack` (`gpu/shadps4/video_core/renderer_vulkan/vk_texture_pack.cpp`).
Debug variables: `TLG_TEXPACK_LOG=1` (every load), `TLG_TEXPACK_TINT=1` (tints pack textures),
`TLG_TEXPACK_VMA=1` (video memory every 10 s).
