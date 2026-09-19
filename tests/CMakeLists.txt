# WebRTC VST Plugin - Test Suite

cmake_minimum_required(VERSION 3.24)

add_executable(webrtc_vst_settings_test settings_unit_test.cpp)
target_include_directories(webrtc_vst_settings_test PRIVATE ${vst3sdk_SOURCE_DIR})
target_link_libraries(webrtc_vst_settings_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
target_compile_features(webrtc_vst_settings_test PRIVATE cxx_std_20)
add_test(NAME SettingsUnitTest COMMAND webrtc_vst_settings_test)
set_tests_properties(SettingsUnitTest PROPERTIES TIMEOUT 30 LABELS "unit;settings")

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
    foreach(tgt webrtc_vst_settings_test webrtc_vst_integration_test webrtc_vst_stress_test webrtc_vst_fuzz_test)
        set_property(TARGET ${tgt} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()

# CTest integration
get_target_property(_plugin_bundle webrtc_vst SMTG_PLUGIN_PACKAGE_PATH)

foreach(tgt webrtc_vst_integration_test webrtc_vst_stress_test webrtc_vst_fuzz_test)
    add_dependencies(${tgt} webrtc_vst)
    target_compile_definitions(${tgt} PRIVATE WEBRTC_VST_DEFAULT_PLUGIN_PATH="${_plugin_bundle}")
    if(APPLE)
        target_link_libraries(${tgt} PRIVATE "-framework Cocoa")
        target_compile_options(${tgt} PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    endif()
endforeach()

add_test(NAME IntegrationTest
    COMMAND webrtc_vst_integration_test
        "${_plugin_bundle}"
)

add_test(NAME StressTest
    COMMAND webrtc_vst_stress_test
        "${_plugin_bundle}"
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
        "${_plugin_bundle}"
)

set_tests_properties(FuzzTest PROPERTIES
    TIMEOUT 300
    LABELS "fuzz"
)

if(APPLE)
    add_executable(webrtc_vst_mac_editor_test mac_editor_smoke.mm
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp)
    target_include_directories(webrtc_vst_mac_editor_test PRIVATE ${vst3sdk_SOURCE_DIR})
    target_link_libraries(webrtc_vst_mac_editor_test PRIVATE sdk sdk_common sdk_hosting "-framework Cocoa" "-framework Vision")
    target_compile_options(webrtc_vst_mac_editor_test PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_compile_definitions(webrtc_vst_mac_editor_test PRIVATE WEBRTC_VST_DEFAULT_PLUGIN_PATH="${_plugin_bundle}")
    add_dependencies(webrtc_vst_mac_editor_test webrtc_vst)
    add_test(NAME MacEditorSmoke COMMAND webrtc_vst_mac_editor_test "${_plugin_bundle}")
    set_tests_properties(MacEditorSmoke PROPERTIES TIMEOUT 30 LABELS "macos;ui")
endif()
