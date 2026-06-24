# Phase 0 — Foundations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish a reproducible cross-compilation environment for the reMarkable Paper Pro and prove it end-to-end by running a freshly cross-compiled aarch64 binary on the real device.

**Architecture:** A Docker `linux/arm64` image installs reMarkable's official "ferrari" Yocto SDK (scarthgap, glibc 2.39, `-mcpu=cortex-a53`). Host scripts download the SDK, run cross-compiles inside the container, and deploy/run binaries on the device over SSH. The deliverable is a `hello` program that prints the device's SoC/kernel — confirming toolchain ABI, dynamic linking, and the deploy path all work.

**Tech Stack:** Docker (linux/arm64), reMarkable ferrari OECORE SDK, GCC 13 (`aarch64-remarkable-linux`), bash, SSH/scp.

> **Note on test cycles:** Phase 0 is build/infrastructure, not unit-testable code. Each task's "test" is a concrete verification gate (build succeeds / `file` reports the right ELF / the device prints expected output). The TDD discipline maps to: state the expected observable result first, then make it true, then verify, then commit.

## Global Constraints

- Install target on device is **`/home/root/rmweb/`** only — never `/` (rootfs is 91% full). Copied verbatim from spec §2.
- **Cross-compile only** — no compiler on device. Toolchain = official **ferrari aarch64 SDK** (`3.27.0.97`, scarthgap, **glibc 2.39**, tune **`-mcpu=cortex-a53`**, sysroot `cortexa53-crypto-remarkable-linux`).
- Device SSH: `root@10.11.99.1`, key auth already configured. Userland is **BusyBox** (`head -n N`, not `head -N`).
- `.env` (device password) is gitignored — never commit it.
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## File Structure

- `.env.example` — template of the connection vars (no secrets).
- `.gitignore` — extend to ignore `toolchain/sdk/` (large installer) and `build/`.
- `scripts/fetch-sdk.sh` — download the ferrari aarch64 SDK installer into `toolchain/sdk/`.
- `toolchain/Dockerfile` — linux/arm64 build image with the SDK installed at `/opt/rmpp-sdk`.
- `toolchain/README.md` — how to build the image and cross-compile.
- `scripts/build.sh` — run a cross-compile command inside the container (mounts the repo).
- `scripts/deploy.sh` — scp a built binary to `/home/root/rmweb/bin/` and run it on the device.
- `hello/hello.c` — minimal aarch64 proof program (prints SoC/kernel/arch).

---

### Task 1: Project scaffold & ignore rules

**Files:**
- Create: `.env.example`, `hello/.gitkeep`, `scripts/.gitkeep`, `toolchain/.gitkeep`
- Modify: `.gitignore`

**Interfaces:**
- Produces: the directory layout (`scripts/`, `toolchain/`, `hello/`) and ignore rules every later task relies on.

- [ ] **Step 1: Create the directories and `.env.example`**

`.env.example`:
```sh
# reMarkable Paper Pro — connection (copy to .env and fill the password)
REMARKABLE_HOST=10.11.99.1
REMARKABLE_USER=root
REMARKABLE_PASSWORD=
```

- [ ] **Step 2: Extend `.gitignore`**

Append:
```gitignore
# cross-compile SDK installer (large, fetched on demand)
toolchain/sdk/
# build outputs
build/
```

- [ ] **Step 3: Verify ignore rule works**

Run: `mkdir -p toolchain/sdk && touch toolchain/sdk/probe && git status --short toolchain/sdk`
Expected: **no output** (the path is ignored). Then `rm toolchain/sdk/probe`.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "Phase 0: project scaffold and ignore rules

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Fetch the ferrari aarch64 SDK

**Files:**
- Create: `scripts/fetch-sdk.sh`

**Interfaces:**
- Produces: `toolchain/sdk/remarkable-production-image-5.7.119-ferrari-public-aarch64-toolchain.sh` (gitignored), consumed by Task 3's Dockerfile.

- [ ] **Step 1: Write `scripts/fetch-sdk.sh`**

```sh
#!/usr/bin/env bash
set -euo pipefail
URL="https://storage.googleapis.com/remarkable-codex-toolchain/3.27.0.97/ferrari/remarkable-production-image-5.7.119-ferrari-public-aarch64-toolchain.sh"
SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)/toolchain/sdk"
mkdir -p "$SDK_DIR"
out="$SDK_DIR/$(basename "$URL")"
if [ -f "$out" ]; then echo "Already present: $out"; exit 0; fi
echo "Downloading ferrari aarch64 SDK (~467 MB)…"
curl -fL --progress-bar -o "$out" "$URL"
echo "Saved: $out ($(du -h "$out" | cut -f1))"
```

