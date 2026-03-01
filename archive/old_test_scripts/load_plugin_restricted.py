import ctypes
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000
if not kernel32.SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS):
    err = ctypes.get_last_error()
    print('SetDefaultDllDirectories failed:', err)
path = r"C:\\Program Files\\Common Files\\VST3\\webrtc_vst.vst3\\Contents\\x86_64-win\\webrtc_vst.vst3"
try:
    ctypes.WinDLL(path)
    print('Loaded OK with restricted search path')
except OSError as e:
    print('Load failed with restricted search path:', e)
