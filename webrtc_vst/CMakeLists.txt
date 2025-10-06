cmake_minimum_required(VERSION 3.24)
project(WebRTC_VST VERSION 0.1.0 LANGUAGES CXX)

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

if(NOT EXISTS "${_VST3_MODULE_FILE}")
    message(FATAL_ERROR "Could not locate SMTG_VST3_SDK.cmake under ${VST3_SDK_ROOT}; ensure you cloned the VST3 SDK with submodules (git submodule update --init --recursive) and that VST3_SDK_ROOT points to the SDK root")
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
set(USE_ZLIB OFF CACHE BOOL "Disable zlib dependency in IXWebSocket" FORCE)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json libdatachannel ixwebsocket opus)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

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
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
)

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







