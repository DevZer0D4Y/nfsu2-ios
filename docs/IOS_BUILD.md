# Building for iPhone, iPad and macOS

This fork adds an Apple host to the NFSU2 Xbox recompilation: a native **Metal** renderer
(MoltenVK/Vulkan as a fallback), touch controls, controller support, in-app game import and
background pause. Everything below happens on **your own Mac with your own copy of the game**:
no game data or lifted code is in this repository, and none may be shared.

What you need:

- An Apple-silicon Mac with **Xcode** (26 or newer) and **Homebrew**, ~15 GB free disk.
- Your own **NFS Underground 2 Xbox disc, NTSC-U** (`default.xbe` SHA-1
  `428b5a70b9400f19cc5be7679db6c72a6f80a1c9`), as an Xbox ISO image.
- For an iPhone/iPad: an Apple ID signed in to Xcode (a free one works; its apps run for 7 days,
  then you reinstall). iOS 17 or newer for the Metal renderer; iOS 16 runs MoltenVK.

## 1. Tools

```sh
brew install cmake ninja sdl2 molten-vk vulkan-loader vulkan-headers vulkan-tools \
             glslang spirv-tools libepoxy openssl@3
python3 -m venv xboxrecomp/.venv
xboxrecomp/.venv/bin/pip install 'capstone<6'
```

## 2. The game files

Extract the disc image into a folder of its own (here `~/nfsu2/game`):

```sh
cd xboxrecomp
.venv/bin/python -m tools.xiso unpack "/path/to/your.iso" -o ~/nfsu2/game
cd ..
shasum ~/nfsu2/game/default.xbe     # must be 428b5a70b9400f19cc5be7679db6c72a6f80a1c9
```

## 3. Lift the XBE to C

Translates the game's executable into C under `gen/` (about 3 minutes; `gen/` is git-ignored and
must never be committed or shared):

```sh
PATH=$PWD/xboxrecomp/.venv/bin:$PATH NFSU2_XBE=~/nfsu2/game/default.xbe \
    NFSU2_GEN_DIR=$PWD/gen tools/regen.sh
```

