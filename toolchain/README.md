# Cross-compile toolchain (reMarkable Paper Pro "ferrari")

A Docker `linux/arm64` image that bundles reMarkable's **official ferrari Yocto SDK**, used to
cross-compile everything we deploy to the device.

## Prerequisites (macOS, Apple Silicon)

- A `linux/arm64` Docker engine. We use **colima** (lightweight, no GUI):
  ```sh
  brew install colima docker docker-buildx
  mkdir -p ~/.docker/cli-plugins
  ln -sfn "$(brew --prefix)/opt/docker-buildx/bin/docker-buildx" ~/.docker/cli-plugins/docker-buildx
  colima start --cpu 14 --memory 32 --disk 150 --vm-type vz --mount-type virtiofs
  ```
- **`docker-buildx` is required** — the Dockerfile uses `RUN --mount=type=bind` (BuildKit) to install
  the SDK without baking the 446 MB installer into an image layer.

## Build the image

```sh
./scripts/fetch-sdk.sh                                   # downloads the ~446 MB SDK installer (gitignored)
docker build -f toolchain/Dockerfile -t rmweb-sdk .      # installs the SDK into the image at /opt/rmpp-sdk
```

Get an interactive cross-compile shell (SDK env auto-sourced, repo mounted at `/work`):
```sh
docker run --rm -it -v "$PWD":/work rmweb-sdk
```

## Verified toolchain facts (image `rmweb-sdk`, 2026-06-24)

| | |
|---|---|
| Compiler | `aarch64-remarkable-linux-gcc` — **GCC 13.4.0** |
| Default flags | `-mcpu=cortex-a53+crc+crypto -mbranch-protection=standard` |
| Sysroot | `/opt/rmpp-sdk/sysroots/cortexa53-crypto-remarkable-linux` (contains `ld-linux-aarch64.so.1`, glibc 2.39) |
| Env script | `source /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux` (sets `$CC`,`$CXX`,`$CFLAGS`,`$SDKTARGETSYSROOT`,`$CMAKE_TOOLCHAIN_FILE`,…) |
| Base CFLAGS | `-O2 -pipe -g -feliminate-unused-debug-types` |

The SDK matches device firmware 3.27.x (scarthgap, glibc 2.39) — ABI-compatible with the on-device
libraries (see `../docs/device-profile.md`).

## Helper scripts

- `scripts/build.sh '<cmd>'` — run `<cmd>` inside the container with the SDK env sourced and the repo
  mounted. Example: `scripts/build.sh '$CC -O2 hello/hello.c -o build/hello'`.
- `scripts/deploy.sh <binary>` — `scp` a binary to `/home/root/rmweb/bin/` on the device and run it.

## Notes

- `FROM --platform=linux/arm64` is pinned deliberately: the SDK ships **aarch64** toolchain binaries,
  so the build container must be arm64 for them to run natively (a harmless BuildKit lint warning results).
- The SDK install dir (`/opt/rmpp-sdk`) lives only inside the image; nothing is installed on the host.
