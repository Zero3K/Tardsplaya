#include "gpac_stream_thread.h"
#include "gpac_player.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>
#include <regex>
#include <chrono>
#include <thread>
#include <memory>

// External functions from other files
extern bool HttpGetText(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token);
extern std::wstring Utf8ToWide(const std::string& str);

GpacStreamThread::GpacStreamThread(
    std::shared_ptr<GpacPlayer> gpacPlayer,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count
)
    : m_gpacPlayer(gpacPlayer)
    , m_playlistUrl(playlist_url)
    , m_cancelToken(cancel_token)
    , m_logCallback(log_callback)
    , m_channelName(channel_name)
    , m_chunkCount(chunk_count)
    , m_running(false)
{
}

GpacStreamThread::~GpacStreamThread() {
    Stop();
}

bool GpacStreamThread::Start() {
    if (m_running.load()) {
        return false;
    }
    
    LogMessage(L"Starting GPAC streaming thread for " + m_channelName);
    
    m_running = true;
    m_thread = std::thread(&GpacStreamThread::StreamingLoop, this);
    
    return true;
}

void GpacStreamThread::Stop() {
    if (!m_running.load()) {
        return;
    }
    
    LogMessage(L"Stopping GPAC streaming thread for " + m_channelName);
    
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool GpacStreamThread::IsRunning() const {
    return m_running.load();
}

void GpacStreamThread::StreamingLoop() {
    LogMessage(L"GPAC streaming loop started for " + m_channelName);
    
    std::vector<std::wstring> currentSegments;
    std::vector<std::wstring> processedSegments;
    
    while (!m_cancelToken.load() && m_running.load()) {
        // Update playlist
        if (!UpdatePlaylist(currentSegments)) {
            LogMessage(L"Failed to update playlist for " + m_channelName);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        
        // Process new segments
        for (const auto& segmentUrl : currentSegments) {
            if (m_cancelToken.load() || !m_running.load()) {
                break;
            }
            
            // Skip if already processed
            bool alreadyProcessed = false;
            for (const auto& processed : processedSegments) {
                if (processed == segmentUrl) {
                    alreadyProcessed = true;
                    break;
                }
            }
            if (alreadyProcessed) {
                continue;
            }
            
            // Download segment
            std::vector<uint8_t> segmentData;
            if (!DownloadSegment(segmentUrl, segmentData)) {
                LogMessage(L"Failed to download segment: " + segmentUrl);
                continue;
            }
            
            // Check for ads and handle accordingly
            if (IsAdSegment(segmentUrl, segmentData)) {
                LogMessage(L"Ad segment detected, skipping: " + segmentUrl);
                if (m_gpacPlayer) {
                    m_gpacPlayer->ShowAdSkippingMessage(true);
                }
                // Skip ad segment
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (m_gpacPlayer) {
                    m_gpacPlayer->ShowAdSkippingMessage(false);
                }
                continue;
            }
            
            // Feed data to GPAC player
            if (!FeedDataToGpac(segmentData)) {
                LogMessage(L"Failed to feed data to GPAC player");
                HandleDiscontinuity();
                continue;
            }
            
            // Mark as processed
            processedSegments.push_back(segmentUrl);
            
            // Update chunk count
            if (m_chunkCount) {
                m_chunkCount->fetch_add(1);
            }
            
            // Limit processed segments list to prevent memory growth
            if (processedSegments.size() > 50) {
                processedSegments.erase(processedSegments.begin(), processedSegments.begin() + 25);
            }
        }
        
        // Wait before next playlist update
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    
    LogMessage(L"GPAC streaming loop ended for " + m_channelName);
    m_running = false;
}

bool GpacStreamThread::UpdatePlaylist(std::vector<std::wstring>& segments) {
    segments.clear();
    
    std::string playlistData;
    if (!HttpGetText(m_playlistUrl, playlistData, std::addressof(m_cancelToken))) {
        return false;
    }
    
    std::wstring playlistText = Utf8ToWide(playlistData);
    
    // Parse M3U8 playlist for segment URLs
    std::wregex segmentRegex(L"(https?://[^\\s]+\\.ts[^\\s]*)");
    std::wsregex_iterator iter(playlistText.begin(), playlistText.end(), segmentRegex);
    std::wsregex_iterator end;
    
    for (; iter != end; ++iter) {
        std::wstring segmentUrl = iter->str();
        segments.push_back(segmentUrl);
    }
    
    LogMessage(std::wstring(L"Updated playlist: ") + std::to_wstring(segments.size()) + L" segments for " + m_channelName);
    return !segments.empty();
}

bool GpacStreamThread::DownloadSegment(const std::wstring& segmentUrl, std::vector<uint8_t>& data) {
    data.clear();
    
    // Parse URL
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = L"", path[2048] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(segmentUrl.c_str(), 0, 0, &uc)) {
        return false;
    }

    // Download using WinHTTP
    HINTERNET hSession = WinHttpOpen(L"Tardsplaya/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
    if (!hSession) return false;
    
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { 
        WinHttpCloseHandle(hSession);
        return false; 
    }
    
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    
    // For HTTPS, ignore certificate errors for compatibility
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) {
        DWORD dwSecurityFlags = SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                               SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                               SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                               SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecurityFlags, sizeof(dwSecurityFlags));
    }
    
    BOOL res = WinHttpSendRequest(hRequest, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(hRequest, 0);
    if (!res) { 
        WinHttpCloseHandle(hRequest); 
        WinHttpCloseHandle(hConnect); 
        WinHttpCloseHandle(hSession);
        return false; 
    }

    // Read data
    DWORD dwSize = 0;
    bool success = true;
    do {
        if (m_cancelToken.load()) { 
            success = false; 
            break; 
        }
        
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            success = false;
            break;
        }
        
        if (dwSize == 0) break;
        
        std::vector<uint8_t> buffer(dwSize);
        DWORD dwDownloaded = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
            success = false;
            break;
        }
        
        if (dwDownloaded > 0) {
            data.insert(data.end(), buffer.begin(), buffer.begin() + dwDownloaded);
        }
    } while (dwSize > 0);
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    return success && !data.empty();
}

