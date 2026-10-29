#!/usr/bin/env bash
# Builds dist/$PORT_SLUG-linux-x86_64.tar.gz after `bash build.sh`: "$PORT_NAME" (the Tk launcher,
# frozen with PyInstaller so players need no Python or Tk), out/tlg-probe with libtlggpu.so, the libraries they need that a desktop
# may lack (out/lib: SDL3, FFmpeg, Zydis, fmt, ...), the preparation scripts and the player README.
# No game or console file may ship.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
# shellcheck source=../port.env
source packaging/port.env
[[ -f out/tlg-probe && -f out/gpu/libtlggpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
name=${TLG_PACKAGE_NAME:-$PORT_SLUG-linux-x86_64}
python=${PYTHON:-python3}
if [[ ! -x out/pyenv/bin/python ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/bin/python -m pip install -q --disable-pip-version-check pyinstaller iced-x86==1.21.0
# The scripts run inside the executable (--script): the standard modules they import come along,
# and iced-x86 for scripts/isa_rewrite.py (CPUs without MOVBE/BMI1/LZCNT/SSE4A).
hidden=()
for module in argparse base64 collections hashlib json re shutil struct tempfile xml.etree.ElementTree \
              urllib.request runpy multiprocessing iced_x86; do
    hidden+=("$module")
done
# packaging/launcher.spec freezes the launcher into one folder.
TLG_SPEC_NAME=$PORT_NAME TLG_SPEC_ROOT=$PWD TLG_SPEC_TK=$PORT_LAUNCHER \
TLG_SPEC_HIDDEN="${hidden[*]}" \
    out/pyenv/bin/python -m PyInstaller --noconfirm --clean --log-level WARN \
    --distpath out/pyi-dist --workpath out/pyi-work packaging/launcher.spec

dest=out/stage/$name
rm -rf -- out/stage
mkdir -p "$dest/out/gpu" "$dest/out/lib" "$dest/launcher" "$dest/system_lib"
cp -r "out/pyi-dist/$PORT_NAME/." "$dest/"
cp launcher/$PORT_ICON.png "$dest/launcher/"
strip --strip-debug -o "$dest/out/tlg-probe" out/tlg-probe
strip --strip-debug -o "$dest/out/gpu/libtlggpu.so" out/gpu/libtlggpu.so
[[ -f out/tlg-gpu-capabilities ]] && strip -o "$dest/out/tlg-gpu-capabilities" out/tlg-gpu-capabilities
# Libraries to bundle: everything the two ELF files load except the base system (glibc, the C++
# runtime, the Vulkan loader and drivers, X11/Wayland, audio servers), which must come from the
# player's distribution to match its drivers.
system='^(ld-linux|libc|libm|libdl|libpthread|librt|libmvec|libresolv|libutil|libstdc\+\+|libgcc_s|libvulkan|libGL|libEGL|libGLX|libGLdispatch|libX|libxcb|libwayland|libdrm|libasound|libpulse|libpipewire|libudev|libdbus|libsystemd|libz\.|libgomp|libcap|libffi)'
ldd out/tlg-probe out/gpu/libtlggpu.so | awk '/=> \// {print $1, $3}' | sort -u | while read -r soname path; do
    [[ $soname == libtlggpu.so ]] && continue
    [[ $soname =~ $system ]] && continue
    cp -L "$path" "$dest/out/lib/$soname"
done
# SDL3 loads its video/audio backends at run time from the player's system: nothing else to copy.
# DLAA (optional): the bridge and NVIDIA's runtime from build_dlss.sh, with NVIDIA's license (the
# DLSS SDK license allows shipping the runtime inside an application). Without them the DLAA
# setting is greyed out and the game keeps its own anti-aliasing.
if [[ -f out/libtlg_dlss.so && -f out/NVIDIA-DLSS-LICENSE.txt ]] && compgen -G 'out/libnvidia-ngx-dlss.so.*' >/dev/null; then
    strip -o "$dest/out/libtlg_dlss.so" out/libtlg_dlss.so
    cp out/libnvidia-ngx-dlss.so.* out/NVIDIA-DLSS-LICENSE.txt "$dest/out/"
    echo "Packaged DLSS: libtlg_dlss.so, $(cd out && echo libnvidia-ngx-dlss.so.*)"
elif [[ -n ${TLG_REQUIRE_DLSS:-} ]]; then
    echo 'TLG_REQUIRE_DLSS is set but the DLSS bridge is missing from out/: run build_dlss.sh' >&2
    exit 1
else
    echo 'No DLSS bridge in out/ (build_dlss.sh not run): packaging without DLAA'
fi
cp -r scripts "$dest/"
find "$dest" -name __pycache__ -prune -exec rm -r {} +
cp LICENSE packaging/README-player.md "$dest/"
cp packaging/linux/install_desktop.sh "$dest/"
# The release name (vYYYY.MM.DD-<commit> or the pushed v* tag, as the CI publishes it): the launcher
# shows it and compares it with the latest release.
if [[ ${GITHUB_REF:-} == refs/tags/v* ]]; then echo "$GITHUB_REF_NAME" > "$dest/VERSION"
elif [[ -n ${GITHUB_SHA:-} ]]; then printf 'v%s-%s\n' "$(date -u +%Y.%m.%d)" "${GITHUB_SHA::7}" > "$dest/VERSION"; fi
cat > "$dest/system_lib/LEIA-ME.txt" <<'TXT'
Coloque aqui o libSceNgs2.sprx do firmware do seu PS4 (/system/common/lib) para ter som.
Put libSceNgs2.sprx from your PS4 firmware (/system/common/lib) here for audio.
TXT
bash packaging/check_package.sh "$dest"
mkdir -p dist
tar -C out/stage -czf "dist/$name.tar.gz" "$name"
du -sh "$dest" "dist/$name.tar.gz"
