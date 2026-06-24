# syntax=docker/dockerfile:1
# Cross-compilation environment for the reMarkable Paper Pro ("ferrari").
# Installs reMarkable's official aarch64 Yocto SDK (scarthgap, glibc 2.39, -mcpu=cortex-a53).
FROM --platform=linux/arm64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      bash file xz-utils python3 make cmake ninja-build perl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# The SDK installer (~467 MB) is bind-mounted from the build context (not COPYed),
# so it never lands in an image layer. Run via `sh` so no +x bit is needed.
# Filename hardcoded (matches scripts/fetch-sdk.sh) for reproducibility.
RUN --mount=type=bind,source=toolchain/sdk/remarkable-production-image-5.7.119-ferrari-public-aarch64-toolchain.sh,target=/tmp/sdk.sh \
    sh /tmp/sdk.sh -y -d /opt/rmpp-sdk

# Convenience: auto-source the SDK env in interactive shells.
RUN echo '. /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux' >> /root/.bashrc
WORKDIR /work
CMD ["/bin/bash"]
