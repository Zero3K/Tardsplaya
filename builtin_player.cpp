#include "builtin_player.h"
#include "stream_thread.h"
#include <propvarutil.h>
#include <mfreadwrite.h>
#include <mfidl.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "evr.lib")

// BuiltinMediaPlayer implementation
BuiltinMediaPlayer::BuiltinMediaPlayer()
    : m_pSession(nullptr)
    , m_pSource(nullptr)
    , m_pTopology(nullptr)
    , m_pVideoDisplay(nullptr)
    , m_pVolumeControl(nullptr)
    , m_pCustomSource(nullptr)
    , m_hwndVideo(nullptr)
    , m_isPlaying(false)
    , m_isInitialized(false)
    , m_volume(1.0f)
    , m_feedRunning(false)
{
}

BuiltinMediaPlayer::~BuiltinMediaPlayer() {
    Cleanup();
}

bool BuiltinMediaPlayer::Initialize(HWND hwndVideo) {
    AddDebugLog(L"[BUILTIN] Initializing built-in media player");
    
    if (m_isInitialized.load()) {
        AddDebugLog(L"[BUILTIN] Player already initialized");
        return true;
    }
    
    m_hwndVideo = hwndVideo;
    
    // Initialize Media Foundation
    if (!MediaFoundationUtils::InitializeMediaFoundation()) {
        AddDebugLog(L"[BUILTIN] Failed to initialize Media Foundation");
        return false;
    }
    
    // Create media session
    if (!CreateMediaSession()) {
        AddDebugLog(L"[BUILTIN] Failed to create media session");
        return false;
    }
    
    // Create custom media source for streaming data
    if (!CreateCustomMediaSource()) {
        AddDebugLog(L"[BUILTIN] Failed to create custom media source");
        return false;
    }
    
    m_isInitialized = true;
    AddDebugLog(L"[BUILTIN] Built-in media player initialized successfully");
    return true;
}

bool BuiltinMediaPlayer::StartStream(const std::wstring& streamName) {
    AddDebugLog(L"[BUILTIN] Starting stream: " + streamName);
    
    if (!m_isInitialized.load()) {
        AddDebugLog(L"[BUILTIN] Player not initialized");
        return false;
    }
    
    if (m_isPlaying.load()) {
        AddDebugLog(L"[BUILTIN] Already playing, stopping current stream");
        StopStream();
    }
    
    // Build topology for playback
    if (!BuildTopology()) {
        AddDebugLog(L"[BUILTIN] Failed to build topology");
        return false;
    }
    
    // Start the feed thread
    m_feedRunning = true;
    m_feedThread = std::thread(&BuiltinMediaPlayer::FeedThreadProc, this);
    
    // Start playback
    if (!StartPlayback()) {
        AddDebugLog(L"[BUILTIN] Failed to start playback");
        m_feedRunning = false;
        if (m_feedThread.joinable()) {
            m_feedThread.join();
        }
        return false;
    }
    
    m_isPlaying = true;
    AddDebugLog(L"[BUILTIN] Stream started successfully: " + streamName);
    return true;
}

void BuiltinMediaPlayer::StopStream() {
    AddDebugLog(L"[BUILTIN] Stopping stream");
    
    m_isPlaying = false;
    m_feedRunning = false;
    
    // Stop the feed thread
    if (m_feedThread.joinable()) {
        m_feedThread.join();
    }
    
    // Stop media session
    if (m_pSession) {
        m_pSession->Stop();
    }
    
    // Clear data buffer
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        while (!m_dataBuffer.empty()) {
            m_dataBuffer.pop();
        }
    }
    
    AddDebugLog(L"[BUILTIN] Stream stopped");
}

bool BuiltinMediaPlayer::FeedData(const char* data, size_t size) {
    if (!m_isPlaying.load() || !data || size == 0) {
        return false;
    }
    
    // Add data to buffer
    std::vector<char> dataVec(data, data + size);
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_dataBuffer.push(std::move(dataVec));
        
        // Limit buffer size to prevent memory bloat
        while (m_dataBuffer.size() > 100) {
            m_dataBuffer.pop();
        }
    }
    
    return true;
}

