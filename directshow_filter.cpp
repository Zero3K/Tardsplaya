// DirectShow Filter Implementation for Tardsplaya Discontinuity Handling
// Provides a standard DirectShow interface for MPC-HC and other players

#include "directshow_filter.h"
#include <strsafe.h>
#include <dvdmedia.h>

// DirectShow base classes and utilities
#pragma comment(lib, "strmbase.lib")
#pragma comment(lib, "msvcrt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "winmm.lib")

//////////////////////////////////////////////////////////////////////////
// CTardsplayaFilterCommunication Implementation
//////////////////////////////////////////////////////////////////////////

CTardsplayaFilterCommunication::CTardsplayaFilterCommunication()
    : pipe_handle_(INVALID_HANDLE_VALUE)
    , pipe_connected_(false)
    , pipe_name_(L"\\\\.\\pipe\\TardsplayaFilter")
{
}

CTardsplayaFilterCommunication::~CTardsplayaFilterCommunication()
{
    Cleanup();
}

bool CTardsplayaFilterCommunication::Initialize(const std::wstring& pipe_name)
{
    pipe_name_ = pipe_name;
    
    // Create named pipe for communication with Tardsplaya main app
    pipe_handle_ = CreateNamedPipe(
        pipe_name_.c_str(),
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,                    // Max instances
        0,                    // Out buffer size
        sizeof(TardsplayaFilterData) * 64,  // In buffer size
        5000,                 // Default timeout
        nullptr               // Security attributes
    );
    
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    return true;
}

void CTardsplayaFilterCommunication::Cleanup()
{
    pipe_connected_ = false;
    
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(pipe_handle_);
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }
}

bool CTardsplayaFilterCommunication::WaitForConnection(DWORD timeout_ms)
{
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    // Wait for client connection
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    
    if (!overlapped.hEvent) {
        return false;
    }
    
    BOOL connected = ConnectNamedPipe(pipe_handle_, &overlapped);
    
    if (!connected) {
        if (GetLastError() == ERROR_IO_PENDING) {
            DWORD result = WaitForSingleObject(overlapped.hEvent, timeout_ms);
            connected = (result == WAIT_OBJECT_0);
        } else if (GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
    }
    
    CloseHandle(overlapped.hEvent);
    
    pipe_connected_ = (connected == TRUE);
    return pipe_connected_;
}

bool CTardsplayaFilterCommunication::ReadPacketData(TardsplayaFilterData& data, DWORD timeout_ms)
{
    if (!pipe_connected_ || pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD bytesRead = 0;
    BOOL success = ReadFile(
        pipe_handle_,
        &data,
        sizeof(TardsplayaFilterData),
        &bytesRead,
        nullptr
    );
    
    if (!success || bytesRead != sizeof(TardsplayaFilterData)) {
        // Connection lost or invalid data
        pipe_connected_ = false;
        return false;
    }
    
    return true;
}

//////////////////////////////////////////////////////////////////////////
// CTardsplayaSourcePin Implementation
//////////////////////////////////////////////////////////////////////////

CTardsplayaSourcePin::CTardsplayaSourcePin(HRESULT* phr, CTardsplayaDiscontinuityFilter* pFilter)
    : CSourceStream(NAME("TardsplayaSourcePin"), phr, pFilter, TARDSPLAYA_PIN_NAME)
    , m_pFilter(pFilter)
    , m_endOfStream(false)
    , m_currentTimestamp(0)
    , m_lastTimestamp(0)
    , m_lastStreamId(0)
    , m_discontinuityDetected(false)
{
    // Initialize timestamp
    m_currentTimestamp = 0;
}

CTardsplayaSourcePin::~CTardsplayaSourcePin()
{
    // Signal any waiting threads
    m_dataAvailable.notify_all();
}

HRESULT CTardsplayaSourcePin::FillBuffer(IMediaSample* pSample)
{
    if (!pSample) {
        return E_POINTER;
    }
    
    // Check for end of stream
    if (m_endOfStream) {
        return S_FALSE; // End of stream
    }
    
    // Get buffer from sample
    BYTE* pBuffer = nullptr;
    HRESULT hr = pSample->GetPointer(&pBuffer);
    if (FAILED(hr)) {
        return hr;
    }
    
    long bufferSize = pSample->GetSize();
    if (bufferSize < PACKET_SIZE) {
        return E_FAIL;
    }
    
    // Wait for data or timeout
    TardsplayaFilterData packetData;
    bool hasData = false;
    
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_dataAvailable.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return !m_packetQueue.empty() || m_endOfStream.load();
        });
        
        if (!m_packetQueue.empty()) {
            packetData = m_packetQueue.front();
            m_packetQueue.pop();
            hasData = true;
        }
    }
    
    if (!hasData) {
        if (m_endOfStream) {
            return S_FALSE;
        }
        // No data available, return empty sample
        pSample->SetActualDataLength(0);
        return S_OK;
    }
    
    // Process discontinuity if detected
    if (packetData.discontinuity_detected) {
        ProcessDiscontinuity(packetData);
    }
    
    // Copy transport stream packet data
    memcpy(pBuffer, packetData.packet.data, PACKET_SIZE);
    pSample->SetActualDataLength(PACKET_SIZE);
    
    // Set timestamp
    REFERENCE_TIME rtStart = packetData.timestamp;
    REFERENCE_TIME rtEnd = rtStart + 1; // Small duration for each packet
    pSample->SetTime(&rtStart, &rtEnd);
    
    // Set discontinuity flag if needed
    if (m_discontinuityDetected.exchange(false)) {
        pSample->SetDiscontinuity(TRUE);
    }
    
    m_currentTimestamp = packetData.timestamp;
    
    return S_OK;
}

