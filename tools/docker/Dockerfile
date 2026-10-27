# Linux build and Vulkan check: Debian with SDL3, GCC and Mesa's software Vulkan driver.
#   docker build -t cmr-linux tools/docker
#   docker run --rm -v "$PWD":/src:ro -v /path/to/disc:/disc:ro cmr-linux bash -c '
#     cmake -S /src -B /build -G Ninja -DGEN_DIR=/src/generated && cmake --build /build &&
#     xvfb-run -a /build/cmrally "/disc/Colin McRae Rally.cue" --mute --turbo --exit-after 3000'
# (generated/ comes from tools/build_game.py on the host.) CMR_GPU_DEBUG=1 adds the Vulkan
# validation layers.
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build gcc libc6-dev python3 libsdl3-dev \
        xvfb xauth mesa-vulkan-drivers libvulkan1 vulkan-tools vulkan-validationlayers \
    && rm -rf /var/lib/apt/lists/*
