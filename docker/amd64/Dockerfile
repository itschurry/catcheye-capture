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
      >> ~/.bashrc; \
    printf '%s\n' \
      'alias cm="cmake -S . -B build/release-amd64 -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Release -DCATCHEYE_CAPTURE_ENABLE_LIBCAMERA=OFF && cmake --build build/release-amd64 --config Release -- -j $(nproc) && mkdir -p build && ln -sf $(realpath build/release-amd64/compile_commands.json) build/compile_commands.json"' \
      'alias ci="rm -rf install/release-amd64 && cmake --install build/release-amd64 --prefix install/release-amd64"' \
      'alias cc="rm -rf build install && find log -mindepth 1 ! -name .gitkeep -delete"' \
      > ~/.bash_aliases


CMD ["bash"]