bool BuiltinMediaPlayer::IsPlaying() const {
    return m_isPlaying.load();
}

void BuiltinMediaPlayer::SetVolume(float volume) {
    m_volume = std::max(0.0f, std::min(1.0f, volume));
    
    if (m_pVolumeControl) {
        UINT32 channelCount = 0;
        if (SUCCEEDED(m_pVolumeControl->GetChannelCount(&channelCount))) {
            for (UINT32 i = 0; i < channelCount; i++) {
                m_pVolumeControl->SetChannelVolume(i, m_volume.load());
            }
        }
    }
}

float BuiltinMediaPlayer::GetVolume() const {
    return m_volume.load();
}

void BuiltinMediaPlayer::ResizeVideo(int width, int height) {
    if (m_pVideoDisplay && m_hwndVideo) {
        RECT rcDest = { 0, 0, width, height };
        m_pVideoDisplay->SetVideoPosition(nullptr, &rcDest);
    }
}

void BuiltinMediaPlayer::Cleanup() {
    AddDebugLog(L"[BUILTIN] Cleaning up built-in media player");
    
    StopStream();
    
    if (m_pVideoDisplay) {
        m_pVideoDisplay->Release();
        m_pVideoDisplay = nullptr;
    }
    
    if (m_pVolumeControl) {
        m_pVolumeControl->Release();
        m_pVolumeControl = nullptr;
    }
    
    if (m_pTopology) {
        m_pTopology->Release();
        m_pTopology = nullptr;
    }
    
    if (m_pCustomSource) {
        m_pCustomSource->Release();
        m_pCustomSource = nullptr;
    }
    
    if (m_pSession) {
        m_pSession->Shutdown();
        m_pSession->Release();
        m_pSession = nullptr;
    }
    
    m_isInitialized = false;
    MediaFoundationUtils::ShutdownMediaFoundation();
}

bool BuiltinMediaPlayer::CreateMediaSession() {
    HRESULT hr = MFCreateMediaSession(nullptr, &m_pSession);
    if (FAILED(hr)) {
        AddDebugLog(L"[BUILTIN] Failed to create media session, HRESULT: " + std::to_wstring(hr));
        return false;
    }
    
    AddDebugLog(L"[BUILTIN] Media session created successfully");
    return true;
}

bool BuiltinMediaPlayer::CreateCustomMediaSource() {
    // Create our custom media source for streaming data
    m_pCustomSource = new StreamMediaSource();
    if (!m_pCustomSource) {
        AddDebugLog(L"[BUILTIN] Failed to create custom media source");
        return false;
    }
    
    StreamMediaSource* pStreamSource = static_cast<StreamMediaSource*>(m_pCustomSource);
    if (!pStreamSource->Initialize()) {
        AddDebugLog(L"[BUILTIN] Failed to initialize custom media source");
        m_pCustomSource->Release();
        m_pCustomSource = nullptr;
        return false;
    }
    
    AddDebugLog(L"[BUILTIN] Custom media source created successfully");
    return true;
}

bool BuiltinMediaPlayer::BuildTopology() {
    AddDebugLog(L"[BUILTIN] Building topology");
    
    HRESULT hr = MFCreateTopology(&m_pTopology);
    if (FAILED(hr)) {
        AddDebugLog(L"[BUILTIN] Failed to create topology");
        return false;
    }
    
    // Create source node
    IMFTopologyNode* pSourceNode = nullptr;
    hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSourceNode);
    if (FAILED(hr)) {
        AddDebugLog(L"[BUILTIN] Failed to create source node");
        return false;
    }
    
    // Set the media source
    pSourceNode->SetUnknown(MF_TOPONODE_SOURCE, m_pCustomSource);
    
    // Get presentation descriptor
    IMFPresentationDescriptor* pPD = nullptr;
    hr = m_pCustomSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) {
        pSourceNode->Release();
        AddDebugLog(L"[BUILTIN] Failed to get presentation descriptor");
        return false;
    }
    
    // Get stream descriptor  
    IMFStreamDescriptor* pSD = nullptr;
    BOOL fSelected = FALSE;
    hr = pPD->GetStreamDescriptorByIndex(0, &fSelected, &pSD);
    if (FAILED(hr)) {
        pPD->Release();
        pSourceNode->Release();
        AddDebugLog(L"[BUILTIN] Failed to get stream descriptor");
        return false;
    }
    
    pSourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD);
    pSourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSD);
    
    // Add source node to topology
    m_pTopology->AddNode(pSourceNode);
    
    // Create output node for video
    IMFTopologyNode* pOutputNode = nullptr;
    hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOutputNode);
    if (SUCCEEDED(hr)) {
        // Create Enhanced Video Renderer
        IMFActivate* pEVRActivate = nullptr;
        hr = MFCreateVideoRendererActivate(m_hwndVideo, &pEVRActivate);
        if (SUCCEEDED(hr)) {
            pOutputNode->SetObject(pEVRActivate);
            m_pTopology->AddNode(pOutputNode);
            
            // Connect source to output
            pSourceNode->ConnectOutput(0, pOutputNode, 0);
            
            pEVRActivate->Release();
        }
        pOutputNode->Release();
    }
    
    // Cleanup
    pSD->Release();
    pPD->Release();
    pSourceNode->Release();
    
    AddDebugLog(L"[BUILTIN] Topology built successfully");
    return true;
}