HRESULT CTardsplayaSourcePin::DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pProperties)
{
    if (!pAlloc || !pProperties) {
        return E_POINTER;
    }
    
    // Set buffer properties for transport stream packets
    pProperties->cBuffers = 32;           // Number of buffers
    pProperties->cbBuffer = PACKET_SIZE;   // Size per buffer (TS packet size)
    pProperties->cbAlign = 1;             // Byte alignment
    pProperties->cbPrefix = 0;            // No prefix
    
    ALLOCATOR_PROPERTIES actualProperties;
    HRESULT hr = pAlloc->SetProperties(pProperties, &actualProperties);
    
    if (FAILED(hr)) {
        return hr;
    }
    
    // Verify we got acceptable properties
    if (actualProperties.cbBuffer < PACKET_SIZE || actualProperties.cBuffers < 1) {
        return E_FAIL;
    }
    
    return S_OK;
}

HRESULT CTardsplayaSourcePin::CheckMediaType(const CMediaType* pMediaType)
{
    if (!pMediaType) {
        return E_POINTER;
    }
    
    // Accept MPEG-2 Transport Stream
    if (*pMediaType->Type() == MEDIATYPE_Stream &&
        *pMediaType->Subtype() == MEDIASUBTYPE_MPEG2_TRANSPORT) {
        return S_OK;
    }
    
    // Accept our custom transport stream format
    if (*pMediaType->Type() == MEDIATYPE_TardsplayaTransportStream &&
        *pMediaType->Subtype() == MEDIASUBTYPE_TardsplayaFrameTaggedTS) {
        return S_OK;
    }
    
    return S_FALSE;
}

HRESULT CTardsplayaSourcePin::GetMediaType(int iPosition, CMediaType* pMediaType)
{
    if (!pMediaType) {
        return E_POINTER;
    }
    
    if (iPosition < 0) {
        return E_INVALIDARG;
    }
    
    switch (iPosition) {
    case 0:
        // Primary: Standard MPEG-2 Transport Stream
        pMediaType->SetType(&MEDIATYPE_Stream);
        pMediaType->SetSubtype(&MEDIASUBTYPE_MPEG2_TRANSPORT);
        pMediaType->SetFormatType(&FORMAT_None);
        pMediaType->SetTemporalCompression(FALSE);
        pMediaType->SetSampleSize(PACKET_SIZE);
        return S_OK;
        
    case 1:
        // Secondary: Our custom transport stream format
        pMediaType->SetType(&MEDIATYPE_TardsplayaTransportStream);
        pMediaType->SetSubtype(&MEDIASUBTYPE_TardsplayaFrameTaggedTS);
        pMediaType->SetFormatType(&FORMAT_None);
        pMediaType->SetTemporalCompression(FALSE);
        pMediaType->SetSampleSize(PACKET_SIZE);
        return S_OK;
        
    default:
        return VFW_S_NO_MORE_ITEMS;
    }
}

HRESULT CTardsplayaSourcePin::SetMediaType(const CMediaType* pMediaType)
{
    HRESULT hr = CSourceStream::SetMediaType(pMediaType);
    if (FAILED(hr)) {
        return hr;
    }
    
    // Additional setup if needed
    return S_OK;
}

