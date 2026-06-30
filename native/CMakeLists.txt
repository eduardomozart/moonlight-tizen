cmake_minimum_required(VERSION 3.10)
project(MdnsService C CXX)

set(CMAKE_C_STANDARD 99)

# We expect the Tizen SDK headers and libs to be available, or provided by the toolchain.
# For Tizen Native Service, we need dlog, capi-appfw-service-application, capi-appfw-message-port.

find_package(PkgConfig REQUIRED)
pkg_check_modules(TIZEN REQUIRED dlog capi-appfw-service-application capi-appfw-message-port capi-base-common)

include_directories(
    src
    mdns
    ${TIZEN_INCLUDE_DIRS}
)

add_executable(mdns_service
    src/mdns_service.c
)

target_link_libraries(mdns_service
    PRIVATE
    ${TIZEN_LIBRARIES}
    pthread
)
