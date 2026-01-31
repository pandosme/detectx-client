ARG CHIP=aarch64
ARG VERSION=12.8.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

#-------------------------------------------------------------------------------
# Stage 1: Build libjpeg-turbo for target architecture
#-------------------------------------------------------------------------------
FROM ${REPO}/${SDK}:${VERSION}-${CHIP}-ubuntu${UBUNTU_VERSION} AS libjpeg-builder

# Install build dependencies
RUN apt-get update && apt-get install -y cmake git && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Clone and build libjpeg-turbo
ARG CHIP
RUN git clone --depth 1 --branch 3.0.1 https://github.com/libjpeg-turbo/libjpeg-turbo.git && \
    cd libjpeg-turbo && \
    . /opt/axis/acapsdk/environment-setup* && \
    mkdir build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/libjpeg \
        -DENABLE_SHARED=TRUE \
        -DENABLE_STATIC=FALSE && \
    make -j$(nproc) && \
    make install

#-------------------------------------------------------------------------------
# Stage 2: Build ACAP application
#-------------------------------------------------------------------------------
FROM ${REPO}/${SDK}:${VERSION}-${CHIP}-ubuntu${UBUNTU_VERSION}

WORKDIR /opt/app

# Copy application source
COPY ./app .

# Copy libjpeg libraries from builder stage
COPY --from=libjpeg-builder /opt/libjpeg/lib/ ./lib/

# Create symlinks for linker
RUN cd lib && \
    for f in *.so.*; do \
        [ -f "$f" ] && ln -sf "$f" "${f%%.*}.so" || true; \
    done

# Build and package ACAP application
RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
    -a 'settings/settings.json' \
    -a 'settings/events.json' \
    -a 'settings/mqtt.json'