HRESULT CTardsplayaSourcePin::Notify(IBaseFilter* pSender, Quality q)
{
    // Handle quality messages (e.g., for flow control)
    // For now, just pass to base class
    return CSourceStream::Notify(pSender, q);
}

bool CTardsplayaSourcePin::HasDataAvailable()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return !m_packetQueue.empty();
}

void CTardsplayaSourcePin::SignalEndOfStream()
{
    m_endOfStream = true;
    m_dataAvailable.notify_all();
}

void CTardsplayaSourcePin::ResetStreamState()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    // Clear packet queue
    std::queue<TardsplayaFilterData> empty;
    m_packetQueue.swap(empty);
    
    // Reset state
    m_endOfStream = false;
    m_currentTimestamp = 0;
    m_lastTimestamp = 0;
    m_lastStreamId = 0;
    m_discontinuityDetected = false;
    
    m_dataAvailable.notify_all();
}

void CTardsplayaSourcePin::ProcessDiscontinuity(TardsplayaFilterData& data)
{
    // Handle discontinuity in the stream
    // Mark for DirectShow discontinuity flag
    m_discontinuityDetected = true;
    
    // Reset timing if significant gap
    if (data.timestamp > m_lastTimestamp + 1000000) { // 100ms gap
        m_currentTimestamp = data.timestamp;
    }
    
    m_lastTimestamp = data.timestamp;
    m_lastStreamId = data.stream_id;
}

bool CTardsplayaSourcePin::QueuePacketData(const TardsplayaFilterData& data)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    // Prevent queue overflow
    if (m_packetQueue.size() >= MAX_QUEUE_SIZE) {
        // Remove oldest packet
        m_packetQueue.pop();
    }
    
    m_packetQueue.push(data);
    m_dataAvailable.notify_one();
    
    return true;
}

bool CTardsplayaSourcePin::DequeuePacketData(TardsplayaFilterData& data)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    if (m_packetQueue.empty()) {
        return false;
    }
    
    data = m_packetQueue.front();
    m_packetQueue.pop();
    
    return true;
}

//////////////////////////////////////////////////////////////////////////
// CTardsplayaDiscontinuityFilter Implementation
//////////////////////////////////////////////////////////////////////////

CTardsplayaDiscontinuityFilter::CTardsplayaDiscontinuityFilter(LPUNKNOWN pUnk, HRESULT* phr)
    : CSource(NAME("TardsplayaDiscontinuityFilter"), pUnk, CLSID_TardsplayaDiscontinuityFilter)
    , m_pSourcePin(nullptr)
    , m_stopRequested(false)
{
    // Create source pin
    m_pSourcePin = new CTardsplayaSourcePin(phr, this);
    
    if (!m_pSourcePin) {
        if (phr) *phr = E_OUTOFMEMORY;
        return;
    }
    
    if (FAILED(*phr)) {
        delete m_pSourcePin;
        m_pSourcePin = nullptr;
        return;
    }
}

