cmake_minimum_required(VERSION 3.24)
project(WebRTC_VST VERSION 0.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)


# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

if(NOT DEFINED VST3_SDK_ROOT)
    message(FATAL_ERROR "VST3_SDK_ROOT must point to the root of the Steinberg VST3 SDK")
endif()

set(VST3_SDK_ROOT "${VST3_SDK_ROOT}" CACHE PATH "Path to the VST3 SDK" FORCE)
set(_VST3_MODULE_FILE "${VST3_SDK_ROOT}/cmake/modules/SMTG_VST3_SDK.cmake")
set(_VST3_LICENSE_FILE "${VST3_SDK_ROOT}/LICENSE.txt")

if(NOT EXISTS "${_VST3_MODULE_FILE}")
    message(FATAL_ERROR "Could not locate SMTG_VST3_SDK.cmake under ${VST3_SDK_ROOT}; ensure you cloned the VST3 SDK with submodules (git submodule update --init --recursive) and that VST3_SDK_ROOT points to the SDK root")
endif()

if(NOT EXISTS "${_VST3_LICENSE_FILE}")
    message(FATAL_ERROR "Could not locate ${_VST3_LICENSE_FILE}; use Steinberg VST3 SDK 3.8+ with complete source checkout")
endif()

# Enforce VST3 SDK 3.8+ MIT licensing to avoid older GPL/proprietary dual-license variants.
file(READ "${_VST3_LICENSE_FILE}" _vst3_license_text)
string(FIND "${_vst3_license_text}" "MIT License" _vst3_mit_license_pos)
if(_vst3_mit_license_pos EQUAL -1)
    message(FATAL_ERROR "Detected non-MIT VST3 SDK license in ${_VST3_LICENSE_FILE}. Use VST3 SDK 3.8+ (MIT).")
endif()

if(NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug/Release/RelWithDebInfo/MinSizeRel)" FORCE)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        add_compile_definitions(NDEBUG)
    else()
        add_compile_definitions(_DEBUG)
    endif()
else()
    add_compile_definitions(
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<CONFIG:RelWithDebInfo>:NDEBUG>
        $<$<CONFIG:Release>:NDEBUG>
        $<$<CONFIG:MinSizeRel>:NDEBUG>
    )
endif()

set(vst3sdk_SOURCE_DIR "${VST3_SDK_ROOT}" CACHE PATH "Path to the VST3 SDK" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "Disable VST3 example projects" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "Disable VST3 hosting examples" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "Disable VSTGUI support" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "Skip running the VST3 validator during build" FORCE)

add_subdirectory(${vst3sdk_SOURCE_DIR} ${CMAKE_BINARY_DIR}/vst3sdk)
smtg_enable_vst3_sdk()

# OpenSSL is required by both this project and libdatachannel's libsrtp dependency.
# Resolve it before FetchContent to avoid nested dependency configure failures.
if(MSVC)
    if(NOT DEFINED OPENSSL_ROOT_DIR OR OPENSSL_ROOT_DIR STREQUAL "")
        set(_openssl_candidates "")
        if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
            list(APPEND _openssl_candidates "$ENV{VCPKG_ROOT}/installed/x64-windows-static")
        endif()
        list(APPEND _openssl_candidates
            "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/vcpkg/installed/x64-windows-static"
            "${CMAKE_CURRENT_SOURCE_DIR}/../deps/vcpkg/installed/x64-windows-static"
        )

        foreach(_cand IN LISTS _openssl_candidates)
            if(EXISTS "${_cand}/include/openssl/ssl.h" AND EXISTS "${_cand}/lib/libcrypto.lib")
                set(OPENSSL_ROOT_DIR "${_cand}" CACHE PATH "Path to OpenSSL root" FORCE)
                break()
            endif()
        endforeach()
    endif()
    set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "Prefer static OpenSSL" FORCE)
endif()

find_package(OpenSSL REQUIRED)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
)

# libdatachannel (WebRTC implementation)
FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.22.1
)

# ixwebsocket for signaling transport
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG master
)

FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
)

