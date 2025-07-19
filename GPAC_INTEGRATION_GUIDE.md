# GPAC Integration Implementation Guide

## Overview

This document describes how to complete the real GPAC library integration for Tardsplaya, replacing the current stub implementation with actual GPAC API calls.

## Current Implementation Status

✅ **Completed Infrastructure:**
- GPAC player wrapper class (`gpac_player.h/.cpp`)
- GPAC stream thread for data feeding (`gpac_stream_thread.h/.cpp`) 
- UI integration with embedded video windows
- Ad detection and skipping with visual feedback
- Fallback to external players when GPAC unavailable
- Complete project structure and build files

⚠️ **Current Limitations:**
- Uses stub GPAC implementation for compilation
- No actual video rendering (shows placeholder windows)
- Simulated ad detection instead of real stream analysis

## Required GPAC Integration Steps

### 1. Install GPAC Development Libraries

Download GPAC from: https://github.com/gpac/gpac

Build GPAC for Windows:
```bash
# Clone GPAC repository
git clone https://github.com/gpac/gpac.git
cd gpac

# Build for Windows (requires Visual Studio)
# Follow GPAC build instructions for Windows
```

### 2. Update Project Dependencies

Add to `Tardsplaya.vcxproj`:
```xml
<AdditionalIncludeDirectories>$(GPAC_ROOT)\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
<AdditionalLibraryDirectories>$(GPAC_ROOT)\bin\gcc;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
<AdditionalDependencies>libgpac.lib;%(AdditionalDependencies)</AdditionalDependencies>
```

### 3. Replace Stub Implementation in `gpac_player.cpp`

#### Current Stub Code:
```cpp
// Stub GPAC structures for compilation
struct GF_Terminal { void* dummy; };
struct GF_User { void* dummy; };
struct GF_Config { void* dummy; };
```

#### Replace with Real GPAC Headers:
```cpp
#include <gpac/terminal.h>
#include <gpac/modules/service.h>
#include <gpac/options.h>
#include <gpac/setup.h>
```

#### Update InitializeGpac() Method:
```cpp
bool GpacPlayer::InitializeGpac() {
    LogMessage(L"Initializing GPAC library");
    
    // Initialize GPAC system
    gf_sys_init(GF_FALSE);
    
    // Create configuration
    GF_Err e;
    m_config = gf_cfg_init(nullptr, &e);
    if (!m_config || e != GF_OK) {
        gf_sys_close();
        return false;
    }
    
    // Setup user structure
    m_user = (GF_User*)gf_malloc(sizeof(GF_User));
    memset(m_user, 0, sizeof(GF_User));
    m_user->config = m_config;
    m_user->EventProc = OnGpacEvent;
    m_user->opaque = this;
    m_user->os_window_handler = (void*)m_videoWindow;
    
    // Create terminal
    m_terminal = gf_term_new(m_user);
    if (!m_terminal) {
        CleanupGpac();
        return false;
    }
    
    LogMessage(L"GPAC library initialized successfully");
    return true;
}
```

#### Update Play() Method:
```cpp
bool GpacPlayer::Play(const std::wstring& streamUrl) {
    if (!m_initialized.load()) {
        LogMessage(L"GPAC player not initialized");
        return false;
    }
    
    LogMessage(L"Starting playback of: " + streamUrl);
    m_currentUrl = streamUrl;
    
    // Convert wstring to char*
    std::string urlStr(streamUrl.begin(), streamUrl.end());
    
    // Connect to stream
    GF_Err e = gf_term_connect(m_terminal, urlStr.c_str());
    if (e != GF_OK) {
        LogMessage(L"Failed to connect to stream: " + std::to_wstring(e));
        return false;
    }
    
    m_playing = true;
    m_paused = false;
    
    // Show the video window
    if (m_videoWindow) {
        ShowWindow(m_videoWindow, SW_SHOW);
        UpdateWindow(m_videoWindow);
    }
    
    LogMessage(L"Playback started successfully");
    return true;
}
```

#### Update OnGpacEvent() Callback:
```cpp
void GpacPlayer::OnGpacEvent(void* user_data, GF_Event* evt) {
    GpacPlayer* player = static_cast<GpacPlayer*>(user_data);
    if (!player) return;
    
    switch (evt->type) {
        case GF_EVENT_CONNECT:
            if (evt->connect.is_connected) {
                player->LogMessage(L"GPAC: Connected to stream");
            } else {
                player->LogMessage(L"GPAC: Disconnected from stream");
            }
            break;
            
        case GF_EVENT_DURATION:
            player->LogMessage(L"GPAC: Stream duration: " + 
                             std::to_wstring(evt->duration.duration));
            break;
            
        case GF_EVENT_MESSAGE:
            if (evt->message.message) {
                std::string msg(evt->message.message);
                std::wstring wmsg(msg.begin(), msg.end());
                player->LogMessage(L"GPAC: " + wmsg);
            }
            break;
            
        case GF_EVENT_PROGRESS:
            // Handle loading progress
            break;
            
        default:
            break;
    }
}
```

