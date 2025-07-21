// Simplified DirectShow Filter Implementation for Tardsplaya
// Compatible with MinGW-w64 cross-compilation

#include "directshow_filter_simple.h"
#include <strsafe.h>

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
    
    pipe_handle_ = CreateNamedPipe(
        pipe_name_.c_str(),
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        0,
        sizeof(TardsplayaFilterData) * 64,
        5000,
        nullptr
    );
    
    return pipe_handle_ != INVALID_HANDLE_VALUE;
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
    BOOL success = ReadFile(pipe_handle_, &data, sizeof(TardsplayaFilterData), &bytesRead, nullptr);
    
    if (!success || bytesRead != sizeof(TardsplayaFilterData)) {
        pipe_connected_ = false;
        return false;
    }
    
    return true;
}

//////////////////////////////////////////////////////////////////////////
// CTardsplayaSourcePin Implementation
//////////////////////////////////////////////////////////////////////////

CTardsplayaSourcePin::CTardsplayaSourcePin(CTardsplayaDiscontinuityFilter* pFilter)
    : m_pFilter(pFilter)
    , m_pConnectedPin(nullptr)
    , m_pAllocator(nullptr)
    , m_connected(false)
    , m_endOfStream(false)
{
    ZeroMemory(&m_mediaType, sizeof(AM_MEDIA_TYPE));
}

CTardsplayaSourcePin::~CTardsplayaSourcePin()
{
    if (m_pConnectedPin) {
        m_pConnectedPin->Release();
    }
    if (m_pAllocator) {
        m_pAllocator->Release();
    }
}

STDMETHODIMP CTardsplayaSourcePin::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    
    if (riid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(static_cast<IPin*>(this));
    } else if (riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IMemInputPin) {
        *ppv = static_cast<IMemInputPin*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    
    AddRef();
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt)
{
    if (!pReceivePin) return E_POINTER;
    
    if (m_connected) return VFW_E_ALREADY_CONNECTED;
    
    // Try to connect with specified media type
    HRESULT hr = pReceivePin->ReceiveConnection(this, pmt);
    if (SUCCEEDED(hr)) {
        m_pConnectedPin = pReceivePin;
        m_pConnectedPin->AddRef();
        m_connected = true;
        CopyMemory(&m_mediaType, pmt, sizeof(AM_MEDIA_TYPE));
    }
    
    return hr;
}

STDMETHODIMP CTardsplayaSourcePin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt)
{
    return VFW_E_TYPE_NOT_ACCEPTED; // Source pin doesn't receive connections
}

