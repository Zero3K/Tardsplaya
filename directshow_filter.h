#pragma once
// DirectShow Filter for Tardsplaya Discontinuity Handling
// Implements a DirectShow source filter that MPC-HC and other players can use

#include <windows.h>
#include <dshow.h>
#include <streams.h>
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

// Shared memory/pipe communication interface
class CTardsplayaFilterCommunication {
public:
    CTardsplayaFilterCommunication();
    ~CTardsplayaFilterCommunication();
    
    // Initialize communication (named pipe)
    bool Initialize(const std::wstring& pipe_name = L"\\\\.\\pipe\\TardsplayaFilter");
    
    // Cleanup communication
    void Cleanup();
    
    // Wait for connection from Tardsplaya main app
    bool WaitForConnection(DWORD timeout_ms = 5000);
    
    // Read packet data from Tardsplaya
    bool ReadPacketData(TardsplayaFilterData& data, DWORD timeout_ms = 1000);
    
    // Check if connected
    bool IsConnected() const { return pipe_connected_; }
    
private:
    HANDLE pipe_handle_;
    std::atomic<bool> pipe_connected_;
    std::wstring pipe_name_;
};

// DirectShow Source Pin - outputs transport stream data
class CTardsplayaSourcePin : public CSourceStream {
public:
    CTardsplayaSourcePin(HRESULT* phr, CTardsplayaDiscontinuityFilter* pFilter);
    virtual ~CTardsplayaSourcePin();
    
    // CSourceStream overrides
    HRESULT FillBuffer(IMediaSample* pSample) override;
    HRESULT DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pProperties) override;
    HRESULT CheckMediaType(const CMediaType* pMediaType) override;
    HRESULT GetMediaType(int iPosition, CMediaType* pMediaType) override;
    HRESULT SetMediaType(const CMediaType* pMediaType) override;
    
    // Quality control
    HRESULT Notify(IBaseFilter* pSender, Quality q) override;
    
    // Custom methods
    bool HasDataAvailable();
    void SignalEndOfStream();
    void ResetStreamState();
    
private:
    CTardsplayaDiscontinuityFilter* m_pFilter;
    CCritSec m_cSharedState;
    std::queue<TardsplayaFilterData> m_packetQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_dataAvailable;
    std::atomic<bool> m_endOfStream;
    std::atomic<LONGLONG> m_currentTimestamp;
    
    // Discontinuity handling
    LONGLONG m_lastTimestamp;
    DWORD m_lastStreamId;
    std::atomic<bool> m_discontinuityDetected;
    
    // Buffer management
    static const size_t MAX_QUEUE_SIZE = 100; // Max packets to queue
    static const DWORD PACKET_SIZE = 188;     // TS packet size
    
    // Helper methods
    void ProcessDiscontinuity(TardsplayaFilterData& data);
    bool QueuePacketData(const TardsplayaFilterData& data);
    bool DequeuePacketData(TardsplayaFilterData& data);
    
    friend class CTardsplayaDiscontinuityFilter;
};

// DirectShow Filter - main filter implementation
class CTardsplayaDiscontinuityFilter : public CSource {
public:
    CTardsplayaDiscontinuityFilter(LPUNKNOWN pUnk, HRESULT* phr);
    virtual ~CTardsplayaDiscontinuityFilter();
    
    // CSource overrides
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Stop() override;
    
    // IUnknown
    DECLARE_IUNKNOWN;
    
    // Create instance function for COM
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT* phr);
    
    // Filter information
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    
    // Custom methods
    CTardsplayaSourcePin* GetSourcePin() { return m_pSourcePin; }
    bool IsReceivingData() const { return m_communication.IsConnected(); }
    
    // Communication management
    bool StartCommunication(const std::wstring& pipe_name = L"\\\\.\\pipe\\TardsplayaFilter");
    void StopCommunication();
    
private:
    CTardsplayaSourcePin* m_pSourcePin;
    CTardsplayaFilterCommunication m_communication;
    std::thread m_communicationThread;
    std::atomic<bool> m_stopRequested;
    
    // Communication thread function
    void CommunicationThreadProc();
    
    // Helper methods
    void ResetFilterState();
    
    CCritSec m_filterLock;
};

// Filter factory template
class CTardsplayaDiscontinuityFilterTemplate : public CFactoryTemplate {
public:
    static const CFactoryTemplate g_Template;
};

// Registration helper functions
HRESULT RegisterFilter();
HRESULT UnregisterFilter();
bool IsFilterRegistered();