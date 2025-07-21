// Tardsplaya DirectShow Filter Integration Implementation
// Communication between main app and DirectShow filter

#include "directshow_integration.h"
#include "filter_guids.h"
#include <shlwapi.h>
#include <process.h>
#include <fstream>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")

namespace tardsplaya_filter {

//////////////////////////////////////////////////////////////////////////
// FilterCommunication Implementation
//////////////////////////////////////////////////////////////////////////

FilterCommunication::FilterCommunication()
    : pipe_handle_(INVALID_HANDLE_VALUE)
    , pipe_connected_(false)
    , pipe_name_(L"\\\\.\\pipe\\TardsplayaFilter")
{
}

FilterCommunication::~FilterCommunication()
{
    Cleanup();
}

bool FilterCommunication::Initialize(const std::wstring& pipe_name)
{
    pipe_name_ = pipe_name;
    
    // Create named pipe client
    pipe_handle_ = CreateFile(
        pipe_name_.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    
    return pipe_handle_ != INVALID_HANDLE_VALUE;
}

void FilterCommunication::Cleanup()
{
    pipe_connected_ = false;
    
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }
}

bool FilterCommunication::ConnectToFilter(DWORD timeout_ms)
{
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        // Try to connect to existing pipe
        if (!Initialize(pipe_name_)) {
            return false;
        }
    }
    
    // Wait for pipe to be available
    if (WaitNamedPipe(pipe_name_.c_str(), timeout_ms)) {
        pipe_connected_ = true;
        return true;
    }
    
    return false;
}