bool BuiltinMediaPlayer::StartPlayback() {
    if (!m_pSession || !m_pTopology) {
        AddDebugLog(L"[BUILTIN] Cannot start playback - missing session or topology");
        return false;
    }
    
    // Set the topology
    HRESULT hr = m_pSession->SetTopology(0, m_pTopology);
    if (FAILED(hr)) {
        AddDebugLog(L"[BUILTIN] Failed to set topology, HRESULT: " + std::to_wstring(hr));
        return false;
    }
    
    // Start the session
    PROPVARIANT varStart;
    PropVariantInit(&varStart);
    hr = m_pSession->Start(&GUID_NULL, &varStart);
    PropVariantClear(&varStart);
    
    if (FAILED(hr)) {
        AddDebugLog(L"[BUILTIN] Failed to start session, HRESULT: " + std::to_wstring(hr));
        return false;
    }
    
    AddDebugLog(L"[BUILTIN] Playback started successfully");
    return true;
}

void BuiltinMediaPlayer::FeedThreadProc() {
    AddDebugLog(L"[BUILTIN] Feed thread started");
    
    while (m_feedRunning.load()) {
        std::vector<char> data;
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            if (!m_dataBuffer.empty()) {
                data = std::move(m_dataBuffer.front());
                m_dataBuffer.pop();
            }
        }
        
        if (!data.empty() && m_pCustomSource) {
            StreamMediaSource* pStreamSource = static_cast<StreamMediaSource*>(m_pCustomSource);
            pStreamSource->FeedData(data.data(), data.size());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    AddDebugLog(L"[BUILTIN] Feed thread ended");
}

// StreamMediaSource implementation
StreamMediaSource::StreamMediaSource()
    : m_refCount(1)
    , m_pEventQueue(nullptr)
    , m_pPresentationDescriptor(nullptr)
    , m_pStreamDescriptor(nullptr)
    , m_isShutdown(false)
{
}

StreamMediaSource::~StreamMediaSource() {
    Shutdown();
}

STDMETHODIMP StreamMediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaEventGenerator)) {
        *ppv = this;
        AddRef();
        return S_OK;
    }
    
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) StreamMediaSource::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) StreamMediaSource::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP StreamMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    if (m_pEventQueue) {
        return m_pEventQueue->BeginGetEvent(pCallback, punkState);
    }
    return MF_E_SHUTDOWN;
}

STDMETHODIMP StreamMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    if (m_pEventQueue) {
        return m_pEventQueue->EndGetEvent(pResult, ppEvent);
    }
    return MF_E_SHUTDOWN;
}

STDMETHODIMP StreamMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    if (m_pEventQueue) {
        return m_pEventQueue->GetEvent(dwFlags, ppEvent);
    }
    return MF_E_SHUTDOWN;
}

STDMETHODIMP StreamMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) {
    if (m_pEventQueue) {
        return m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
    }
    return MF_E_SHUTDOWN;
}

