# AppLoad prerequisites for authentication

The qualified loader starts from [rm-appload v0.5.3](https://github.com/asivery/rm-appload/tree/5bb34a362f09f753f18bd6261558f8e2737aacdb),
commit `5bb34a362f09f753f18bd6261558f8e2737aacdb`. The patches here change only
AppLoad's QTFB implementation and one keyboard diagnostic. They contain no
application catalog, document integration, boot hook or firmware resources.

## Apply and rebuild

Apply in this order to a clean checkout of that exact revision:

```sh
patch --batch -d /absolute/path/to/rm-appload -p1 \
  < patches/appload-v0.5.3-qtfb-lifetime.patch
patch --batch -d /absolute/path/to/rm-appload -p1 \
  < patches/appload-v0.5.3-no-key-logging.patch
patch --batch -d /absolute/path/to/rm-appload -p1 \
  < patches/appload-v0.5.3-touch-lifecycle.patch
```

- **QTFB lifetime:** removes the disconnect worker's wait for GUI painting while
  holding the backend mutex. Reference-counted image storage retains shared
  memory until the last paint finishes; queued work checks the current image
  before modifying its controller. This prevents a closing client from blocking
  the stock UI or releasing a frame still being painted.
- **Keyboard privacy:** removes the key-label `console.log` from `Key.qml`.
  Layout, gestures, modifiers and wire packets remain unchanged. A rebuilt file
  on disk is insufficient: the running AppLoad must load that replacement
  before any credentials are typed.
- **Touch lifetime:** forwards optional QTFB input type `0x13`, with zeroed
  payload fields, before `TouchBegin` and on `TouchCancel`. The matching rmweb
  client cancels abandoned contacts without activating a control. Stationary
  points are no longer forwarded as duplicate presses. Existing packet sizes
  and pointer/key encodings remain unchanged; older clients ignore the new type.

Build the patched source with the official Ferrari SDK matching the device and
reviewed XOVI dependencies. Keep AppLoad's complete corresponding GPL-3.0 source,
patches, license and build receipt with any distributed AppLoad binary. rmweb's
build scripts do not build or install this dependency.

## Firmware qualification remains separate

These patches do **not** make stock AppLoad v0.5.3's QML hooks compatible with
all firmware. The physical passkey test used a separately qualified AppLoad
build for Paper Pro **3.28.0.172 / Qt 6.10.3**, including the 3.28 hook changes
from [upstream PR 59](https://github.com/asivery/rm-appload/pull/59), merged as
`3b42440e369a82535fb93df4a9155de9199cf487`. Those firmware hooks are not bundled
here. Use an AppLoad installation qualified for the exact firmware and preserve
its existing hook changes when applying these fixes. Do not substitute upstream
master without requalifying its scaling, rotation and stock-UI integration.

Back up the installed loader and its receipts, verify the rebuilt library and
resources, then activate through the device's existing reversible AppLoad
installation procedure. Test opening, keyboard input, touch cancellation,
Cancel/Return, reopening and stock-UI health. Replacing a loaded shared library
alone does not activate it. No installer or boot change is supplied here.

## Actual-source regression checks

Run on Linux with Qt 6.8 or newer Core, Gui and Quick, against the fully patched
source tree:

```sh
cmake -S patches/appload-tests -B build/appload-tests -G Ninja \
  -DAPPLOAD_SOURCE=/absolute/path/to/rm-appload
cmake --build build/appload-tests
ctest --test-dir build/appload-tests --output-on-failure
```

The fixture compiles the real socket loop and controller. Ten bounded cases
exercise initial association, close before painting, retained frames after
close, empty input, repeated opens, queued replacement, duplicate initialization,
negative framebuffer keys, SIGPIPE and touch-sequence reset. It uses private
socket pairs and an offscreen Qt platform, without the global server or tablet
files. Passing these checks does not qualify the firmware hooks or physical UI.
