// Simplified DirectShow Filter Implementation for Tardsplaya
// Compatible with MinGW-w64 cross-compilation without full DirectShow SDK

#include <windows.h>
#include <objbase.h>
#include <olectl.h>
#include <strmif.h>
#include <uuids.h>
#include <atomic>
#include <memory>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "filter_guids.h"
#include "tsduck_transport_router.h"

// Forward declarations
class CTardsplayaDiscontinuityFilter;
class CTardsplayaSourcePin;

// Communication structure for Tardsplaya main app to filter
struct TardsplayaFilterData {
    tsduck_transport::TSPacket packet;
    DWORD stream_id;
    LONGLONG timestamp;
    bool discontinuity_detected;
    bool end_of_stream;
    
    TardsplayaFilterData() : stream_id(0), timestamp(0), discontinuity_detected(false), end_of_stream(false) {}
};

//////////////////////////////////////////////////////////////////////////
// Simplified Communication Interface
//////////////////////////////////////////////////////////////////////////

class CTardsplayaFilterCommunication {
public:
    CTardsplayaFilterCommunication();
    ~CTardsplayaFilterCommunication();
    
    bool Initialize(const std::wstring& pipe_name = L"\\\\.\\pipe\\TardsplayaFilter");
    void Cleanup();
    bool WaitForConnection(DWORD timeout_ms = 5000);
    bool ReadPacketData(TardsplayaFilterData& data, DWORD timeout_ms = 1000);
    bool IsConnected() const { return pipe_connected_; }
    
private:
    HANDLE pipe_handle_;
    std::atomic<bool> pipe_connected_;
    std::wstring pipe_name_;
};

//////////////////////////////////////////////////////////////////////////
// IUnknown Implementation Helper
//////////////////////////////////////////////////////////////////////////

class CUnknownImpl {
public:
    CUnknownImpl() : m_refCount(1) {}
    virtual ~CUnknownImpl() {}
    
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) = 0;
    STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&m_refCount); }
    STDMETHOD_(ULONG, Release)() { 
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }
    
protected:
    volatile LONG m_refCount;
};

//////////////////////////////////////////////////////////////////////////
// Simplified DirectShow Source Pin
//////////////////////////////////////////////////////////////////////////

