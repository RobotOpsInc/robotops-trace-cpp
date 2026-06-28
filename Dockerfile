# syntax=docker/dockerfile:1.4

# ============================================================================
# Base stage - Common dependencies for all targets
# ============================================================================
#
# Parameterized by ROS distro so a single Dockerfile builds for either
# Jazzy (Ubuntu 24.04 Noble, primary) or Humble (Ubuntu 22.04 Jammy, the
# arm64/Jetson target). Pass --build-arg ROS_DISTRO=humble for the Humble build.
#
#   ROS_DISTRO  ROS 2 distribution (jazzy | humble). Selects the
#               `ros:${ROS_DISTRO}-ros-base` base image and /opt/ros/${ROS_DISTRO}.
#
# NOTE: the SDK core itself has NO ROS dependency (see the `cmake-standalone`
# CI job + ROBOTOPS_TRACE_STANDALONE in CMakeLists.txt). This image exists to
# build/test the *ament* packaging path and to bloom the .deb for apt-managed
# fleets.
ARG ROS_DISTRO=jazzy
FROM ros:${ROS_DISTRO}-ros-base AS base

# Re-declare after FROM so the value is in scope in the build stage.
ARG ROS_DISTRO

# Build argument for configurable APT repository environment
# Production: apt.robotops.com
# Development: apt.development.robotops.com
ARG APT_REPO_URL=https://apt.robotops.com

# Install core build dependencies and packaging tools
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    python3-colcon-common-extensions \
    python3-rosdep \
    python3-bloom \
    python3-pip \
    fakeroot \
    dpkg-dev \
    debhelper \
    curl \
    ca-certificates \
    gnupg \
    && rm -rf /var/lib/apt/lists/*

# Install just command runner. This MUST fail the image build immediately if the
# download/install fails or yields no usable binary — otherwise a flaky install
# silently "continues" and CI dies much later with `just: not found`, after a
# broken layer has already been cached (the humble-arm64 flake on ROB-441/443).
# Fail-hard + verify the binary is on PATH and runnable so a bad layer is never
# cached: a clean error here triggers a clean retry instead.
RUN curl -fsSL https://just.systems/install.sh | bash -s -- --to /usr/local/bin \
    && command -v just \
    && just --version

# Configure RobotOps APT repository so future SDK + integration deps resolve.
# The shared `robotops` aptly repo publishes the same package set to every
# Ubuntu codename (noble/jammy/focal), so pull from the channel matching this
# image's distro: jazzy → noble, humble → jammy. $UBUNTU_CODENAME is exported by
# /etc/os-release in the ros:${ROS_DISTRO}-ros-base base image.
RUN . /etc/os-release && \
    curl -fsSL ${APT_REPO_URL}/robotops-public-key.asc | gpg --dearmor -o /usr/share/keyrings/robotops-archive-keyring.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/robotops-archive-keyring.gpg] ${APT_REPO_URL} ${UBUNTU_CODENAME} main" \
    > /etc/apt/sources.list.d/robotops.list && \
    apt-get update

# Copy package.xml to install dependencies from it (single source of truth)
WORKDIR /workspace/src/robotops_trace_cpp
COPY package.xml .

# Initialize rosdep and install dependencies from package.xml.
# This respects version constraints declared in package.xml.
RUN rosdep update && \
    rosdep install --from-paths . --ignore-src -y --rosdistro ${ROS_DISTRO}

WORKDIR /workspace

# ============================================================================
# Development stage - For interactive development
# ============================================================================
FROM base AS dev

# Re-declare after FROM so ROS_DISTRO is in scope in this stage.
ARG ROS_DISTRO

# Source ROS in bashrc for interactive use
RUN echo "source /opt/ros/${ROS_DISTRO}/setup.bash" >> ~/.bashrc

CMD ["/bin/bash"]

# ============================================================================
# Test stage - Adds sanitizers for safety testing
# ============================================================================
FROM base AS test

# Install sanitizer libraries
RUN apt-get update && apt-get install -y \
    clang \
    llvm \
    libasan8 \
    libubsan1 \
    && rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash"]