STDMETHODIMP StreamMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) {
    if (!ppPresentationDescriptor) return E_POINTER;
    
    if (m_pPresentationDescriptor) {
        m_pPresentationDescriptor->AddRef();
        *ppPresentationDescriptor = m_pPresentationDescriptor;
        return S_OK;
    }
    
    return E_FAIL;
}

STDMETHODIMP StreamMediaSource::GetCharacteristics(DWORD* pdwCharacteristics) {
    if (!pdwCharacteristics) return E_POINTER;
    
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP StreamMediaSource::Pause() {
    return S_OK; // Live streams typically don't support pause
}

STDMETHODIMP StreamMediaSource::Shutdown() {
    m_isShutdown = true;
    
    if (m_pEventQueue) {
        m_pEventQueue->Shutdown();
        m_pEventQueue->Release();
        m_pEventQueue = nullptr;
    }
    
    if (m_pPresentationDescriptor) {
        m_pPresentationDescriptor->Release();
        m_pPresentationDescriptor = nullptr;
    }
    
    if (m_pStreamDescriptor) {
        m_pStreamDescriptor->Release();
        m_pStreamDescriptor = nullptr;
    }
    
    return S_OK;
}

STDMETHODIMP StreamMediaSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) {
    if (m_pEventQueue) {
        m_pEventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, pvarStartPosition);
    }
    return S_OK;
}

STDMETHODIMP StreamMediaSource::Stop() {
    if (m_pEventQueue) {
        m_pEventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    return S_OK;
}

bool StreamMediaSource::FeedData(const char* data, size_t size) {
    if (m_isShutdown.load() || !data || size == 0) {
        return false;
    }
    
    // Add data to queue for processing
    std::vector<char> dataVec(data, data + size);
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_dataQueue.push(std::move(dataVec));
        
        // Limit queue size
        while (m_dataQueue.size() > 50) {
            m_dataQueue.pop();
        }
    }
    
    return true;
}

bool StreamMediaSource::Initialize() {
    // Create event queue
    HRESULT hr = MFCreateEventQueue(&m_pEventQueue);
    if (FAILED(hr)) {
        return false;
    }
    
    // Create presentation descriptor
    return CreatePresentationDescriptorInternal();
}

bool StreamMediaSource::CreatePresentationDescriptorInternal() {
    // Create media type for transport stream
    IMFMediaType* pMediaType = nullptr;
    if (!MediaFoundationUtils::CreateMediaTypeForTS(&pMediaType)) {
        return false;
    }
    
    // Create stream descriptor
    HRESULT hr = MFCreateStreamDescriptor(0, 1, &pMediaType, &m_pStreamDescriptor);
    pMediaType->Release();
    
    if (FAILED(hr)) {
        return false;
    }
    
    // Create presentation descriptor
    hr = MFCreatePresentationDescriptor(1, &m_pStreamDescriptor, &m_pPresentationDescriptor);
    if (FAILED(hr)) {
        return false;
    }
    
    // Set duration to unknown (live stream)
    hr = m_pPresentationDescriptor->SetUINT64(MF_PD_DURATION, 0);
    
    return SUCCEEDED(hr);
}

// MediaFoundationUtils implementation
bool MediaFoundationUtils::InitializeMediaFoundation() {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        AddDebugLog(L"[BUILTIN] Failed to initialize Media Foundation, HRESULT: " + std::to_wstring(hr));
        return false;
    }
    
    AddDebugLog(L"[BUILTIN] Media Foundation initialized successfully");
    return true;
}

void MediaFoundationUtils::ShutdownMediaFoundation() {
    MFShutdown();
    AddDebugLog(L"[BUILTIN] Media Foundation shut down");
}

bool MediaFoundationUtils::CreateMediaTypeForTS(IMFMediaType** ppMediaType) {
    if (!ppMediaType) return false;
    
    HRESULT hr = MFCreateMediaType(ppMediaType);
    if (FAILED(hr)) {
        return false;
    }
    
    // Set major type to video (transport streams typically contain video)
    (*ppMediaType)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    
    // Set subtype to H.264 (most common in HLS streams)
    (*ppMediaType)->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    
    // Set other basic properties
    (*ppMediaType)->SetUINT32(MF_MT_COMPRESSED, TRUE);
    
    return true;
}