### 4. Update `gpac_stream_thread.cpp`

#### Replace FeedDataToGpac() Method:
```cpp
bool GpacStreamThread::FeedDataToGpac(const std::vector<uint8_t>& data) {
    if (!m_gpacPlayer || data.empty()) {
        return false;
    }
    
    // Get GPAC terminal from player
    GF_Terminal* terminal = m_gpacPlayer->GetTerminal();
    if (!terminal) {
        return false;
    }
    
    // Feed data to GPAC terminal
    GF_Err e = gf_term_process_data(terminal, data.data(), data.size());
    if (e != GF_OK) {
        LogMessage(L"Failed to feed data to GPAC: " + std::to_wstring(e));
        return false;
    }
    
    LogMessage(L"Fed " + std::to_wstring(data.size()) + L" bytes to GPAC");
    return true;
}
```

#### Enhance Ad Detection:
```cpp
bool GpacStreamThread::IsAdSegment(const std::wstring& segmentUrl, const std::vector<uint8_t>& data) {
    // URL-based detection
    if (segmentUrl.find(L"ads") != std::wstring::npos ||
        segmentUrl.find(L"commercial") != std::wstring::npos ||
        segmentUrl.find(L"preroll") != std::wstring::npos ||
        segmentUrl.find(L"midroll") != std::wstring::npos) {
        return true;
    }
    
    // SCTE-35 marker detection in TS data
    if (data.size() > 188) { // Minimum TS packet size
        for (size_t i = 0; i < data.size() - 188; i += 188) {
            if (data[i] == 0x47) { // TS sync byte
                uint16_t pid = ((data[i+1] & 0x1F) << 8) | data[i+2];
                if (pid == 0x1FFF) continue; // Null packet
                
                // Check for SCTE-35 PID or ad markers
                // Implementation depends on specific stream format
            }
        }
    }
    
    return false;
}
```

### 5. Testing the Integration

1. **Build Test:**
   ```bash
   # Ensure project compiles with real GPAC headers
   msbuild Tardsplaya.sln /p:Configuration=Release
   ```

2. **Runtime Test:**
   - Launch Tardsplaya.exe
   - Load a Twitch channel
   - Click "Watch" to start GPAC playback
   - Verify video appears in embedded window
   - Test ad detection and "Skipping ads" message

3. **Fallback Test:**
   - Temporarily disable GPAC (set `g_useGpacPlayer = false`)
   - Verify external player fallback still works
   - Test both modes for compatibility

### 6. Performance Optimization

#### GPAC Configuration:
```cpp
// In InitializeGpac(), optimize for streaming
gf_cfg_set_key(m_config, "General", "ModulesDirectory", "./modules");
gf_cfg_set_key(m_config, "Video", "DriverName", "DirectX");
gf_cfg_set_key(m_config, "Audio", "DriverName", "DirectSound");
gf_cfg_set_key(m_config, "Network", "BufferLength", "3000");
```

#### Memory Management:
- Implement proper cleanup in destructors
- Monitor memory usage during streaming
- Handle GPAC errors gracefully

## Expected Benefits

After real GPAC integration:

1. **Better Discontinuity Handling:** GPAC will seamlessly handle stream discontinuities
2. **Real Ad Detection:** Actual analysis of stream data for ad markers
3. **Improved Performance:** Native video rendering without external processes
4. **Enhanced User Experience:** Integrated playback within application windows
5. **Reduced Dependencies:** No need for external media players

## Troubleshooting

Common issues and solutions:

- **GPAC Library Not Found:** Ensure GPAC is properly built and linked
- **Video Not Rendering:** Check GPAC video driver configuration  
- **Stream Connection Fails:** Verify GPAC network settings and codec support
- **High CPU Usage:** Optimize GPAC buffer settings and rendering options

## Conclusion

The current implementation provides a complete framework for GPAC integration. The stub implementation can be easily replaced with real GPAC API calls following this guide, resulting in a professional-grade streaming application with built-in video rendering and advanced features like ad-skipping.