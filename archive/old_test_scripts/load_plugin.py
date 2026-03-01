import ctypes
path = r"C:\\Users\\steve\\AppData\\Local\\Programs\\Common\\VST3\\webrtc_vst.vst3\\Contents\\x86_64-win\\webrtc_vst.vst3"
try:
    ctypes.WinDLL(path)
    print('Loaded OK')
except OSError as e:
    print('Load failed:', e)
