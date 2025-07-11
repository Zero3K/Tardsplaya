#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <evr.h>
#include <d3d9.h>
#include <vmr9.h>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>

// Built-in media player using Windows Media Foundation
class BuiltinMediaPlayer {
public:
    BuiltinMediaPlayer();
    ~BuiltinMediaPlayer();

    // Initialize the player with a target window for video rendering
    bool Initialize(HWND hwndVideo);
    
    // Start playing a stream from memory data
    bool StartStream(const std::wstring& streamName);
    
    // Stop the stream
    void StopStream();
    
    // Feed raw stream data (TS segments) to the player
    bool FeedData(const char* data, size_t size);
    
    // Check if player is currently playing
    bool IsPlaying() const;
    
    // Set volume (0.0 to 1.0)
    void SetVolume(float volume);
    
    // Get current volume
    float GetVolume() const;
    
    // Resize video to fit window
    void ResizeVideo(int width, int height);
    
    // Cleanup resources
    void Cleanup();

private:
    // Media Foundation interfaces
    IMFMediaSession* m_pSession;
    IMFMediaSource* m_pSource;
    IMFTopology* m_pTopology;
    IMFVideoDisplayControl* m_pVideoDisplay;
    IMFAudioStreamVolume* m_pVolumeControl;
    
    // Custom media source for streaming data
    IMFMediaSource* m_pCustomSource;
    
    // Window handles
    HWND m_hwndVideo;
    
    // Playback state
    std::atomic<bool> m_isPlaying;
    std::atomic<bool> m_isInitialized;
    std::atomic<float> m_volume;
    
    // Stream data buffer
    std::queue<std::vector<char>> m_dataBuffer;
    std::mutex m_bufferMutex;
    std::thread m_feedThread;
    std::atomic<bool> m_feedRunning;
    
    // Internal methods
    bool CreateMediaSession();
    bool CreateCustomMediaSource();
    bool BuildTopology();
    bool StartPlayback();
    void FeedThreadProc();
    
    // Media Foundation event handling
    static LRESULT CALLBACK MediaEventProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMediaEvent(WPARAM wParam, LPARAM lParam);
};

// Custom media source for streaming HLS/TS data
class StreamMediaSource : public IMFMediaSource {
public:
    StreamMediaSource();
    virtual ~StreamMediaSource();
    
    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    
    // IMFMediaEventGenerator methods
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;
    
    // IMFMediaSource methods
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) override;
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;
    
    // Custom methods for feeding data
    bool FeedData(const char* data, size_t size);
    bool Initialize();

private:
    ULONG m_refCount;
    IMFMediaEventQueue* m_pEventQueue;
    IMFPresentationDescriptor* m_pPresentationDescriptor;
    IMFStreamDescriptor* m_pStreamDescriptor;
    
    std::queue<std::vector<char>> m_dataQueue;
    std::mutex m_dataMutex;
    std::atomic<bool> m_isShutdown;
    
    bool CreatePresentationDescriptorInternal();
};

// Utility functions for Media Foundation
class MediaFoundationUtils {
public:
    static bool InitializeMediaFoundation();
    static void ShutdownMediaFoundation();
    static bool CreateMediaTypeForTS(IMFMediaType** ppMediaType);
};