bool GpacStreamThread::FeedDataToGpac(const std::vector<uint8_t>& data) {
    if (!m_gpacPlayer || data.empty()) {
        LogMessage(L"Cannot feed data to GPAC: invalid player or empty data");
        return false;
    }
    
    LogMessage(L"Feeding " + std::to_wstring(data.size()) + L" bytes of MPEG-TS data to GPAC decoders for " + m_channelName);
    
    // Use GPAC player's MPEG-TS processing method
    bool success = m_gpacPlayer->ProcessMpegTsData(data.data(), data.size());
    
    if (success) {
        LogMessage(L"MPEG-TS data successfully processed by GPAC decoders");
    } else {
        LogMessage(L"Failed to process MPEG-TS data through GPAC decoders");
    }
    
    return success;
}

bool GpacStreamThread::IsAdSegment(const std::wstring& segmentUrl, const std::vector<uint8_t>& data) {
    // Simple ad detection based on URL patterns
    // Real implementation could analyze the actual media data
    
    // Common ad URL patterns
    if (segmentUrl.find(L"ads") != std::wstring::npos ||
        segmentUrl.find(L"commercial") != std::wstring::npos ||
        segmentUrl.find(L"preroll") != std::wstring::npos ||
        segmentUrl.find(L"midroll") != std::wstring::npos) {
        return true;
    }
    
    // Could also analyze data for SCTE-35 markers or other ad indicators
    // TODO: Implement more sophisticated ad detection
    
    return false;
}

void GpacStreamThread::HandleDiscontinuity() {
    LogMessage(L"Handling stream discontinuity for " + m_channelName);
    
    if (m_gpacPlayer) {
        m_gpacPlayer->HandleDiscontinuity();
    }
    
    // GPAC handles discontinuities better than external players
    // so this should be more robust than the original implementation
}

void GpacStreamThread::LogMessage(const std::wstring& message) {
    if (m_logCallback) {
        m_logCallback(L"[GPAC_STREAM] " + message);
    }
}

// Factory function
std::unique_ptr<GpacStreamThread> CreateGpacStreamThread(
    std::shared_ptr<GpacPlayer> gpacPlayer,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    std::function<void(const std::wstring&)> log_callback,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count
) {
    return std::make_unique<GpacStreamThread>(
        gpacPlayer, playlist_url, cancel_token, log_callback, channel_name, chunk_count
    );
}