bool FilterCommunication::SendPacketData(const FilterData& data, DWORD timeout_ms)
{
    if (!pipe_connected_ || pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    DWORD bytesWritten = 0;
    BOOL success = WriteFile(
        pipe_handle_,
        &data,
        sizeof(FilterData),
        &bytesWritten,
        nullptr
    );
    
    if (!success || bytesWritten != sizeof(FilterData)) {
        pipe_connected_ = false;
        return false;
    }
    
    return true;
}

bool FilterCommunication::SendEndOfStream()
{
    FilterData eosData;
    eosData.end_of_stream = true;
    return SendPacketData(eosData);
}

bool FilterCommunication::IsFilterRegistered()
{
    // Check if our filter is registered by looking in registry
    HKEY hkey = nullptr;
    
    WCHAR clsidStr[64];
    StringFromGUID2(CLSID_TardsplayaDiscontinuityFilter, clsidStr, 64);
    
    WCHAR keyPath[256];
    swprintf_s(keyPath, L"CLSID\\%s", clsidStr);
    
    LONG result = RegOpenKeyEx(HKEY_CLASSES_ROOT, keyPath, 0, KEY_READ, &hkey);
    
    if (result == ERROR_SUCCESS) {
        RegCloseKey(hkey);
        return true;
    }
    
    return false;
}

bool FilterCommunication::RegisterFilter()
{
    // Find our filter DLL
    WCHAR dllPath[MAX_PATH];
    GetModuleFileName(nullptr, dllPath, MAX_PATH);
    
    // Replace .exe with Filter.dll
    PathRemoveFileSpec(dllPath);
    PathAppend(dllPath, L"TardsplayaFilter.dll");
    
    if (!PathFileExists(dllPath)) {
        return false;
    }
    
    // Register the DLL
    HMODULE hmod = LoadLibrary(dllPath);
    if (!hmod) {
        return false;
    }
    
    typedef HRESULT (WINAPI *DllRegisterServerProc)();
    DllRegisterServerProc pfnDllRegisterServer = 
        (DllRegisterServerProc)GetProcAddress(hmod, "DllRegisterServer");
    
    HRESULT hr = E_FAIL;
    if (pfnDllRegisterServer) {
        hr = pfnDllRegisterServer();
    }
    
    FreeLibrary(hmod);
    return SUCCEEDED(hr);
}

bool FilterCommunication::UnregisterFilter()
{
    // Find our filter DLL
    WCHAR dllPath[MAX_PATH];
    GetModuleFileName(nullptr, dllPath, MAX_PATH);
    
    PathRemoveFileSpec(dllPath);
    PathAppend(dllPath, L"TardsplayaFilter.dll");
    
    if (!PathFileExists(dllPath)) {
        return false;
    }
    
    // Unregister the DLL
    HMODULE hmod = LoadLibrary(dllPath);
    if (!hmod) {
        return false;
    }
    
    typedef HRESULT (WINAPI *DllUnregisterServerProc)();
    DllUnregisterServerProc pfnDllUnregisterServer = 
        (DllUnregisterServerProc)GetProcAddress(hmod, "DllUnregisterServer");
    
    HRESULT hr = E_FAIL;
    if (pfnDllUnregisterServer) {
        hr = pfnDllUnregisterServer();
    }
    
    FreeLibrary(hmod);
    return SUCCEEDED(hr);
}

//////////////////////////////////////////////////////////////////////////
// FilterStreamManager Implementation
//////////////////////////////////////////////////////////////////////////

FilterStreamManager::FilterStreamManager()
    : filter_active_(false)
    , stop_requested_(false)
    , pipe_name_(L"\\\\.\\pipe\\TardsplayaFilter")
{
    filter_comm_ = std::make_unique<FilterCommunication>();
    ts_router_ = std::make_unique<tsduck_transport::TransportStreamRouter>();
    current_stats_.start_time = std::chrono::steady_clock::now();
}

FilterStreamManager::~FilterStreamManager()
{
    StopFilterStream();
}

bool FilterStreamManager::StartFilterStream(
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    const std::wstring& channel_name)
{
    if (filter_active_) {
        return false; // Already active
    }
    
    log_callback_ = log_callback;
    
    // Check if filter is registered
    if (!FilterCommunication::IsFilterRegistered()) {
        if (log_callback_) {
            log_callback_(L"DirectShow filter not registered. Attempting to register...");
        }
        
        if (!FilterCommunication::RegisterFilter()) {
            if (log_callback_) {
                log_callback_(L"Failed to register DirectShow filter. Filter functionality will not be available.");
            }
            return false;
        }
        
        if (log_callback_) {
            log_callback_(L"DirectShow filter registered successfully.");
        }
    }
    
    // Initialize communication
    if (!filter_comm_->Initialize(pipe_name_)) {
        if (log_callback_) {
            log_callback_(L"Failed to initialize DirectShow filter communication.");
        }
        return false;
    }
    
    // Start filter thread
    stop_requested_ = false;
    filter_thread_ = std::thread(&FilterStreamManager::FilterStreamThread, this, 
                                playlist_url, std::ref(cancel_token), channel_name);
    
    filter_active_ = true;
    
    if (log_callback_) {
        log_callback_(L"DirectShow filter stream started. Filter is now available for MPC-HC and other DirectShow players.");
    }
    
    return true;
}

void FilterStreamManager::StopFilterStream()
{
    if (!filter_active_) {
        return;
    }
    
    stop_requested_ = true;
    
    if (filter_thread_.joinable()) {
        filter_thread_.join();
    }
    
    if (filter_comm_) {
        filter_comm_->SendEndOfStream();
        filter_comm_->Cleanup();
    }
    
    filter_active_ = false;
    
    if (log_callback_) {
        log_callback_(L"DirectShow filter stream stopped.");
    }
}

FilterStreamManager::FilterStats FilterStreamManager::GetFilterStats() const
{
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    FilterStats stats = current_stats_;
    stats.filter_connected = filter_comm_->IsConnected();
    
    return stats;
}

void FilterStreamManager::FilterStreamThread(
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    const std::wstring& channel_name)
{
    if (log_callback_) {
        log_callback_(L"DirectShow filter thread started for channel: " + channel_name);
    }
    
    // Configure transport stream router for DirectShow output
    tsduck_transport::TransportStreamRouter::RouterConfig config;
    config.player_path = L""; // No direct player - output via DirectShow filter
    config.buffer_size_packets = 5000;
    config.low_latency_mode = true;
    config.enable_pat_pmt_repetition = true;
    
    // Wait for filter connection
    bool filter_connected = false;
    for (int i = 0; i < 50 && !stop_requested_ && !cancel_token; ++i) {
        if (filter_comm_->ConnectToFilter(100)) {
            filter_connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!filter_connected) {
        if (log_callback_) {
            log_callback_(L"DirectShow filter not connected. No DirectShow player is using the filter.");
        }
        return;
    }
    
    if (log_callback_) {
        log_callback_(L"DirectShow filter connected. Streaming data to filter...");
    }
    
    // Create a custom HLS fetcher that sends data to DirectShow filter
    // This is a simplified implementation - in reality, we'd integrate more deeply
    // with the existing transport stream router
    
    DWORD stream_id = GetCurrentThreadId(); // Simple stream ID
    uint64_t packet_count = 0;
    
    while (!stop_requested_ && !cancel_token && filter_comm_->IsConnected()) {
        // For demonstration, create a simple transport stream packet
        // In a real implementation, this would come from the HLS stream processing
        
        FilterData filterData;
        filterData.stream_id = stream_id;
        filterData.timestamp = GetTickCount64() * 10000; // Convert to 100ns units
        filterData.discontinuity_detected = false;
        
        // Create a basic transport stream packet (placeholder)
        tsduck_transport::TSPacket packet;
        packet.data[0] = 0x47; // TS sync byte
        packet.timestamp = std::chrono::steady_clock::now();
        packet.frame_number = packet_count++;
        
        filterData.packet = packet;
        
        if (filter_comm_->SendPacketData(filterData)) {
            UpdateStats(filterData);
        } else {
            if (log_callback_) {
                log_callback_(L"Lost connection to DirectShow filter.");
            }
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(40)); // ~25fps
    }
    
    // Send end of stream
    filter_comm_->SendEndOfStream();
    
    if (log_callback_) {
        log_callback_(L"DirectShow filter thread finished.");
    }
}

FilterData FilterStreamManager::ConvertTSPacketToFilterData(const tsduck_transport::TSPacket& packet, DWORD stream_id)
{
    FilterData data;
    data.packet = packet;
    data.stream_id = stream_id;
    data.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        packet.timestamp.time_since_epoch()).count() / 100; // Convert to 100ns units
    data.discontinuity_detected = packet.discontinuity;
    data.end_of_stream = false;
    
    return data;
}

void FilterStreamManager::HandleDiscontinuity(FilterData& data)
{
    data.discontinuity_detected = true;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    current_stats_.discontinuities_handled++;
}

void FilterStreamManager::UpdateStats(const FilterData& data)
{
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    current_stats_.packets_sent++;
    current_stats_.frames_processed = data.packet.frame_number;
    
    if (data.discontinuity_detected) {
        current_stats_.discontinuities_handled++;
    }
}

//////////////////////////////////////////////////////////////////////////
// Helper Functions
//////////////////////////////////////////////////////////////////////////

bool IsDirectShowPlayerCompatible()
{
    // Check if DirectShow is available on the system
    HMODULE quartz = LoadLibrary(L"quartz.dll");
    if (quartz) {
        FreeLibrary(quartz);
        return true;
    }
    return false;
}

std::vector<std::wstring> GetCompatibleDirectShowPlayers()
{
    std::vector<std::wstring> players;
    
    // Common DirectShow-based media players
    const std::wstring commonPaths[] = {
        L"C:\\Program Files\\MPC-HC\\mpc-hc64.exe",
        L"C:\\Program Files (x86)\\MPC-HC\\mpc-hc.exe",
        L"C:\\Program Files\\MPC-BE\\mpc-be64.exe",
        L"C:\\Program Files (x86)\\MPC-BE\\mpc-be.exe",
        L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe",
        L"C:\\Program Files (x86)\\VideoLAN\\VLC\\vlc.exe"
    };
    
    for (const auto& path : commonPaths) {
        if (PathFileExists(path.c_str())) {
            players.push_back(path);
        }
    }
    
    return players;
}

bool LaunchMPCHCWithFilter(const std::wstring& mpc_path)
{
    std::wstring actualPath = mpc_path;
    
    if (actualPath.empty()) {
        // Try to find MPC-HC automatically
        auto players = GetCompatibleDirectShowPlayers();
        for (const auto& player : players) {
            if (player.find(L"mpc-hc") != std::wstring::npos) {
                actualPath = player;
                break;
            }
        }
    }
    
    if (actualPath.empty() || !PathFileExists(actualPath.c_str())) {
        return false;
    }
    
    // Launch MPC-HC
    // Note: MPC-HC would need to be configured to use our DirectShow filter
    SHELLEXECUTEINFO sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = actualPath.c_str();
    sei.nShow = SW_SHOWNORMAL;
    
    return ShellExecuteEx(&sei);
}

std::wstring GetMPCHCConfigurationInstructions()
{
    std::wstringstream instructions;
    
    instructions << L"DirectShow Filter Configuration Instructions:\n\n";
    instructions << L"1. Register the Tardsplaya DirectShow Filter:\n";
    instructions << L"   - The filter should be automatically registered when you start DirectShow streaming\n";
    instructions << L"   - Or manually register: regsvr32 TardsplayaFilter.dll\n\n";
    
    instructions << L"2. Configure MPC-HC to use the filter:\n";
    instructions << L"   a) Open MPC-HC\n";
    instructions << L"   b) Go to View → Options → External Filters\n";
    instructions << L"   c) Click 'Add Filter...'\n";
    instructions << L"   d) Find 'Tardsplaya Discontinuity Handler' in the list\n";
    instructions << L"   e) Click OK and set Priority to 'Prefer'\n\n";
    
    instructions << L"3. Using the filter:\n";
    instructions << L"   a) Start DirectShow streaming in Tardsplaya\n";
    instructions << L"   b) In MPC-HC, the filter will appear as a source\n";
    instructions << L"   c) The filter will automatically handle stream discontinuities\n\n";
    
    instructions << L"4. Verifying the filter is working:\n";
    instructions << L"   a) Check Tardsplaya log for 'DirectShow filter connected' message\n";
    instructions << L"   b) In MPC-HC, go to View → Filters to see active filters\n";
    instructions << L"   c) 'Tardsplaya Discontinuity Handler' should be listed\n\n";
    
    instructions << L"Benefits:\n";
    instructions << L"- Automatic discontinuity detection and correction\n";
    instructions << L"- Frame number tagging for lag reduction\n";
    instructions << L"- Real-time stream health monitoring\n";
    instructions << L"- Professional transport stream format output\n";
    
    return instructions.str();
}

} // namespace tardsplaya_filter