#!/usr/bin/env bash
# Shared Zephyr install: sets up everything under /opt, regardless of where this file lives
# or what directory it is run from. Stops at the first error. Needs sudo.
#
#   zephyr_setup.sh              full setup; skips anything already installed, so it is safe to re-run
#   zephyr_setup.sh link-boards  only (re)link the TH-1 boards into Zephyr
set -euo pipefail

ZEPHYR_VERSION="v4.4.2"
SDK_VERSION="1.0.1"
SDK_ARCH="linux-aarch64"
SDK_TOOLCHAINS=(arm-zephyr-eabi)

WS=/opt/zephyr-ws
SDK=/opt/zephyr-sdk-${SDK_VERSION}
SDK_URL=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VERSION}
SDK_TARBALL=zephyr-sdk-${SDK_VERSION}_${SDK_ARCH}_minimal.tar.xz

BOARDS_REPO=https://github.com/teahaz/TH-1-MO
BOARDS_DIR=/opt/TH-1-MO
BOARDS_SRC=$BOARDS_DIR/firmware/lib/boards/th-1
BOARDS_DST=$WS/zephyr/boards/th-1

if [ "$(id -u)" -eq 0 ]; then
    echo "Run as a normal user (not root); sudo is used where needed." >&2
    exit 1
fi
USER_NAME=$(id -un)
GROUP_NAME=$(id -gn)

# Zephyr's board scan does not follow directory symlinks, so mirror the directory tree
# and symlink each file. Covers every board under th-1/.
link_boards() {
    (cd "$BOARDS_SRC" && find . -mindepth 1 -type d) | while read -r d; do mkdir -p "$BOARDS_DST/$d"; done
    mkdir -p "$BOARDS_DST"
    (cd "$BOARDS_SRC" && find . -type f) | while read -r f; do ln -sfn "$BOARDS_SRC/${f#./}" "$BOARDS_DST/${f#./}"; done
}

if [ "${1:-}" = "link-boards" ]; then
    link_boards
    exit 0
elif [ $# -gt 0 ]; then
    echo "Usage: $0 [link-boards]" >&2
    exit 1
fi

# Everything below skips what is already installed, so the script is safe to re-run.

# 1. System packages (only the missing ones)
missing=()
for pkg in cmake gperf python3-venv ninja-build device-tree-compiler git wget xz-utils curl; do
    dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
done
if [ ${#missing[@]} -gt 0 ]; then
    sudo apt-get install -y "${missing[@]}"
fi

# 2. Workspace directory owned by the current user
[ -d "$WS" ] || sudo install -d -o "$USER_NAME" -g "$GROUP_NAME" "$WS"

# 3. Python venv + west (pip skips what is already satisfied)
[ -x "$WS/.venv/bin/python" ] || python3 -m venv "$WS/.venv"
"$WS/.venv/bin/pip" install west

# 4. Zephyr source at the pinned release, then modules
cd "$WS"
# shellcheck disable=SC1091
source "$WS/.venv/bin/activate"
[ -d "$WS/.west" ] || west init --mr "$ZEPHYR_VERSION" "$WS"
if [ ! -f "$WS/.west_update_done" ]; then
    west update -o=--depth=1 -n
    touch "$WS/.west_update_done"
fi

# 5. Zephyr Python requirements (pip skips what is already satisfied)
pip install -r "$WS/zephyr/scripts/requirements.txt"

# 6. Zephyr SDK (minimal bundle, verified against the release checksum file)
if [ ! -x "$SDK/setup.sh" ]; then
    DL=$(mktemp -d)
    trap 'rm -rf "$DL"' EXIT
    curl -fL -o "$DL/$SDK_TARBALL" "$SDK_URL/$SDK_TARBALL"
    curl -fL -o "$DL/sha256.sum" "$SDK_URL/sha256.sum"
    (cd "$DL" && grep " $SDK_TARBALL\$" sha256.sum | sha256sum -c -)
    sudo tar -xf "$DL/$SDK_TARBALL" -C /opt
    sudo chown -R "$USER_NAME:$GROUP_NAME" "$SDK"
fi

# setup.sh -h installs host tools; -t installs a toolchain. CMake package registry (-c) is skipped on purpose.
[ -d "$SDK/hosttools/sysroots" ] || "$SDK/setup.sh" -h
for tc in "${SDK_TOOLCHAINS[@]}"; do
    [ -d "$SDK/gnu/$tc" ] || "$SDK/setup.sh" -t "$tc"
done

# 7. TH-1/MO repo (clone if missing), then link every board under boards/th-1 into Zephyr.
#    Submodules are skipped on purpose: firmware/zephyr is a full Zephyr copy we don't need.
if [ ! -d "$BOARDS_DIR/.git" ]; then
    sudo install -d -o "$USER_NAME" -g "$GROUP_NAME" "$BOARDS_DIR"
    git clone "$BOARDS_REPO" "$BOARDS_DIR"
fi
link_boards

# 8. Environment file used by projects
cat > "$WS/env.sh" <<EOF
# Source this to use the shared Zephyr install:  source $WS/env.sh
source $WS/.venv/bin/activate
export ZEPHYR_BASE=$WS/zephyr
export ZEPHYR_SDK_INSTALL_DIR=$SDK
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
EOF

# 9. README describing the environment and how to use it
sed -e "s|@ZEPHYR_VERSION@|$ZEPHYR_VERSION|g" \
    -e "s|@SDK_VERSION@|$SDK_VERSION|g" \
    -e "s|@SDK_TOOLCHAINS@|${SDK_TOOLCHAINS[*]}|g" \
    -e "s|@WS@|$WS|g" \
    -e "s|@SDK@|$SDK|g" \
    -e "s|@BOARDS_DIR@|$BOARDS_DIR|g" > "$WS/README.md" <<'EOF'
# Shared Zephyr install

One Zephyr install shared by every Zephyr project on this machine.

| Component | Version | Location |
|-----------|---------|----------|
| Zephyr (west workspace) | @ZEPHYR_VERSION@ | `@WS@` |
| Zephyr SDK | @SDK_VERSION@ (minimal + host tools + `@SDK_TOOLCHAINS@`) | `@SDK@` |
| Python venv (west + Zephyr requirements) | | `@WS@/.venv` |
| TH-1 board files (symlinked into Zephyr) | | `@BOARDS_DIR@` |

Host: aarch64 (Raspberry Pi CM5). Every project uses this same Zephyr version.

## Building a project

Your project can live anywhere (e.g. `~/myapp`). It does not need to be inside the workspace.

```bash
source @WS@/env.sh      # activates the venv, sets ZEPHYR_BASE and ZEPHYR_SDK_INSTALL_DIR
cd ~/myapp                        # directory containing CMakeLists.txt and prj.conf
west build -b <board> .           # output goes to ~/myapp/build
```

`env.sh` is per shell session. Source it again in each new terminal.

The TH-1/MO board is available as `mo` (`west build -b mo .`).
EOF

echo "Done. Use it with: source $WS/env.sh"
