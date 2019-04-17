# Start with current Ubuntu LTS
FROM ubuntu:18.04

# Add a PulseAudio's dependencies
RUN apt-get update && apt-get install -y \
    autoconf \
    automake \
    autopoint \
    bash-completion \
    check \
    dbus-x11 \
    g++ \
    gcc \
    gettext \
    git-core \
    libasound2-dev \
    libasyncns-dev \
    libavahi-client-dev \
    libbluetooth-dev \
    libcap-dev \
    libfftw3-dev \
    libglib2.0-dev \
    libgtk-3-dev \
    libice-dev \
    libjack-dev \
    liblircclient-dev \
    libltdl-dev \
    liborc-0.4-dev \
    libsbc-dev \
    libsndfile1-dev \
    libsoxr-dev \
    libspeexdsp-dev \
    libssl-dev \
    libsystemd-dev \
    libtdb-dev \
    libudev-dev \
    libwebrtc-audio-processing-dev \
    libwrap0-dev \
    libx11-xcb-dev \
    libxcb1-dev \
    libxml-parser-perl \
    libxml2-utils \
    libxtst-dev \
    make \
    ninja-build \
    python3-setuptools \
    systemd

# Install meson from upstream tarball
ARG MESON_VERSION=0.50.0
RUN apt-get install -y wget && \
    wget -q https://github.com/mesonbuild/meson/releases/download/${MESON_VERSION}/meson-${MESON_VERSION}.tar.gz && \
    tar -xf meson-${MESON_VERSION}.tar.gz && \
    cd meson-${MESON_VERSION} && \
    python3 setup.py install

# Add a user and set as default for the build. This is safer, in general, and
# allows us to avoid having to explicitly allow running as root in the
# check-daemon stage.
RUN groupadd -g 1000 a_group && \
    useradd a_user -u 1000 -g a_group -m
USER a_user:a_group

# And make sure subsequent commands are run in the user's home directory
WORKDIR /home/a_user
