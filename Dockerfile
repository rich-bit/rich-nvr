# ===== Stage 1: build LIVE555 =====
FROM ubuntu:24.04 AS live555
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates wget pkg-config libssl-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
RUN wget http://www.live555.com/liveMedia/public/live555-latest.tar.gz \
 && tar -xzf live555-latest.tar.gz \
 && cd live \
 && ./genMakefiles linux \
 && make -j"$(nproc)" CXXFLAGS="-std=gnu++20"

SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN mkdir -p /opt/live555/include/{liveMedia,groupsock,UsageEnvironment,BasicUsageEnvironment} /opt/live555/lib \
 && cp -r /tmp/live/liveMedia/include/*             /opt/live555/include/liveMedia/ \
 && cp -r /tmp/live/groupsock/include/*             /opt/live555/include/groupsock/ \
 && cp -r /tmp/live/UsageEnvironment/include/*      /opt/live555/include/UsageEnvironment/ \
 && cp -r /tmp/live/BasicUsageEnvironment/include/* /opt/live555/include/BasicUsageEnvironment/ \
 && cp /tmp/live/liveMedia/libliveMedia.a                   /opt/live555/lib/ \
 && cp /tmp/live/groupsock/libgroupsock.a                   /opt/live555/lib/ \
 && cp /tmp/live/UsageEnvironment/libUsageEnvironment.a     /opt/live555/lib/ \
 && cp /tmp/live/BasicUsageEnvironment/libBasicUsageEnvironment.a /opt/live555/lib/

# ===== Stage 2: build your app (devcontainer uses this stage) =====
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
ARG BUILD_TYPE=Release
ENV BUILD_TYPE=${BUILD_TYPE}

# Toolchain, Qt dev, GStreamer dev + plugins, OpenCV dev, ffmpeg, JSON and HTTP libs
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git pkg-config wget \
      qt6-base-dev qt6-wayland libwayland-client0 libxkbcommon0 libgl1 libegl1 libgbm1 \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstrtspserver-1.0-dev \
      gstreamer1.0-tools \
      gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
      gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
      gstreamer1.0-libav \
      libopencv-dev libssl-dev \
      nlohmann-json3-dev libcpp-httplib-dev \
      ffmpeg \
  && rm -rf /var/lib/apt/lists/*

COPY --from=live555 /opt/live555 /opt/live555
WORKDIR /app
COPY . .

RUN set -eux; \
    QT_ARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"; \
    QT6_PREFIX="/usr/lib/${QT_ARCH}/cmake/Qt6"; \
    mkdir -p build && cd build; \
    cmake .. \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DLIVE555_DIR=/opt/live555 \
      -DCMAKE_PREFIX_PATH="${QT6_PREFIX}" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; \
    cmake --build . -j"$(nproc)"; \
    mkdir -p /app/dist/server /app/dist/client; \
    if [ -d ../dist ]; then cp -a ../dist/* /app/dist/ || true; fi; \
    [ -x server/richserver ] && install -Dm755 server/richserver /app/dist/server/richserver || true; \
    [ -x client/richclient ] && install -Dm755 client/richclient /app/dist/client/richclient || true; \
    cp -f compile_commands.json /app/dist/ 2>/dev/null || true; \
    ls -l /app/dist /app/dist/server /app/dist/client || true

# ===== Stage 3: runtime (lean image for running) =====
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
ARG INSTALL_DEBUG_TOOLS=0
ENV INSTALL_DEBUG_TOOLS=${INSTALL_DEBUG_TOOLS}

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libssl3 \
      # Qt + GL stack (for client when you run it in the container)
      libqt6core6 libqt6gui6 libqt6widgets6 libqt6network6 \
      qt6-wayland libwayland-client0 libxkbcommon0 libgl1 libegl1 libgbm1 \
      # GStreamer runtime + plugins (match build stage)
      libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libgstrtspserver-1.0-0 \
      gstreamer1.0-tools \
      gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
      gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
      gstreamer1.0-libav \
      # OpenCV runtime (you currently use dev meta; keep it simple)
      libopencv-dev \
      # ffmpeg CLI for VideoExporter / debugging
      ffmpeg \
  && if [ "$INSTALL_DEBUG_TOOLS" = "1" ]; then \
       apt-get install -y --no-install-recommends gdb gdbserver procps strace lsof; \
     fi \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /app/dist /app/dist
EXPOSE 8554 8080
CMD ["/bin/bash"]