STDMETHODIMP CTardsplayaSourcePin::Disconnect()
{
    if (!m_connected) return S_FALSE;
    
    if (m_pConnectedPin) {
        m_pConnectedPin->Release();
        m_pConnectedPin = nullptr;
    }
    
    m_connected = false;
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::ConnectedTo(IPin** pPin)
{
    if (!pPin) return E_POINTER;
    
    if (!m_connected) {
        *pPin = nullptr;
        return VFW_E_NOT_CONNECTED;
    }
    
    *pPin = m_pConnectedPin;
    if (*pPin) (*pPin)->AddRef();
    
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::ConnectionMediaType(AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    
    if (!m_connected) return VFW_E_NOT_CONNECTED;
    
    CopyMemory(pmt, &m_mediaType, sizeof(AM_MEDIA_TYPE));
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::QueryPinInfo(PIN_INFO* pInfo)
{
    if (!pInfo) return E_POINTER;
    
    wcscpy_s(pInfo->achName, TARDSPLAYA_PIN_NAME);
    pInfo->dir = PINDIR_OUTPUT;
    pInfo->pFilter = static_cast<IBaseFilter*>(m_pFilter);
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::QueryDirection(PIN_DIRECTION* pPinDir)
{
    if (!pPinDir) return E_POINTER;
    
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::QueryId(LPWSTR* Id)
{
    if (!Id) return E_POINTER;
    
    *Id = (LPWSTR)CoTaskMemAlloc((wcslen(TARDSPLAYA_PIN_NAME) + 1) * sizeof(WCHAR));
    if (!*Id) return E_OUTOFMEMORY;
    
    wcscpy(*Id, TARDSPLAYA_PIN_NAME);
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
    return CheckMediaType(pmt) ? S_OK : S_FALSE;
}

STDMETHODIMP CTardsplayaSourcePin::EnumMediaTypes(IEnumMediaTypes** ppEnum)
{
    // Simplified - return E_NOTIMPL for now
    return E_NOTIMPL;
}

STDMETHODIMP CTardsplayaSourcePin::QueryInternalConnections(IPin** apPin, ULONG* nPin)
{
    return E_NOTIMPL;
}

STDMETHODIMP CTardsplayaSourcePin::EndOfStream()
{
    m_endOfStream = true;
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::BeginFlush()
{
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::EndFlush()
{
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate)
{
    return S_OK;
}

// IMemInputPin methods
STDMETHODIMP CTardsplayaSourcePin::GetAllocator(IMemAllocator** ppAllocator)
{
    return VFW_E_NO_ALLOCATOR;
}

STDMETHODIMP CTardsplayaSourcePin::NotifyAllocator(IMemAllocator* pAllocator, BOOL bReadOnly)
{
    if (m_pAllocator) {
        m_pAllocator->Release();
    }
    m_pAllocator = pAllocator;
    if (m_pAllocator) {
        m_pAllocator->AddRef();
    }
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pProps)
{
    if (!pProps) return E_POINTER;
    
    pProps->cBuffers = 32;
    pProps->cbBuffer = PACKET_SIZE;
    pProps->cbAlign = 1;
    pProps->cbPrefix = 0;
    
    return S_OK;
}

STDMETHODIMP CTardsplayaSourcePin::Receive(IMediaSample* pSample)
{
    return E_NOTIMPL; // Source pin doesn't receive samples
}

STDMETHODIMP CTardsplayaSourcePin::ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nSamplesProcessed)
{
    return E_NOTIMPL;
}

STDMETHODIMP CTardsplayaSourcePin::ReceiveCanBlock()
{
    return S_FALSE;
}

bool CTardsplayaSourcePin::CheckMediaType(const AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return false;
    
    // Accept MPEG-2 Transport Stream
    return (pmt->majortype == MEDIATYPE_Stream && 
            pmt->subtype == MEDIASUBTYPE_MPEG2_TRANSPORT);
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
    
    std::queue<TardsplayaFilterData> empty;
    m_packetQueue.swap(empty);
    
    m_endOfStream = false;
    m_dataAvailable.notify_all();
}

bool CTardsplayaSourcePin::QueuePacketData(const TardsplayaFilterData& data)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    if (m_packetQueue.size() >= MAX_QUEUE_SIZE) {
        m_packetQueue.pop();
    }
    
    m_packetQueue.push(data);
    m_dataAvailable.notify_one();
    
    return true;
}

void CTardsplayaSourcePin::ProcessDiscontinuity(TardsplayaFilterData& data)
{
    // Handle discontinuity - simplified implementation
    // In a full implementation, this would reset timing and signal DirectShow
}

//////////////////////////////////////////////////////////////////////////
// CTardsplayaDiscontinuityFilter Implementation
//////////////////////////////////////////////////////////////////////////

CTardsplayaDiscontinuityFilter::CTardsplayaDiscontinuityFilter()
    : m_pSourcePin(nullptr)
    , m_stopRequested(false)
    , m_pGraph(nullptr)
    , m_pClock(nullptr)
    , m_state(State_Stopped)
    , m_filterName(TARDSPLAYA_FILTER_NAME)
{
    InitializeCriticalSection(&m_critSec);
    m_pSourcePin = new CTardsplayaSourcePin(this);
}

CTardsplayaDiscontinuityFilter::~CTardsplayaDiscontinuityFilter()
{
    StopCommunication();
    
    if (m_pSourcePin) {
        delete m_pSourcePin;
    }
    
    if (m_pGraph) {
        m_pGraph->Release();
    }
    
    if (m_pClock) {
        m_pClock->Release();
    }
    
    DeleteCriticalSection(&m_critSec);
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    
    if (riid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(this);
    } else if (riid == IID_IPersist) {
        *ppv = static_cast<IPersist*>(this);
    } else if (riid == IID_IMediaFilter) {
        *ppv = static_cast<IMediaFilter*>(this);
    } else if (riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    
    AddRef();
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::GetClassID(CLSID* pClassID)
{
    if (!pClassID) return E_POINTER;
    
    *pClassID = CLSID_TardsplayaDiscontinuityFilter;
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::Stop()
{
    EnterCriticalSection(&m_critSec);
    
    StopCommunication();
    if (m_pSourcePin) {
        m_pSourcePin->ResetStreamState();
    }
    
    m_state = State_Stopped;
    
    LeaveCriticalSection(&m_critSec);
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::Pause()
{
    EnterCriticalSection(&m_critSec);
    m_state = State_Paused;
    LeaveCriticalSection(&m_critSec);
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::Run(REFERENCE_TIME tStart)
{
    EnterCriticalSection(&m_critSec);
    
    if (!StartCommunication()) {
        // Continue anyway - may connect later
    }
    
    m_state = State_Running;
    
    LeaveCriticalSection(&m_critSec);
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State)
{
    if (!State) return E_POINTER;
    
    EnterCriticalSection(&m_critSec);
    *State = m_state;
    LeaveCriticalSection(&m_critSec);
    
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::SetSyncSource(IReferenceClock* pClock)
{
    EnterCriticalSection(&m_critSec);
    
    if (m_pClock) {
        m_pClock->Release();
    }
    
    m_pClock = pClock;
    if (m_pClock) {
        m_pClock->AddRef();
    }
    
    LeaveCriticalSection(&m_critSec);
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::GetSyncSource(IReferenceClock** pClock)
{
    if (!pClock) return E_POINTER;
    
    EnterCriticalSection(&m_critSec);
    
    *pClock = m_pClock;
    if (*pClock) {
        (*pClock)->AddRef();
    }
    
    LeaveCriticalSection(&m_critSec);
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::EnumPins(IEnumPins** ppEnum)
{
    // Simplified - return E_NOTIMPL for now
    return E_NOTIMPL;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::FindPin(LPCWSTR Id, IPin** ppPin)
{
    if (!ppPin) return E_POINTER;
    
    if (wcscmp(Id, TARDSPLAYA_PIN_NAME) == 0) {
        *ppPin = static_cast<IPin*>(m_pSourcePin);
        (*ppPin)->AddRef();
        return S_OK;
    }
    
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::QueryFilterInfo(FILTER_INFO* pInfo)
{
    if (!pInfo) return E_POINTER;
    
    wcscpy_s(pInfo->achName, m_filterName.c_str());
    pInfo->pGraph = m_pGraph;
    
    if (m_pGraph) {
        m_pGraph->AddRef();
    }
    
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName)
{
    EnterCriticalSection(&m_critSec);
    
    if (m_pGraph) {
        m_pGraph->Release();
    }
    
    m_pGraph = pGraph;
    if (m_pGraph) {
        m_pGraph->AddRef();
    }
    
    if (pName) {
        m_filterName = pName;
    }
    
    LeaveCriticalSection(&m_critSec);
    return S_OK;
}

STDMETHODIMP CTardsplayaDiscontinuityFilter::QueryVendorInfo(LPWSTR* pVendorInfo)
{
    return E_NOTIMPL;
}

HRESULT CTardsplayaDiscontinuityFilter::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv)
{
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;
    
    CTardsplayaDiscontinuityFilter* pFilter = new CTardsplayaDiscontinuityFilter();
    if (!pFilter) return E_OUTOFMEMORY;
    
    HRESULT hr = pFilter->QueryInterface(riid, ppv);
    pFilter->Release();
    
    return hr;
}

bool CTardsplayaDiscontinuityFilter::StartCommunication(const std::wstring& pipe_name)
{
    StopCommunication();
    
    if (!m_communication.Initialize(pipe_name)) {
        return false;
    }
    
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
    if (!m_communication.WaitForConnection(10000)) {
        return;
    }
    
    while (!m_stopRequested && m_communication.IsConnected()) {
        TardsplayaFilterData data;
        
        if (m_communication.ReadPacketData(data, 100)) {
            if (data.end_of_stream) {
                if (m_pSourcePin) {
                    m_pSourcePin->SignalEndOfStream();
                }
                break;
            }
            
            if (m_pSourcePin) {
                m_pSourcePin->QueuePacketData(data);
            }
        } else {
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
// CClassFactory Implementation
//////////////////////////////////////////////////////////////////////////

CClassFactory::CClassFactory()
{
}

STDMETHODIMP CClassFactory::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *ppv = this;
        AddRef();
        return S_OK;
    }
    
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP CClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv)
{
    return CTardsplayaDiscontinuityFilter::CreateInstance(pUnkOuter, riid, ppv);
}

STDMETHODIMP CClassFactory::LockServer(BOOL fLock)
{
    if (fLock) {
        InterlockedIncrement(&g_cServerLocks);
    } else {
        InterlockedDecrement(&g_cServerLocks);
    }
    return S_OK;
}

//////////////////////////////////////////////////////////////////////////
// Registration Functions (simplified)
//////////////////////////////////////////////////////////////////////////

HRESULT RegisterFilter()
{
    // Simplified registration - just register COM class
    // Full DirectShow registration would require IFilterMapper2
    return S_OK;
}

HRESULT UnregisterFilter()
{
    return S_OK;
}

bool IsFilterRegistered()
{
    return true; // Simplified
}