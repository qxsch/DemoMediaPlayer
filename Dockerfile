# ============================================================
#  DemoMediaPlayer – Docker cross-compilation for Windows x64
#
#  Usage:
#    docker build --target dist --output type=local,dest=./dist .
#
#  Override the mpv-dev download URL:
#    docker build --build-arg MPV_DEV_URL="https://..." \
#                 --target dist --output type=local,dest=./dist .
# ============================================================

FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive

# ── Install cross-compilation toolchain ──────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-mingw-w64-x86-64 \
        wget curl ca-certificates \
        p7zip-full jq \
    && rm -rf /var/lib/apt/lists/*

# ── Download mpv development files (headers + import lib + DLL) ──
#
# By default the latest release from shinchiro/mpv-winbuild-cmake
# on GitHub is fetched.  Override MPV_DEV_URL to pin a build.
ARG MPV_DEV_URL=""

RUN set -e; \
    mkdir -p /opt/mpv-dev; \
    if [ -z "$MPV_DEV_URL" ]; then \
        echo ">>> Querying latest mpv-dev release from GitHub …"; \
        MPV_DEV_URL=$( \
            curl -fsSL \
                "https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest" \
            | jq -r '[ .assets[] \
                        | select(.name | test("mpv-dev-x86_64.*\\.7z$"; "i")) ] \
                      | first | .browser_download_url' \
        ); \
        if [ "$MPV_DEV_URL" = "null" ] || [ -z "$MPV_DEV_URL" ]; then \
            echo "ERROR: Could not find mpv-dev asset."; \
            echo "Set --build-arg MPV_DEV_URL=<url> manually."; \
            exit 1; \
        fi; \
    fi; \
    echo ">>> Downloading: $MPV_DEV_URL"; \
    wget -q --show-progress -O /tmp/mpv-dev.7z "$MPV_DEV_URL"; \
    cd /opt/mpv-dev && 7z x -y /tmp/mpv-dev.7z > /dev/null; \
    rm -f /tmp/mpv-dev.7z; \
    echo ">>> mpv-dev contents:"; \
    find /opt/mpv-dev -type f | sort

# ── Discover paths inside the extracted archive ──────────────────
RUN set -e; \
    INC=$(dirname "$(find /opt/mpv-dev -path '*/mpv/client.h' | head -1)"); \
    INC=${INC%/mpv}; \
    LIB=$(dirname "$(find /opt/mpv-dev -name 'libmpv.dll.a' | head -1)"); \
    DLL=$(find /opt/mpv-dev \( -name 'libmpv-2.dll' -o -name 'mpv-2.dll' \) \
          | head -1); \
    printf 'MPV_INC=%s\nMPV_LIB=%s\nMPV_DLL=%s\n' "$INC" "$LIB" "$DLL" \
        | tee /opt/mpv.env


# ── Compile ──────────────────────────────────────────────────────
WORKDIR /build
COPY src/ src/
COPY app.rc .
COPY app.manifest .
COPY icon.ico .

RUN set -e; . /opt/mpv.env; \
    echo ">>> Compiling resources …"; \
    x86_64-w64-mingw32-windres app.rc -o app_res.o; \
    echo ">>> Compiling …"; \
    x86_64-w64-mingw32-gcc -o mediaplayer.exe src/*.c app_res.o \
        -Isrc -I"$MPV_INC" -L"$MPV_LIB" \
        -lmpv \
        -lcomdlg32 -luser32 -lgdi32 -lole32 -lshell32 \
        -ldwmapi -lcomctl32 -luxtheme \
        -lwinmm \
        -lm \
        -mwindows -municode \
        -O2 -s -static-libgcc; \
    echo ">>> Build OK"; \
    ls -lh mediaplayer.exe

# ── Package distributable files ──────────────────────────────────
RUN set -e; . /opt/mpv.env; \
    mkdir /dist; \
    cp mediaplayer.exe /dist/; \
    cp "$MPV_DLL" /dist/; \
    echo ">>> dist/ contents:"; \
    ls -lh /dist/

# ── Export stage (used with --output) ────────────────────────────
FROM scratch AS dist
COPY --from=builder /dist/ /
