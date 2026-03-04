# WebRTC VST Plugin - Test Suite

cmake_minimum_required(VERSION 3.24)

# Integration Tests
add_executable(webrtc_vst_integration_test
    integration_test.cpp
)

target_include_directories(webrtc_vst_integration_test
    PRIVATE
        ${vst3sdk_SOURCE_DIR}
        ${vst3sdk_SOURCE_DIR}/pluginterfaces
        ${vst3sdk_SOURCE_DIR}/public.sdk/source
)

target_link_libraries(webrtc_vst_integration_test
    PRIVATE
        sdk
        sdk_common
        sdk_hosting
)

target_compile_features(webrtc_vst_integration_test PRIVATE cxx_std_20)

# Add VST3 hosting sources
target_sources(webrtc_vst_integration_test
    PRIVATE
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
)

if(WIN32)
    target_sources(webrtc_vst_integration_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
    )
elseif(APPLE)
    target_sources(webrtc_vst_integration_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm
    )
elseif(UNIX)
    target_sources(webrtc_vst_integration_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
    )
endif()

# Stress Tests
add_executable(webrtc_vst_stress_test
    stress_test.cpp
)

target_include_directories(webrtc_vst_stress_test
    PRIVATE
        ${vst3sdk_SOURCE_DIR}
        ${vst3sdk_SOURCE_DIR}/pluginterfaces
        ${vst3sdk_SOURCE_DIR}/public.sdk/source
)

target_link_libraries(webrtc_vst_stress_test
    PRIVATE
        sdk
        sdk_common
        sdk_hosting
)

target_compile_features(webrtc_vst_stress_test PRIVATE cxx_std_20)

# Add VST3 hosting sources
target_sources(webrtc_vst_stress_test
    PRIVATE
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
)

if(WIN32)
    target_sources(webrtc_vst_stress_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
    )
elseif(APPLE)
    target_sources(webrtc_vst_stress_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm
    )
elseif(UNIX)
    target_sources(webrtc_vst_stress_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
    )
endif()

# Fuzz Tests
add_executable(webrtc_vst_fuzz_test
    fuzz_test.cpp
)

target_include_directories(webrtc_vst_fuzz_test
    PRIVATE
        ${vst3sdk_SOURCE_DIR}
        ${vst3sdk_SOURCE_DIR}/pluginterfaces
        ${vst3sdk_SOURCE_DIR}/public.sdk/source
)

target_link_libraries(webrtc_vst_fuzz_test
    PRIVATE
        sdk
        sdk_common
        sdk_hosting
)

target_compile_features(webrtc_vst_fuzz_test PRIVATE cxx_std_20)

target_sources(webrtc_vst_fuzz_test
    PRIVATE
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
)

if(WIN32)
    target_sources(webrtc_vst_fuzz_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
    )
elseif(APPLE)
    target_sources(webrtc_vst_fuzz_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm
    )
elseif(UNIX)
    target_sources(webrtc_vst_fuzz_test
        PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
    )
endif()

# Set MSVC runtime to match plugin
if(MSVC)
    foreach(tgt webrtc_vst_integration_test webrtc_vst_stress_test webrtc_vst_fuzz_test)
        set_property(TARGET ${tgt} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()

# CTest integration
enable_testing()

add_test(NAME IntegrationTest
    COMMAND webrtc_vst_integration_test
        $<TARGET_FILE_DIR:webrtc_vst>/../webrtc_vst.vst3
)

add_test(NAME StressTest
    COMMAND webrtc_vst_stress_test
        $<TARGET_FILE_DIR:webrtc_vst>/../webrtc_vst.vst3
)

# Set test properties
set_tests_properties(IntegrationTest PROPERTIES
    TIMEOUT 120
    LABELS "integration"
)

set_tests_properties(StressTest PROPERTIES
    TIMEOUT 300
    LABELS "stress"
)

add_test(NAME FuzzTest
    COMMAND webrtc_vst_fuzz_test
        $<TARGET_FILE_DIR:webrtc_vst>/../webrtc_vst.vst3
)

set_tests_properties(FuzzTest PROPERTIES
    TIMEOUT 300
    LABELS "fuzz"
)