This step also runs on Windows: see [No Mac? Lift the code on Windows](#no-mac-lift-the-code-on-windows).

## 4. macOS (to try it on the Mac)

```sh
tools/build_ffmpeg_apple.sh macos              # movie decoder (FFmpeg source: see step 5)
cmake -S . -B build-macos-vk -G Ninja -DNFSU2_GEN_DIR=$PWD/gen -DNFSU2_VULKAN=ON \
      -DCMAKE_PREFIX_PATH=/opt/homebrew -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
      -DNFSU2_FFMPEG_DIR=$HOME/nfsu2/deps/macos
cmake --build build-macos-vk
mkdir -p run && cd run
XDG_DATA_HOME=$PWD/data NFSU2_GAME_DIR=$HOME/nfsu2/game ../build-macos-vk/nfsu2_recomp
```

Leave out `-DNFSU2_FFMPEG_DIR` to skip FFmpeg (movies then use the game's own, slower decoder).
`NFSU2_RENDERER=vulkan` selects MoltenVK instead of Metal. A controller connected to the Mac works;
without one, the keyboard is the controller (`NFSU2_KEYBOARD=0` turns that off):

| Keys | Xbox controller |
|---|---|
| Arrow keys | d-pad and left stick (menus, steering) |
| W / S | right / left trigger (gas / brake, reverse) |
| A / D | steer left / right |
| Enter | A and Start together (Press START, OK, select, pause) |
| Esc or Backspace | B (back) |
| Space / Left Shift | X / Y |
| Q / E | White / Black |
| Tab / P | Back / Start |
| I J K L | right stick |

## 5. iOS dependencies

Download into `~/nfsu2/deps/dl/` and unpack under `~/nfsu2/deps/src/` (one folder each):

| Library | Get | Unpack to |
|---|---|---|
| SDL2 2.32.10 | `https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz` | `src/sdl2/` |
| glslang 16.6.0 | `https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz` | `src/glslang/` |
| MoltenVK 1.4.2 (device) | `MoltenVK-ios.tar` from `https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.4.2` | `src/moltenvk/` |
| MoltenVK 1.4.2 (Simulator) | `MoltenVK-all.tar`, same release | `src/moltenvk-sim/` |
| FFmpeg 7.1.1 | `https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz` (sha256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`) | `src/ffmpeg/` |

Then build them for the device (use `sim` instead of `device` for the iOS Simulator):

```sh
tools/build_ios_deps.sh device       # SDL2, glslang, MoltenVK -> ~/nfsu2/deps/device
tools/build_ffmpeg_apple.sh device   # FFmpeg (vp6 only, LGPL, dynamic frameworks)
```

## 6. Build and install on the iPhone

Pick your own bundle ID and Team ID (Xcode → Settings → Accounts):

```sh
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_PREFIX_PATH=$HOME/nfsu2/deps/device \
  -DCMAKE_FIND_ROOT_PATH=$HOME/nfsu2/deps/device -DNFSU2_GEN_DIR=$PWD/gen -DNFSU2_VULKAN=ON \
  -DNFSU2_FFMPEG_DIR=$HOME/nfsu2/deps/device \
  -DNFSU2_IOS_BUNDLE_ID=com.yourname.nfsu2x -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YOURTEAMID \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="Apple Development"
xcodebuild -project build-ios/nfsu2_recomp.xcodeproj -scheme nfsu2_recomp -configuration Release \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
xcrun devicectl list devices                                  # your iPhone's identifier
xcrun devicectl device install app --device <UDID> build-ios/Release-iphoneos/nfsu2_recomp.app
```

The first time, allow the developer on the phone: Settings → General → VPN & Device Management.
After a change to a CMake file, run `cmake build-ios` again before `xcodebuild`.

## 7. Put the game on the phone

Either:

- **In the app:** copy the game folder to the phone (Files app, AirDrop, or Finder → your iPhone →
  Files → NFSU2), start the app, and pick the folder when it asks. It checks `default.xbe` and
  copies the game into place; or
- **From the Mac:**
  ```sh
  xcrun devicectl device copy to --device <UDID> --domain-type appDataContainer \
    --domain-identifier com.yourname.nfsu2x --source ~/nfsu2/game --destination Documents/game
  ```

## Playing

- **Controls:** a paired controller (DualSense, Xbox, MFi) works and hides the touch controls; without
  one, an on-screen pad appears (stick, A/B/X/Y, gas and brake, Start/Back).
- **Settings:** `Documents/nfsu2x_env.txt` on the phone (Files app → NFSU2), one `KEY=VALUE` per line,
  the same names as the Linux/Switch builds. Useful ones:

  | Setting | Effect |
  |---|---|
  | `NFSU2_RENDERER=vulkan` | MoltenVK instead of the native Metal renderer |
  | `NFSU2_TOUCH=0` / `=2` | no touch controls / always show them |
  | `RECOMP_GL_SCALE=1.5` | render at 1.5x (sharper, more GPU) |
  | `RECOMP_FPS_LOG=1` | fps, CPU, heat and sound health every 10 s in `nfsu2x_log.txt` |
  | `RECOMP_AUDIO_VOLUME=50` | master volume 0–100 |

- The log of the last run is `Documents/nfsu2x_log.txt`; saves live in `Documents/game/UDATA`.
- Leaving the app pauses the game, its sound and all GPU work.

## No Mac? Lift the code on Windows

Translating the game's code to C (step 3) doesn't need a Mac: it's plain Python and runs the same
under WSL2 on Windows (tested on Ubuntu 24.04: about 2 minutes, identical output on every run). You
still need a Mac, your own or a rented cloud Mac, for everything from step 4 on: Xcode, signing and
installing on the iPhone.

Set up WSL2 with Ubuntu (PowerShell as administrator, then reboot if asked), open **Ubuntu** from
the Start menu and create your Linux user:

```powershell
wsl --install -d Ubuntu-24.04
```

Everything below runs in the Ubuntu terminal. Work under `~`, not under `/mnt/c/...`: Windows
drives are much slower from WSL. Refresh the package lists first, or an older WSL install fails with
"404 Not Found":

```sh
sudo apt-get update
sudo apt-get install -y git python3-venv p7zip-full     # p7zip only if your image is in a .7z
git clone --branch ios https://github.com/myUdav4iik/nfsu2-ios.git ~/nfsu2-ios
cd ~/nfsu2-ios
python3 -m venv xboxrecomp/.venv
xboxrecomp/.venv/bin/pip install 'capstone<6'

cp "/mnt/c/Users/<you>/path/to/your.iso" ~/              # C: is /mnt/c
cd xboxrecomp
.venv/bin/python -m tools.xiso unpack ~/your.iso -o ~/nfsu2/game
cd ..
sha1sum ~/nfsu2/game/default.xbe     # must be 428b5a70b9400f19cc5be7679db6c72a6f80a1c9

PATH=$PWD/xboxrecomp/.venv/bin:$PATH NFSU2_XBE=~/nfsu2/game/default.xbe \
    NFSU2_GEN_DIR=$PWD/gen tools/regen.sh
```

Don't point the system `python3` at another version (for example with `update-alternatives`):
Ubuntu's own tools, `apt` included, need its default Python.

The result is `gen/`, 83 files and about 130 MB. Pack it and copy it to the Mac over a private
route: a USB drive, your local network, or `scp` to a Mac you control:

```sh
tar czf ~/nfsu2-gen.tar.gz -C ~/nfsu2-ios gen
scp ~/nfsu2-gen.tar.gz you@your-mac:~/
```

On the Mac, clone the same branch and commit, unpack it into the repo
(`tar xzf ~/nfsu2-gen.tar.gz -C <repo>`), install the tools from step 1 except the Python venv, and
continue from step 4. To check that the copy arrived intact, compare
`cd gen && find . -type f | LC_ALL=C sort | xargs sha256sum` on Windows with the same command using
`shasum -a 256` on the Mac. The game folder still has to reach the phone (step 7).

> [!CAUTION]
> **`gen/` is the game's own code, translated.** It is derived from EA's copyrighted executable,
> like the disc itself. Never upload or share it: no GitHub (it's in `.gitignore`; keep it there),
> no public cloud links, chat servers or file-sharing sites. Move it only between machines you
> control, and delete copies you no longer need, including the tarball and anything left on a
> rented cloud Mac when the rental ends. The same goes for a built app: it contains this code, so
> everyone builds their own from their own disc.

## Known issues

- Career free-roam radio speech can drop out on the iPhone (fine on macOS); see
  `docs/IOS_PORT_LOG.md`.

## Licences

See `docs/APPLE_NOTICES.md` for the libraries the Apple builds include and their licences.
