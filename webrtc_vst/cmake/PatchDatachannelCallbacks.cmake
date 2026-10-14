# libdatachannel 0.22.1 hides the base assignment operators in its stored
# callback wrapper. Assigning nullptr/std::function then creates a temporary
# wrapper and copies its optional stored value without holding the callback
# mutex. Explicitly route these assignments through the synchronized base.
set(callback_header "${libdatachannel_SOURCE_DIR}/include/rtc/utils.hpp")
file(READ "${callback_header}" callback_source)
string(REPLACE "\r\n" "\n" callback_source "${callback_source}")
set(old_declaration "class synchronized_stored_callback final : public synchronized_callback<Args...> {\npublic:\n")
set(new_declaration "${old_declaration}	// WebRTC VST: preserve synchronization when assigning/resetting callbacks.\n	template <typename F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, synchronized_stored_callback>, int> = 0>\n	synchronized_stored_callback &operator=(F &&func) {\n		synchronized_callback<Args...>::operator=(std::function<void(Args...)>(std::forward<F>(func)));\n		return *this;\n	}\n")
string(FIND "${callback_source}" "${new_declaration}" patched_position)
if(patched_position EQUAL -1)
    string(FIND "${callback_source}" "${old_declaration}" original_position)
    if(original_position EQUAL -1)
        message(FATAL_ERROR "libdatachannel stored callback changed; review the synchronized-assignment patch")
    endif()
    string(REPLACE "${old_declaration}" "${new_declaration}" callback_source "${callback_source}")
    string(REPLACE "#include <tuple>" "#include <tuple>\n#include <type_traits>" callback_source "${callback_source}")
    file(WRITE "${callback_header}" "${callback_source}")
endif()