set(LIBDATACHANNEL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDATACHANNEL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LIBDATACHANNEL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBDATACHANNEL_USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(LIBDATACHANNEL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(LIBDATACHANNEL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(LIBDATACHANNEL_BUILD_JS_API OFF CACHE BOOL "" FORCE)
set(LIBDATACHANNEL_USE_ASIO OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "Disable libdatachannel examples" FORCE)
set(NO_TESTS ON CACHE BOOL "Disable libdatachannel tests" FORCE)
set(USE_TLS ON CACHE BOOL "Enable TLS support in IXWebSocket" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "Enable OpenSSL backend in IXWebSocket" FORCE)
set(USE_MBED_TLS OFF CACHE BOOL "Disable mbedTLS backend in IXWebSocket" FORCE)
set(USE_ZLIB OFF CACHE BOOL "Disable zlib dependency in IXWebSocket" FORCE)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json libdatachannel ixwebsocket opus)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------

set(PLUGIN_NAME "webrtc_vst")

set(PLUGIN_SOURCES
    src/PluginProcessor.cpp
    src/WebRTCSession.cpp
    src/VDONinjaSignalingClient.cpp
    src/AudioRingBuffer.cpp
    src/PluginController.cpp
    src/ParameterStringRegistry.cpp
    src/qrcodegen.cpp
    src/plugin_entry.cpp
    resources/webrtc_vst.uidesc
)

set(PLUGIN_HEADERS
    src/PluginProcessor.h
    src/WebRTCSession.h
    src/VDONinjaSignalingClient.h
    src/AudioRingBuffer.h
    src/PluginConfig.h
    src/PluginController.h
    src/StreamIdGenerator.h
    src/ParameterIDs.h
    src/ParameterStringRegistry.h
    src/qrcodegen.hpp
    src/Version.h
)

smtg_add_vst3plugin(${PLUGIN_NAME} ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_include_directories(${PLUGIN_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${vst3sdk_SOURCE_DIR}
        ${vst3sdk_SOURCE_DIR}/pluginterfaces
        ${vst3sdk_SOURCE_DIR}/public.sdk/source
        ${vst3sdk_SOURCE_DIR}/base/source
        ${libdatachannel_SOURCE_DIR}/include
)

target_link_libraries(${PLUGIN_NAME}
    PRIVATE
        sdk
        vstgui_support
        LibDataChannel::LibDataChannel
        ixwebsocket
        nlohmann_json::nlohmann_json
        opus
        Threads::Threads
        OpenSSL::Crypto
)

smtg_target_add_plugin_resources(${PLUGIN_NAME}
    RESOURCES
        resources/webrtc_vst.uidesc
)

if(MSVC)
    target_link_options(${PLUGIN_NAME}
        PRIVATE
            "$<$<CONFIG:Release>:/MAP:${CMAKE_CURRENT_BINARY_DIR}/webrtc_vst_release.map>"
            "$<$<CONFIG:RelWithDebInfo>:/MAP:${CMAKE_CURRENT_BINARY_DIR}/webrtc_vst_relwithdebinfo.map>"
    )
endif()


if(SMTG_MAC)
    smtg_target_set_bundle(${PLUGIN_NAME}
        INFOPLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist
    )
endif()

install(TARGETS ${PLUGIN_NAME}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION lib
)


add_executable(webrtc_vst_cli_host
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/cli_host/main.cpp
)

set_target_properties(webrtc_vst_cli_host PROPERTIES
    OUTPUT_NAME webrtc_vst_cli_host
)

target_include_directories(webrtc_vst_cli_host
    PRIVATE
        ${vst3sdk_SOURCE_DIR}
        ${vst3sdk_SOURCE_DIR}/pluginterfaces
        ${vst3sdk_SOURCE_DIR}/public.sdk/source
)

target_link_libraries(webrtc_vst_cli_host
    PRIVATE
        sdk
        sdk_common
        sdk_hosting
)


target_compile_features(webrtc_vst_cli_host PRIVATE cxx_std_20)

target_sources(webrtc_vst_cli_host
    PRIVATE
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
)

if(WIN32)
    target_sources(webrtc_vst_cli_host PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
elseif(APPLE)
    target_sources(webrtc_vst_cli_host PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
else()
    target_sources(webrtc_vst_cli_host PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()

if(MSVC)
    foreach(tgt ${PLUGIN_NAME} webrtc_vst_cli_host LibDataChannel::LibDataChannel ixwebsocket opus)
        if(TARGET ${tgt})
            get_target_property(_aliased ${tgt} ALIASED_TARGET)
            if(_aliased)
                set_property(TARGET ${_aliased} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            else()
                set_property(TARGET ${tgt} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            endif()
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Test Suite
# ---------------------------------------------------------------------------

option(WEBRTC_VST_BUILD_TESTS "Build test suite" ON)

if(WEBRTC_VST_BUILD_TESTS)
    message(STATUS "Building WebRTC VST test suite")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()



