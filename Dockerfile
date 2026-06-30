FROM ubuntu:22.04 AS base

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

# Install required packages and dependencies
RUN apt-get update && apt-get install -y software-properties-common \
	&& add-apt-repository -y ppa:deadsnakes/ppa \
	&& apt-get update && apt-get install -y \
	cmake \
	ccache \
	expect \
	git \
	ninja-build \
	pkg-config \
	libncurses5 \
	python2.7 \
	libpython2.7 \
	python3.8 \
	libpython3.8 \
	unzip \
	zip \
	aria2 \
	gettext \
	rpm2cpio \
	cpio \
	libkf5itemmodels5 \
	libkf5kiowidgets5 \
	libkchart2 \
	&& rm -rf /var/lib/apt/lists/*

# Some of the Samsung Tizen scripts refer to `python`, but Ubuntu only provides `/usr/bin/python2`
RUN ln -sf /usr/bin/python2 /usr/bin/python
RUN ln -sf /usr/bin/python2.7 /usr/bin/python

# Create a non-root user and set up the working directory
RUN useradd -m -s /bin/bash moonlight
USER moonlight
WORKDIR /home/moonlight

# Install Tizen Studio CLI and configure the toolchain path
RUN aria2c -x 5 -s 5 -o web-cli_Tizen_Studio_6.1_ubuntu-64.bin \
'https://download.tizen.org/sdk/Installer/tizen-studio_6.1/web-cli_Tizen_Studio_6.1_ubuntu-64.bin'
RUN chmod a+x web-cli_Tizen_Studio_6.1_ubuntu-64.bin
RUN ./web-cli_Tizen_Studio_6.1_ubuntu-64.bin --accept-license /home/moonlight/tizen-studio
ENV PATH=/home/moonlight/tizen-studio/tools/ide/bin:/home/moonlight/tizen-studio/tools:${PATH}

# Backup the minimal tizen-studio before installing heavy Native Development toolchains
RUN cp -a /home/moonlight/tizen-studio /home/moonlight/tizen-studio-minimal
RUN /home/moonlight/tizen-studio/package-manager/package-manager-cli.bin install \
    NativeToolchain-Gcc-9.2 \
    NativeCLI \
    MOBILE-6.0-NativeAppDevelopment \
    --accept-license

# Prepare the Tizen certificate and security profiles for signing the application package
RUN tizen certificate \
	-a Moonlight \
	-f Moonlight \
	-p 123456
RUN tizen security-profiles add \
	-n Moonlight \
	-a /home/moonlight/tizen-studio-data/keystore/author/Moonlight.p12 \
	-p 123456

# Workaround to package applications without gnome-keyring
# These steps must be repeated each time before packaging an application
# See: <https://developer.tizen.org/forums/sdk-ide/pwd-fle-format-profile.xml-certificates> for more details
RUN sed -i 's|/home/moonlight/tizen-studio-data/keystore/author/Moonlight.pwd|123456|' /home/moonlight/tizen-studio-data/profile/profiles.xml
RUN sed -i 's|/home/moonlight/tizen-studio-data/tools/certificate-generator/certificates/distributor/tizen-distributor-signer.pwd|tizenpkcs12passfordsigner|' /home/moonlight/tizen-studio-data/profile/profiles.xml

# Install Samsung Emscripten SDK and configure Java path for closure compiler
RUN aria2c -x 5 -s 5 -o emscripten-1.39.4.7-linux64.zip \
'https://developer.samsung.com/smarttv/file/a5013a65-af11-4b59-844f-2d34f14d19a9'
RUN unzip emscripten-1.39.4.7-linux64.zip

# Replace deprecated OpenSSL download URL in Emscripten SDK ports to prevent build failure caused by invalid upstream path
RUN sed -i 's|https://www.openssl.org/source/old/1.1.1/openssl-|https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1d/openssl-|g' \
/home/moonlight/emscripten-release-bundle/emsdk/fastcomp/emscripten/tools/ports/tizen/ssl.py
RUN sed -i 's|https://www.openssl.org/source/old/1.1.1/openssl-|https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1d/openssl-|g' \
/home/moonlight/emscripten-release-bundle/emsdk/fastcomp/emscripten/tools/ports/tizen/crypto.py

# Activate the Emscripten SDK to set up the environment variables for compiling the application
WORKDIR emscripten-release-bundle/emsdk
RUN ./emsdk activate latest-fastcomp

# Copy only the backend files required for compiling the application
WORKDIR /home/moonlight
COPY --chown=moonlight CMakeLists.txt ./moonlight-tizen/
COPY --chown=moonlight h264bitstream ./moonlight-tizen/h264bitstream/
COPY --chown=moonlight libgamestream ./moonlight-tizen/libgamestream/
COPY --chown=moonlight moonlight-common-c ./moonlight-tizen/moonlight-common-c/
COPY --chown=moonlight opus ./moonlight-tizen/opus/
COPY --chown=moonlight ports ./moonlight-tizen/ports/ 
COPY --chown=moonlight wasm/*.c ./moonlight-tizen/wasm/
COPY --chown=moonlight wasm/*.cpp ./moonlight-tizen/wasm/
COPY --chown=moonlight wasm/*.hpp ./moonlight-tizen/wasm/
COPY --chown=moonlight wasm/dispatcher ./moonlight-tizen/wasm/dispatcher/
COPY --chown=moonlight native ./moonlight-tizen/native/

RUN cmake \
	-DCMAKE_TOOLCHAIN_FILE=/home/moonlight/emscripten-release-bundle/emsdk/fastcomp/emscripten/cmake/Modules/Platform/Emscripten.cmake \
	-DCMAKE_C_COMPILER_LAUNCHER=ccache \
	-DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
	-G Ninja \
	-S moonlight-tizen \
	-B build
RUN --mount=type=cache,target=/home/moonlight/.ccache,uid=1000,gid=1000 \
	--mount=type=cache,target=/home/moonlight/.emscripten_cache,uid=1000,gid=1000 \
	--mount=type=cache,target=/home/moonlight/.emscripten_ports,uid=1000,gid=1000 \
	CCACHE_DIR=/home/moonlight/.ccache cmake --build build

# Copy the remaining frontend files required for packaging the application
COPY --chown=moonlight res/ ./moonlight-tizen/res/
COPY --chown=moonlight wasm/index.html ./moonlight-tizen/wasm/
COPY --chown=moonlight wasm/platform/ ./moonlight-tizen/wasm/platform/
COPY --chown=moonlight wasm/static/ ./moonlight-tizen/wasm/static/

RUN cmake --install build --prefix build
RUN cp moonlight-tizen/res/icon.png build/widget/

# Build the Native mDNS Service using Tizen CLI and the provided native rootstrap
RUN tizen create native-project -p mobile-6.0 -t ServiceApp -n MdnsService
RUN rm -f MdnsService/src/*.c && \
    mkdir -p MdnsService/src MdnsService/mdns && \
    cp -r moonlight-tizen/native/src/* MdnsService/src/ && \
    cp -r moonlight-tizen/native/mdns/* MdnsService/mdns/ && \
    cp moonlight-tizen/native/tizen-manifest.xml MdnsService/
RUN tizen build-native -C Release -a arm -c gcc -r mobile-6.0-device.core -- MdnsService

# Create package directory
RUN BIN=$(find MdnsService -type f -executable -iname "*mdnsservice*" | head -1) && \
    if [ -z "$BIN" ]; then echo "Binary not found"; exit 1; fi && \
    mkdir -p MdnsService/package && \
    cp "$BIN" MdnsService/package/mdns_service && \
    chmod +x MdnsService/package/mdns_service && \
    cp MdnsService/tizen-manifest.xml MdnsService/package/

# Sign and package the Native Service into a TPK file using Expect
RUN echo \
	'set timeout -1\n' \
	'spawn tizen package -t tpk -s Moonlight -o MdnsService/package -- MdnsService/package\n' \
	'expect eof\n' \
| expect

# Remove the heavy tizen-studio and move the minimal backup into its place
RUN rm -rf /home/moonlight/tizen-studio && \
    mv /home/moonlight/tizen-studio-minimal /home/moonlight/tizen-studio

# Embed the Native Service TPK into the WebApp WGT for Hybrid Packaging
RUN mkdir -p build/widget/tpk && \
    cp MdnsService/package/*.tpk build/widget/tpk/

# Sign and package the application into a WGT file using Expect to automate the interactive password prompts
RUN sed -i 's|123456||' /home/moonlight/tizen-studio-data/profile/profiles.xml
RUN echo \
	'set timeout -1\n' \
	'spawn tizen package -t wgt -s Moonlight -- build/widget\n' \
	'expect "Author password:"\n' \
	'send -- "123456\\r"\n' \
	'expect "Yes: (Y), No: (N) ?"\n' \
	'send -- "N\\r"\n' \
	'expect eof\n' \
| expect
RUN mv build/widget/Moonlight.wgt .

# Clean up unnecessary files to reduce image size
RUN rm -rf \
	build \
	moonlight-tizen \
	MdnsService \
	web-cli_Tizen_Studio_6.1_ubuntu-64.bin \
	tizen-package-expect.sh \
	.package-manager \
	emscripten-1.39.4.7-linux64.zip \
	emscripten-release-bundle \
	.emscripten \
	.emscripten_cache \
	.emscripten_cache.lock \
	.emscripten_ports \
	.emscripten_sanity

# Use a multi-stage build to reclaim space from deleted files
FROM ubuntu:22.04
COPY --from=base / /
USER moonlight
WORKDIR /home/moonlight

# Add Tizen Studio tools to PATH environment variable
ENV PATH=/home/moonlight/tizen-studio/tools/ide/bin:/home/moonlight/tizen-studio/tools:${PATH}
