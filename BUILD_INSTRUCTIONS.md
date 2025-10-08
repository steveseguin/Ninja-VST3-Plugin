# Build Instructions - WebRTC VST Plugin

## ⚠️ Important: Build from PowerShell (Windows)

This plugin uses Visual Studio and must be built from PowerShell, not WSL.

---

## Build from PowerShell

```powershell
# Navigate to project directory
cd C:\Users\steve\Code\gpt\vst

# Build the plugin
cmake --build build/webrtc_vst_win --config Release --target webrtc_vst
```

---

## Full Rebuild (if needed)

```powershell
# Clean previous build
Remove-Item -Recurse -Force build/webrtc_vst_win -ErrorAction SilentlyContinue

# Reconfigure
cmake -B build/webrtc_vst_win -S . -G "Visual Studio 17 2022" `
      -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build/webrtc_vst_win --config Release --target webrtc_vst
```

---

## Deploy to Audacity

```powershell
# Copy plugin to VST3 directory
Copy-Item -Recurse -Force build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3 `
  C:\Users\steve\AppData\Local\Programs\Common\VST3\
```

---

## Verify Compilation

After building, check for errors in the output:
- ✅ Look for: `Build succeeded`
- ❌ Watch for: Compilation errors, linker errors

Common issues:
- **Missing dependencies**: Run `.\tools\force_static_runtime.ps1` first
- **Path issues**: Ensure using Windows paths (not WSL paths)
- **Generator error**: Make sure Visual Studio 2022 is installed

---

## Build Output Location

Compiled plugin will be at:
```
build/webrtc_vst_win/VST3/Release/webrtc_vst.vst3/
```

---

## Next Steps After Build

1. Deploy to VST3 directory (see command above)
2. Launch Audacity
3. Add VST3 effect from menu
4. Test plugin functionality
5. Check for crashes during:
   - Open/close cycles
   - Active streaming
   - Network disconnection

---

## Troubleshooting

### "CMakeCache.txt directory is different"
**Solution**: Delete build directory and reconfigure
```powershell
Remove-Item -Recurse -Force build/webrtc_vst_win
# Then run full rebuild steps above
```

### "Could not create CMAKE_GENERATOR"
**Solution**: Install Visual Studio 2022 with C++ workload

### Build succeeds but plugin doesn't load
**Solution**: Check dependencies are statically linked
```powershell
.\tools\force_static_runtime.ps1
# Then rebuild
```

---

## Build from WSL (Not Supported)

WSL cannot build Visual Studio projects directly. You must use PowerShell on Windows.

If you see this error in WSL:
```
Error: could not create CMAKE_GENERATOR "Visual Studio 17 2022"
```

**Solution**: Switch to PowerShell and run build commands there.