CTardsplayaDiscontinuityFilter::~CTardsplayaDiscontinuityFilter()
{
    StopCommunication();
    
    if (m_pSourcePin) {
        delete m_pSourcePin;
        m_pSourcePin = nullptr;
    }
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::Run(REFERENCE_TIME tStart)
{
    CAutoLock lock(&m_filterLock);
    
    HRESULT hr = CSource::Run(tStart);
    if (FAILED(hr)) {
        return hr;
    }
    
    // Start communication with Tardsplaya main app
    if (!StartCommunication()) {
        // Continue anyway - may connect later
    }
    
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::Pause()
{
    CAutoLock lock(&m_filterLock);
    return CSource::Pause();
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::Stop()
{
    CAutoLock lock(&m_filterLock);
    
    StopCommunication();
    
    if (m_pSourcePin) {
        m_pSourcePin->ResetStreamState();
    }
    
    return CSource::Stop();
}

CUnknown* WINAPI CTardsplayaDiscontinuityFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT* phr)
{
    CTardsplayaDiscontinuityFilter* pFilter = new CTardsplayaDiscontinuityFilter(pUnk, phr);
    
    if (!pFilter) {
        if (phr) *phr = E_OUTOFMEMORY;
        return nullptr;
    }
    
    return pFilter;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::QueryFilterInfo(FILTER_INFO* pInfo)
{
    if (!pInfo) {
        return E_POINTER;
    }
    
    wcscpy_s(pInfo->achName, TARDSPLAYA_FILTER_NAME);
    pInfo->pGraph = m_pGraph;
    
    if (m_pGraph) {
        m_pGraph->AddRef();
    }
    
    return S_OK;
}

bool CTardsplayaDiscontinuityFilter::StartCommunication(const std::wstring& pipe_name)
{
    StopCommunication();
    
    if (!m_communication.Initialize(pipe_name)) {
        return false;
    }
    
    // Start communication thread
    m_stopRequested = false;
    m_communicationThread = std::thread(&CTardsplayaDiscontinuityFilter::CommunicationThreadProc, this);
    
    return true;
}

void CTardsplayaDiscontinuityFilter::StopCommunication()
{
    m_stopRequested = true;
    
    if (m_communicationThread.joinable()) {
        m_communicationThread.join();
    }
    
    m_communication.Cleanup();
}

void CTardsplayaDiscontinuityFilter::CommunicationThreadProc()
{
    // Wait for connection from Tardsplaya
    if (!m_communication.WaitForConnection(10000)) {
        return; // Timeout or error
    }
    
    // Communication loop
    while (!m_stopRequested && m_communication.IsConnected()) {
        TardsplayaFilterData data;
        
        if (m_communication.ReadPacketData(data, 100)) {
            if (data.end_of_stream) {
                // Signal end of stream
                if (m_pSourcePin) {
                    m_pSourcePin->SignalEndOfStream();
                }
                break;
            }
            
            // Queue packet data for pin to process
            if (m_pSourcePin) {
                m_pSourcePin->QueuePacketData(data);
            }
        } else {
            // Read failed or timeout
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void CTardsplayaDiscontinuityFilter::ResetFilterState()
{
    if (m_pSourcePin) {
        m_pSourcePin->ResetStreamState();
    }
}

//////////////////////////////////////////////////////////////////////////
// Factory Template
//////////////////////////////////////////////////////////////////////////

const CFactoryTemplate CTardsplayaDiscontinuityFilterTemplate::g_Template = {
    TARDSPLAYA_FILTER_NAME,
    &CLSID_TardsplayaDiscontinuityFilter,
    CTardsplayaDiscontinuityFilter::CreateInstance,
    nullptr,
    nullptr
};

//////////////////////////////////////////////////////////////////////////
// Registration Functions
//////////////////////////////////////////////////////////////////////////

HRESULT RegisterFilter()
{
    // Register the filter with DirectShow
    IFilterMapper2* pFilterMapper = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FilterMapper2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IFilterMapper2,
        (void**)&pFilterMapper
    );
    
    if (FAILED(hr)) {
        return hr;
    }
    
    REGFILTER2 regFilter = {};
    regFilter.dwVersion = 1;
    regFilter.dwMerit = MERIT_NORMAL;
    regFilter.cPins = 1;
    
    REGFILTERPINS regPins = {};
    regPins.strName = TARDSPLAYA_PIN_NAME;
    regPins.bRendered = FALSE;
    regPins.bOutput = TRUE;
    regPins.bZero = FALSE;
    regPins.bMany = FALSE;
    regPins.clsConnectsToFilter = nullptr;
    regPins.strConnectsToPin = nullptr;
    regPins.nMediaTypes = 1;
    
    REGPINTYPES regPinTypes = {};
    regPinTypes.clsMajorType = &MEDIATYPE_Stream;
    regPinTypes.clsMinorType = &MEDIASUBTYPE_MPEG2_TRANSPORT;
    
    regPins.lpMediaType = &regPinTypes;
    regFilter.rgPins = &regPins;
    
    hr = pFilterMapper->RegisterFilter(
        CLSID_TardsplayaDiscontinuityFilter,
        TARDSPLAYA_FILTER_NAME,
        nullptr,
        &FILTER_CATEGORY,
        nullptr,
        &regFilter
    );
    
    pFilterMapper->Release();
    return hr;
}

HRESULT UnregisterFilter()
{
    IFilterMapper2* pFilterMapper = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FilterMapper2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IFilterMapper2,
        (void**)&pFilterMapper
    );
    
    if (FAILED(hr)) {
        return hr;
    }
    
    hr = pFilterMapper->UnregisterFilter(
        &FILTER_CATEGORY,
        nullptr,
        CLSID_TardsplayaDiscontinuityFilter
    );
    
    pFilterMapper->Release();
    return hr;
}

bool IsFilterRegistered()
{
    // Check if filter is registered by trying to create an instance
    IBaseFilter* pFilter = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TardsplayaDiscontinuityFilter,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IBaseFilter,
        (void**)&pFilter
    );
    
    if (SUCCEEDED(hr) && pFilter) {
        pFilter->Release();
        return true;
    }
    
    return false;
}