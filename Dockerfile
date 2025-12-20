# syntax=docker/dockerfile:1.6

# ===== Stage 1: build LIVE555 =====
FROM ubuntu:24.04 AS live555
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates wget pkg-config libssl-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
COPY cache/live555-latest.tar.gz* ./
RUN set -eux; \
  if [ ! -f live555-latest.tar.gz ]; then \
    wget http://www.live555.com/liveMedia/public/live555-latest.tar.gz -O live555-latest.tar.gz; \
  fi; \
  tar -xzf live555-latest.tar.gz; \
  cd live; \
  ./genMakefiles linux; \
  make -j"$(nproc)" CXXFLAGS="-std=gnu++20"

SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN mkdir -p /opt/live555/{liveMedia,groupsock,UsageEnvironment,BasicUsageEnvironment} \
 && cp -r /tmp/live/liveMedia/include            /opt/live555/liveMedia/ \
 && cp    /tmp/live/liveMedia/libliveMedia.a     /opt/live555/liveMedia/ \
 && cp -r /tmp/live/groupsock/include            /opt/live555/groupsock/ \
 && cp    /tmp/live/groupsock/libgroupsock.a     /opt/live555/groupsock/ \
 && cp -r /tmp/live/UsageEnvironment/include     /opt/live555/UsageEnvironment/ \
 && cp    /tmp/live/UsageEnvironment/libUsageEnvironment.a /opt/live555/UsageEnvironment/ \
 && cp -r /tmp/live/BasicUsageEnvironment/include /opt/live555/BasicUsageEnvironment/ \
 && cp    /tmp/live/BasicUsageEnvironment/libBasicUsageEnvironment.a /opt/live555/BasicUsageEnvironment/

# ===== Stage 2: build your app (devcontainer uses this stage) =====
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
ARG BUILD_TYPE=Release
ENV BUILD_TYPE=${BUILD_TYPE}

# Toolchain, GStreamer dev + plugins, OpenCV dev, ffmpeg, SDL2, JSON and HTTP libs
# Note: Qt6Core still needed by VideoExporter (TODO: replace with std:: alternatives)
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git pkg-config wget \
      libssl-dev \
      nlohmann-json3-dev libcpp-httplib-dev \
      libopencv-dev \
      qt6-base-dev qt6-base-private-dev \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstrtspserver-1.0-dev \
      gstreamer1.0-tools \
      gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
      gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
      gstreamer1.0-libav \
      libsdl2-dev \
      libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
      ffmpeg \
  && rm -rf /var/lib/apt/lists/*

COPY --from=live555 /opt/live555 /opt/live555
WORKDIR /app
COPY . .

RUN set -eux; \
  mkdir -p build && cd build; \
  cmake .. \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_PREFIX_PATH=/usr \
    -DLIVE555_DIR=/opt/live555 \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; \
  cmake --build . -j"$(nproc)"; \
  mkdir -p /app/dist/server /app/dist/client; \
  if [ -d ../dist ]; then cp -a ../dist/* /app/dist/ || true; fi; \
  [ -x server/nvrserver ] && install -Dm755 server/nvrserver /app/dist/server/nvrserver || true; \
  [ -x client/nvrclient ] && install -Dm755 client/nvrclient /app/dist/client/nvrclient || true; \
  cp -f compile_commands.json /app/dist/ 2>/dev/null || true; \
  ls -l /app/dist /app/dist/server /app/dist/client || true

# ===== Stage 3: runtime (lean image for running) =====
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
ARG INSTALL_DEBUG_TOOLS=0
ENV INSTALL_DEBUG_TOOLS=${INSTALL_DEBUG_TOOLS}

RUN apt-get update && apt-get install -y --no-install-recommends \
  ca-certificates libssl3 \
  libqt6core6 \
  libcpp-httplib0.14 \
  libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libgstrtspserver-1.0-0 \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  libopencv-dev \
  libsdl2-2.0-0 \
  ffmpeg \
  && if [ "$INSTALL_DEBUG_TOOLS" = "1" ]; then \
       apt-get install -y --no-install-recommends gdb gdbserver procps strace lsof; \
     fi \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /app/dist /app/dist
EXPOSE 8554 8080
CMD ["/bin/bash"]

# Build command:
# docker build -t rich-nvr:latest .