- [ ] **Step 2: Make executable and run it**

Run: `chmod +x scripts/fetch-sdk.sh && ./scripts/fetch-sdk.sh`
Expected: downloads ~467 MB, prints `Saved: …aarch64-toolchain.sh (446M)` (approx).

- [ ] **Step 3: Verify it's a real OE SDK self-extractor**

Run: `head -c 256 toolchain/sdk/*aarch64-toolchain.sh`
Expected: a shell-script header containing a line like `# This is a self-extracting archive` and a reference to the OE/SDK relocation logic.

- [ ] **Step 4: Commit**

```bash
git add scripts/fetch-sdk.sh
git commit -m "Phase 0: SDK fetch script (ferrari aarch64 toolchain)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Docker build environment with the SDK installed

**Files:**
- Create: `toolchain/Dockerfile`, `toolchain/README.md`

**Interfaces:**
- Consumes: the SDK installer from Task 2.
- Produces: a Docker image tagged `rmweb-sdk` with the toolchain at `/opt/rmpp-sdk` and an env script `environment-setup-cortexa53-crypto-remarkable-linux`. Consumed by Tasks 4–5 and all later phases.

- [ ] **Step 1: Verify Docker is available (precondition)**

Run: `docker version --format '{{.Server.Os}}/{{.Server.Arch}}'`
Expected: `linux/arm64` (Docker Desktop on Apple Silicon). If this fails, STOP and resolve Docker availability before continuing.

- [ ] **Step 2: Write `toolchain/Dockerfile`**

```dockerfile
# syntax=docker/dockerfile:1
FROM --platform=linux/arm64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      bash file xz-utils python3 make cmake ninja-build perl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ARG SDK_INSTALLER=remarkable-production-image-5.7.119-ferrari-public-aarch64-toolchain.sh
COPY toolchain/sdk/${SDK_INSTALLER} /tmp/sdk.sh
RUN chmod +x /tmp/sdk.sh && /tmp/sdk.sh -y -d /opt/rmpp-sdk && rm /tmp/sdk.sh

# Convenience: auto-source the SDK env in interactive shells
RUN echo '. /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux' >> /root/.bashrc
WORKDIR /work
CMD ["/bin/bash"]
```

- [ ] **Step 3: Build the image**

Run: `docker build -f toolchain/Dockerfile -t rmweb-sdk .`
Expected: completes; the SDK install step prints the OE relocation/`Setting it up...done` messages.

- [ ] **Step 4: Verify the toolchain inside the image**

Run:
```bash
docker run --rm rmweb-sdk bash -lc '. /opt/rmpp-sdk/environment-setup-* && echo "CC=$CC" && $CC --version | head -n1 && echo "SYSROOT=$SDKTARGETSYSROOT" && ls "$SDKTARGETSYSROOT/lib/ld-linux-aarch64.so.1"'
```
Expected: `CC=aarch64-remarkable-linux-gcc …`, a GCC 13.x version line, a sysroot path under `/opt/rmpp-sdk/sysroots/cortexa53-crypto-remarkable-linux`, and the `ld-linux-aarch64.so.1` path listed (confirming glibc sysroot present). If the env-setup filename differs, note the actual name from `ls /opt/rmpp-sdk/environment-setup-*` and update the Dockerfile/scripts.

- [ ] **Step 5: Write `toolchain/README.md`**

Document: prerequisites (Docker Desktop, Apple Silicon), `./scripts/fetch-sdk.sh`, `docker build -f toolchain/Dockerfile -t rmweb-sdk .`, and how to get a shell (`docker run --rm -it -v "$PWD":/work rmweb-sdk`). Record the verified env-setup filename, `$CC`, and sysroot path from Step 4.

- [ ] **Step 6: Commit**

```bash
git add toolchain/Dockerfile toolchain/README.md
git commit -m "Phase 0: Docker build env with ferrari aarch64 SDK

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Hello-world cross-compile

**Files:**
- Create: `hello/hello.c`, `scripts/build.sh`

**Interfaces:**
- Consumes: the `rmweb-sdk` image from Task 3.
- Produces: `build/hello` (aarch64 ELF), consumed by Task 5; and `scripts/build.sh BUILD_CMD…` used by later phases to run any cross-compile in the container.

- [ ] **Step 1: Write `hello/hello.c`**

```c
#include <stdio.h>
#include <sys/utsname.h>

static void cat(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("(cannot read %s)\n", path); return; }
    char buf[256];
    while (fgets(buf, sizeof buf, f)) fputs(buf, stdout);
    fclose(f);
}

int main(void) {
    struct utsname u;
    uname(&u);
    printf("rmweb hello — running on the reMarkable Paper Pro\n");
    printf("  kernel : %s %s\n", u.sysname, u.release);
    printf("  arch   : %s\n", u.machine);
    printf("  soc    : "); cat("/sys/devices/soc0/machine");
    return 0;
}
```

