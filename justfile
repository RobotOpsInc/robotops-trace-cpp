# robotops_trace_cpp development commands
# Install just: https://github.com/casey/just

# Load credentials from .env.local (optional)
set dotenv-load := true
set dotenv-filename := ".env.local"

# ROS 2 distro selecting the build image (ros:${ROS_DISTRO}-ros-base) and the
# sourced /opt/ros/${ROS_DISTRO} underlay. MUST match the distro the .deb is
# built/linked for (jazzy → Ubuntu 24.04 Noble, the default; humble → Ubuntu
# 22.04 Jammy, the arm64/Jetson target). Override with:
#   just ROS_DISTRO=humble <recipe>
export ROS_DISTRO := env_var_or_default('ROS_DISTRO', 'jazzy')

# APT repo the Docker base image pulls RobotOps deps from.
export APT_REPO_URL := env_var_or_default('APT_REPO_URL', 'https://apt.robotops.com')

# Default recipe - show available commands
default:
    @just --list

# ----------------------------------------------------------------------------
# Version Management
# ----------------------------------------------------------------------------

# Bump version (usage: just bump-version patch|minor|major)
bump-version type:
    #!/usr/bin/env bash
    set -euo pipefail

    if [[ "{{type}}" != "patch" && "{{type}}" != "minor" && "{{type}}" != "major" ]]; then
        echo "Error: type must be 'patch', 'minor', or 'major'"
        exit 1
    fi

    CURRENT=$(grep '<version>' package.xml | sed 's/.*<version>\(.*\)<\/version>.*/\1/')
    IFS='.' read -r major minor patch <<< "$CURRENT"
    DATE=$(date +%Y-%m-%d)

    # Calculate new version
    case "{{type}}" in
        patch)
            NEW_VERSION="$major.$minor.$((patch + 1))"
            ;;
        minor)
            NEW_VERSION="$major.$((minor + 1)).0"
            ;;
        major)
            NEW_VERSION="$((major + 1)).0.0"
            echo "⚠️  MAJOR VERSION BUMP: $CURRENT -> $NEW_VERSION"
            echo "⚠️  A core major bump fans out a coordinated integrations bump."
            echo "   - robotops-trace-cpp (this repo)"
            echo "   - robotops-trace-python"
            echo "   - robotops-trace-integrations"
            ;;
    esac

    echo "Bumping version: $CURRENT -> $NEW_VERSION"

    # Update package.xml (source of truth)
    sed -i.bak "s|<version>$CURRENT</version>|<version>$NEW_VERSION</version>|" package.xml
    rm package.xml.bak

    # Add changelog entry
    {
        echo "$NEW_VERSION ($DATE)"
        echo "-------------------"
        echo ""
        echo "*"
        echo ""
    } > /tmp/changelog_entry.txt

    # Insert at the top of CHANGELOG.rst (before the first version entry)
    awk '/^[0-9]+\.[0-9]+\.[0-9]+ \(/ { if (!inserted) { system("cat /tmp/changelog_entry.txt"); inserted=1 } } { print }' CHANGELOG.rst > /tmp/CHANGELOG.rst.new
    mv /tmp/CHANGELOG.rst.new CHANGELOG.rst
    rm /tmp/changelog_entry.txt

    echo "✅ Version bumped to $NEW_VERSION"
    echo "📝 Edit CHANGELOG.rst to add your changes"

# ----------------------------------------------------------------------------
# Docker (ament/apt build path)
# ----------------------------------------------------------------------------

# Build the development Docker image
build:
    DOCKER_BUILDKIT=1 docker-compose build dev

# Build all Docker images (dev + test)
build-all:
    DOCKER_BUILDKIT=1 docker-compose build

# Start interactive development shell
dev:
    docker-compose run --rm dev

# Build the ROS2 package (inside container via colcon)
compile:
    docker-compose run --rm build

# Run all tests
test:
    DOCKER_BUILDKIT=1 docker-compose run --rm test

# Clean build artifacts
clean:
    rm -rf build/ install/ log/
    docker-compose down -v

# ----------------------------------------------------------------------------
# Standalone (plain-CMake, no ROS) — the "survives beyond ROS" guardrail
# ----------------------------------------------------------------------------

# Build the core with plain CMake, no ament/ROS sourced (spec §2.2)
standalone:
    cmake -B build-standalone -S . -DROBOTOPS_TRACE_STANDALONE=ON
    cmake --build build-standalone
    ./build-standalone/robotops_trace_version_check

# ----------------------------------------------------------------------------
# CI commands
# ----------------------------------------------------------------------------

# Run lint checks (in container)
ci-lint:
    #!/usr/bin/env bash
    set -exo pipefail
    echo "🔍 Running lint checks..."
    source /opt/ros/${ROS_DISTRO}/setup.bash
    cd /workspace
    colcon build --packages-select robotops_trace_cpp
    colcon test --packages-select robotops_trace_cpp --ctest-args -R lint
    colcon test-result --verbose

# Run tests (in container)
ci-test:
    #!/usr/bin/env bash
    set -exo pipefail
    echo "🧪 Running tests..."
    source /opt/ros/${ROS_DISTRO}/setup.bash
    cd /workspace
    colcon build --packages-select robotops_trace_cpp --cmake-args -DCMAKE_BUILD_TYPE=Debug
    colcon test --packages-select robotops_trace_cpp --event-handlers console_direct+
    colcon test-result --verbose

# Build the core with plain CMake, no ROS sourced (the franchise guardrail)
ci-standalone:
    #!/usr/bin/env bash
    set -exo pipefail
    echo "🧱 Building standalone (no ROS)..."
    cmake -B build-standalone -S . -DROBOTOPS_TRACE_STANDALONE=ON
    cmake --build build-standalone
    ./build-standalone/robotops_trace_version_check

# Run full CI suite (for GitHub Actions, already in container)
ci-inner:
    #!/usr/bin/env bash
    set -exo pipefail
    echo "🚀 Running CI suite (in container)..."
    just ci-lint
    just ci-test
    echo "✅ CI suite completed!"
