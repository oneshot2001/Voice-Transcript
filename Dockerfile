ARG ARCH=aarch64
ARG VERSION=12.2.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

RUN apt-get update && DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --no-install-recommends \
        git cmake curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*

#-------------------------------------------------------------------------------
# whisper.cpp: static CPU-only libs cross-compiled with the ACAP SDK toolchain
#-------------------------------------------------------------------------------

ARG WHISPER_VERSION=v1.7.5
WORKDIR /opt/build
RUN git clone --depth 1 --branch ${WHISPER_VERSION} \
        https://github.com/ggml-org/whisper.cpp.git
RUN . /opt/axis/acapsdk/environment-setup* && \
    cd whisper.cpp && \
    cmake -B build \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DGGML_NATIVE=OFF \
        -DGGML_OPENMP=OFF \
        -DWHISPER_BUILD_EXAMPLES=OFF \
        -DWHISPER_BUILD_TESTS=OFF && \
    cmake --build build -j"$(nproc)"

#-------------------------------------------------------------------------------
# Whisper models bundled into the .eap.
# base: higher accuracy, slower on i.MX8
# tiny: lower accuracy, significantly faster
#-------------------------------------------------------------------------------

ARG MODEL_BASE=ggml-base-q5_1.bin
ARG MODEL_TINY=ggml-tiny-q5_1.bin
RUN mkdir -p /opt/model && \
    curl -fsSL -o /opt/model/${MODEL_BASE} \
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${MODEL_BASE}" && \
    curl -fsSL -o /opt/model/${MODEL_TINY} \
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${MODEL_TINY}"

#-------------------------------------------------------------------------------
# Build ACAP application
#-------------------------------------------------------------------------------

WORKDIR /opt/app
COPY ./app .
RUN mkdir -p whisper/lib whisper/include && \
    cp $(find /opt/build/whisper.cpp/build -name 'lib*.a') whisper/lib/ && \
    cp /opt/build/whisper.cpp/include/whisper.h whisper/include/ && \
    cp /opt/build/whisper.cpp/ggml/include/*.h whisper/include/ && \
    cp /opt/model/${MODEL_BASE} . && \
    cp /opt/model/${MODEL_TINY} .

RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
	-a 'settings/settings.json' \
	-a 'settings/events.json' \
	-a 'settings/mqtt.json' \
    -a ${MODEL_BASE} \
    -a ${MODEL_TINY}