class CTardsplayaSourcePin : 
    public CUnknownImpl,
    public IPin,
    public IMemInputPin
{
public:
    CTardsplayaSourcePin(CTardsplayaDiscontinuityFilter* pFilter);
    virtual ~CTardsplayaSourcePin();
    
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    
    // IPin
    STDMETHOD(Connect)(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHOD(ReceiveConnection)(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHOD(Disconnect)() override;
    STDMETHOD(ConnectedTo)(IPin** pPin) override;
    STDMETHOD(ConnectionMediaType)(AM_MEDIA_TYPE* pmt) override;
    STDMETHOD(QueryPinInfo)(PIN_INFO* pInfo) override;
    STDMETHOD(QueryDirection)(PIN_DIRECTION* pPinDir) override;
    STDMETHOD(QueryId)(LPWSTR* Id) override;
    STDMETHOD(QueryAccept)(const AM_MEDIA_TYPE* pmt) override;
    STDMETHOD(EnumMediaTypes)(IEnumMediaTypes** ppEnum) override;
    STDMETHOD(QueryInternalConnections)(IPin** apPin, ULONG* nPin) override;
    STDMETHOD(EndOfStream)() override;
    STDMETHOD(BeginFlush)() override;
    STDMETHOD(EndFlush)() override;
    STDMETHOD(NewSegment)(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;
    
    // IMemInputPin  
    STDMETHOD(GetAllocator)(IMemAllocator** ppAllocator) override;
    STDMETHOD(NotifyAllocator)(IMemAllocator* pAllocator, BOOL bReadOnly) override;
    STDMETHOD(GetAllocatorRequirements)(ALLOCATOR_PROPERTIES* pProps) override;
    STDMETHOD(Receive)(IMediaSample* pSample) override;
    STDMETHOD(ReceiveMultiple)(IMediaSample** pSamples, long nSamples, long* nSamplesProcessed) override;
    STDMETHOD(ReceiveCanBlock)() override;
    
    // Custom methods
    bool HasDataAvailable();
    void SignalEndOfStream();
    void ResetStreamState();
    bool QueuePacketData(const TardsplayaFilterData& data);
    
private:
    CTardsplayaDiscontinuityFilter* m_pFilter;
    IPin* m_pConnectedPin;
    IMemAllocator* m_pAllocator;
    AM_MEDIA_TYPE m_mediaType;
    bool m_connected;
    
    std::queue<TardsplayaFilterData> m_packetQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_dataAvailable;
    std::atomic<bool> m_endOfStream;
    
    static const size_t MAX_QUEUE_SIZE = 100;
    static const DWORD PACKET_SIZE = 188;
    
    bool CheckMediaType(const AM_MEDIA_TYPE* pmt);
    void ProcessDiscontinuity(TardsplayaFilterData& data);
};

//////////////////////////////////////////////////////////////////////////
// Simplified DirectShow Filter
//////////////////////////////////////////////////////////////////////////

class CTardsplayaDiscontinuityFilter :
    public CUnknownImpl,
    public IBaseFilter
{
public:
    CTardsplayaDiscontinuityFilter();
    virtual ~CTardsplayaDiscontinuityFilter();
    
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    
    // IPersist
    STDMETHOD(GetClassID)(CLSID* pClassID) override;
    
    // IMediaFilter
    STDMETHOD(Stop)() override;
    STDMETHOD(Pause)() override;
    STDMETHOD(Run)(REFERENCE_TIME tStart) override;
    STDMETHOD(GetState)(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    STDMETHOD(SetSyncSource)(IReferenceClock* pClock) override;
    STDMETHOD(GetSyncSource)(IReferenceClock** pClock) override;
    
    // IBaseFilter
    STDMETHOD(EnumPins)(IEnumPins** ppEnum) override;
    STDMETHOD(FindPin)(LPCWSTR Id, IPin** ppPin) override;
    STDMETHOD(QueryFilterInfo)(FILTER_INFO* pInfo) override;
    STDMETHOD(JoinFilterGraph)(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHOD(QueryVendorInfo)(LPWSTR* pVendorInfo) override;
    
    // Custom methods
    CTardsplayaSourcePin* GetSourcePin() { return m_pSourcePin; }
    bool StartCommunication(const std::wstring& pipe_name = L"\\\\.\\pipe\\TardsplayaFilter");
    void StopCommunication();
    
    // Create instance function for COM
    static HRESULT CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv);
    
private:
    CTardsplayaSourcePin* m_pSourcePin;
    CTardsplayaFilterCommunication m_communication;
    std::thread m_communicationThread;
    std::atomic<bool> m_stopRequested;
    
    IFilterGraph* m_pGraph;
    IReferenceClock* m_pClock;
    FILTER_STATE m_state;
    std::wstring m_filterName;
    
    CRITICAL_SECTION m_critSec;
    
    void CommunicationThreadProc();
    void ResetFilterState();
};

//////////////////////////////////////////////////////////////////////////
// Class Factory
//////////////////////////////////////////////////////////////////////////

class CClassFactory : public CUnknownImpl, public IClassFactory
{
public:
    CClassFactory();
    
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    
    // IClassFactory
    STDMETHOD(CreateInstance)(IUnknown* pUnkOuter, REFIID riid, void** ppv) override;
    STDMETHOD(LockServer)(BOOL fLock) override;
};

//////////////////////////////////////////////////////////////////////////
// Registration functions
//////////////////////////////////////////////////////////////////////////

HRESULT RegisterFilter();
HRESULT UnregisterFilter();
bool IsFilterRegistered();

//////////////////////////////////////////////////////////////////////////
// Global variables
//////////////////////////////////////////////////////////////////////////

extern HMODULE g_hInst;
extern LONG g_cServerLocks;