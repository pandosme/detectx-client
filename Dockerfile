ARG ARCH=aarch64
ARG VERSION=12.8.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

#-------------------------------------------------------------------------------
# Stage 1: Build ACAP application (DetectX Client - no local model)
#-------------------------------------------------------------------------------
FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

WORKDIR /opt/app

# Copy application source, headers, and prebuilt libraries
COPY ./app .

# Ensure local lib and include directories exist (if not already present)
RUN mkdir -p lib include

# Build and package ACAP application with settings files only
# DetectX Client uses remote Hub for inference, so no model files needed
ARG CHIP=
RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
    -a 'settings/settings.json' \
    -a 'settings/events.json' \
    -a 'settings/mqtt.json'
