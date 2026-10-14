# Copy only the module to reproduce a lost Resources folder / single-file install.
file(MAKE_DIRECTORY "${TEST_DIR}")
file(COPY_FILE "${PLUGIN_BINARY}" "${TEST_DIR}/webrtc_vst.vst3" ONLY_IF_DIFFERENT)
execute_process(COMMAND "${TEST_HOST}" "${TEST_DIR}/webrtc_vst.vst3"
    RESULT_VARIABLE result TIMEOUT 55)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Windows editor without external resources failed: ${result}")
endif()