- [ ] **Step 2: Write `scripts/build.sh`**

```sh
#!/usr/bin/env bash
set -euo pipefail
# Run a cross-compile command inside the SDK container, repo mounted at /work.
# Usage: scripts/build.sh '<command run after sourcing the SDK env>'
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD":/work -w /work rmweb-sdk \
  bash -lc ". /opt/rmpp-sdk/environment-setup-* && $*"
```

- [ ] **Step 3: Build hello in the container**

Run:
```bash
chmod +x scripts/build.sh
./scripts/build.sh 'mkdir -p build && $CC -O2 -Wall hello/hello.c -o build/hello'
```
Expected: no errors; `build/hello` is created.

- [ ] **Step 4: Verify the binary is a correct aarch64 dynamic ELF**

Run: `file build/hello`
Expected: `build/hello: ELF 64-bit LSB pie executable, ARM aarch64, … dynamically linked, interpreter /lib/ld-linux-aarch64.so.1, …`

- [ ] **Step 5: Commit**

```bash
git add hello/hello.c scripts/build.sh
git commit -m "Phase 0: hello-world cross-compile + container build helper

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Deploy & run on the device (end-to-end proof)

**Files:**
- Create: `scripts/deploy.sh`

**Interfaces:**
- Consumes: `build/hello` from Task 4; `.env` for the host/user.
- Produces: `scripts/deploy.sh <local-binary>` — used by every later phase to push a binary to `/home/root/rmweb/bin/` and run it.

- [ ] **Step 1: Write `scripts/deploy.sh`**

```sh
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f .env ] && . ./.env
HOST="${REMARKABLE_HOST:-10.11.99.1}"
USER="${REMARKABLE_USER:-root}"
BIN="${1:?usage: deploy.sh <local-binary>}"
NAME="$(basename "$BIN")"
ssh "$USER@$HOST" 'mkdir -p /home/root/rmweb/bin'
scp "$BIN" "$USER@$HOST:/home/root/rmweb/bin/$NAME"
echo "=== running /home/root/rmweb/bin/$NAME on device ==="
ssh "$USER@$HOST" "/home/root/rmweb/bin/$NAME"
```

- [ ] **Step 2: Deploy and run**

Run: `chmod +x scripts/deploy.sh && ./scripts/deploy.sh build/hello`
Expected output from the device:
```
rmweb hello — running on the reMarkable Paper Pro
  kernel : Linux 6.12.49…
  arch   : aarch64
  soc    : reMarkable Ferrari
```

- [ ] **Step 3: Verify (the verification gate)**

The output above IS the test: a binary cross-compiled with the ferrari SDK ran on the device, the dynamic linker resolved glibc, and it read a device sysfs file. If `arch` = `aarch64` and `soc` = `reMarkable Ferrari`, Phase 0 is proven.

- [ ] **Step 4: Commit**

```bash
git add scripts/deploy.sh
git commit -m "Phase 0: device deploy script; end-to-end cross-compile proven

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 5: Update task tracker**

Mark tracker tasks "Фаза 0: …" complete; record the verified toolchain facts (env-setup name, `$CC`, sysroot) in `toolchain/README.md` and `CLAUDE.md` if not already.

---

## Self-Review

**Spec coverage:** Phase 0 in spec §5 = "git + structure + docs; reproducible Docker build env with the ferrari aarch64 SDK; hello world cross-compiles, deploys to `/home`, runs on device." → Tasks 1 (structure), 2–3 (SDK + Docker env), 4 (cross-compile), 5 (deploy to `/home/root/rmweb`, run on device). Docs: `toolchain/README.md` (Task 3/5); git already initialized; project docs committed earlier. Covered.

**Placeholder scan:** No "TBD/TODO/handle edge cases" — every step has concrete commands and expected output. The only deliberately-deferred item is the exact `environment-setup-*` filename, which Task 3 Step 4 explicitly discovers and records (not a placeholder — a verification step).

**Type/path consistency:** `rmweb-sdk` image name, `/opt/rmpp-sdk` install dir, `/home/root/rmweb/bin/` device path, and `scripts/{fetch-sdk,build,deploy}.sh` are used consistently across tasks. `scripts/build.sh` (container compile) and `scripts/deploy.sh` (device run) are the two reusable helpers later phases depend on.

**Risk note:** Tasks 2–3 are partly exploratory (SDK installer behavior in a container). If the installer needs different flags or the `linux/arm64` base hits an issue, fix inline and update `toolchain/README.md` with what actually worked.
