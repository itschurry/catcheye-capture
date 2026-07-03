FROM --platform=$TARGETPLATFORM debian:trixie

LABEL authors="itschurry"
LABEL maintainer="itschurry"

ARG DEBIAN_FRONTEND=noninteractive
ARG TARGETARCH
ARG LIBCAMERA_REF=main

ARG UID=1000
ARG GID=1000
ARG USERNAME=user
ARG HOME=/home/${USERNAME}

ENV TZ=Asia/Seoul
ENV CMAKE_PREFIX_PATH=/usr/local:/usr
# ENV PKG_CONFIG_PATH=/usr/local/lib/aarch64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
# ENV GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0:/usr/lib/aarch64-linux-gnu/gstreamer-1.0

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      tzdata ca-certificates curl git sudo unzip wget zip \
      python3 python3-pip python3-venv \
      build-essential cmake file ninja-build pkg-config meson \
      python3-jinja2 python3-ply python3-yaml python3-pyelftools \
      flex bison xz-utils \
      libgstreamer1.0-dev \
      libgstreamer-plugins-base1.0-dev \
      libdrm-dev \
      libevent-dev \
      libexif-dev \
      libgnutls28-dev \
      libdw-dev \
      libudev-dev \
      libyaml-dev \
      libfmt-dev \
      libopencv-dev \
      libyaml-cpp-dev \
      libspdlog-dev \
      libblas-dev \
      liblapack-dev \
      libopenblas-pthread-dev \
      libjpeg-dev \
      libpng-dev \
      libtiff-dev \
      libgpiod-dev \
      gpiod; \
    apt-get clean; \
    rm -rf /var/lib/apt/lists/*

# User setup
RUN set -eux; \
    if ! getent group "${GID}" >/dev/null; then \
      groupadd -g "${GID}" "${USERNAME}"; \
    fi; \
    useradd -m -u "${UID}" -g "${GID}" -d "${HOME}" -s /bin/bash "${USERNAME}"; \
    echo "${USERNAME} ALL=(ALL) NOPASSWD: ALL" > "/etc/sudoers.d/${USERNAME}"; \
    chmod 0440 "/etc/sudoers.d/${USERNAME}"; \
    usermod -aG sudo,video,dialout,plugdev "${USERNAME}"

USER ${USERNAME}
WORKDIR ${HOME}

RUN set -eux; \
    printf '%s\n' \
      'export CMAKE_PREFIX_PATH=/usr/local:/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}' \
      'multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"' \
      'export PKG_CONFIG_PATH="/usr/local/lib/${multiarch}/pkgconfig:/usr/local/lib/pkgconfig:/usr/lib/${multiarch}/pkgconfig:/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"' \
      'export GST_PLUGIN_PATH="/usr/local/lib/${multiarch}/gstreamer-1.0:/usr/lib/${multiarch}/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"' \
      >> ~/.bashrc; \
    printf '%s\n' \
      'alias cm="cmake -S . -B build/release-arm64 -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Release -DCATCHEYE_CAPTURE_ENABLE_LIBCAMERA=ON && cmake --build build/release-arm64 --config Release -- -j $(nproc) && mkdir -p build && ln -sf $(realpath build/release-arm64/compile_commands.json) build/compile_commands.json"' \
      'alias ci="rm -rf install/release-arm64 && cmake --install build/release-arm64 --prefix install/release-arm64"' \
      'alias cc="rm -rf build install && find log -mindepth 1 ! -name .gitkeep -delete"' \
      > ~/.bash_aliases

# Raspberry Pi libcamera, built natively inside the arm64 container.
RUN set -eux; \
    git clone --branch "${LIBCAMERA_REF}" --depth 1 https://github.com/raspberrypi/libcamera.git; \
    meson setup libcamera/build libcamera \
      --buildtype=release \
      --prefix /usr \
      --libdir lib/aarch64-linux-gnu \
      -Dpipelines=rpi/vc4,rpi/pisp \
      -Dipas=rpi/vc4,rpi/pisp \
      -Dv4l2=enabled \
      -Dgstreamer=enabled \
      -Dtest=false \
      -Dlc-compliance=disabled \
      -Dcam=disabled \
      -Dqcam=disabled \
      -Ddocumentation=disabled \
      -Dpycamera=disabled; \
    ninja -C libcamera/build -j"$(nproc)"; \
    sudo meson install -C libcamera/build; \
    sudo ldconfig

CMD ["bash"]
