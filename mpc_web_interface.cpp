#include "mpc_web_interface.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <thread>
#include <vector>
#include <mutex>

// Forward declaration for debug logging from main application
extern void AddDebugLog(const std::wstring& msg);

// Static members for port management
std::vector<int> MPCPortManager::used_ports_;
int MPCPortManager::next_port_ = 13579;  // Start from MPC-HC default port
std::mutex MPCPortManager::port_mutex_;

// Port management implementation
int MPCPortManager::AssignPort() {
    std::lock_guard<std::mutex> lock(port_mutex_);
    
    int assigned_port = next_port_;
    
    // Find next available port
    while (IsPortInUse(assigned_port)) {
        assigned_port++;
        // Avoid well-known ports and stay in reasonable range
        if (assigned_port > 13600) {
            assigned_port = 13579;  // Wrap around to start
        }
    }
    
    used_ports_.push_back(assigned_port);
    next_port_ = assigned_port + 1;
    
    AddDebugLog(L"[MPC-PORT] Assigned port " + std::to_wstring(assigned_port) + 
                L" (total used ports: " + std::to_wstring(used_ports_.size()) + L")");
    
    return assigned_port;
}

void MPCPortManager::ReleasePort(int port) {
    std::lock_guard<std::mutex> lock(port_mutex_);
    
    auto it = std::find(used_ports_.begin(), used_ports_.end(), port);
    if (it != used_ports_.end()) {
        used_ports_.erase(it);
        AddDebugLog(L"[MPC-PORT] Released port " + std::to_wstring(port) + 
                    L" (remaining used ports: " + std::to_wstring(used_ports_.size()) + L")");
    }
}

bool MPCPortManager::IsPortInUse(int port) {
    // No lock needed here as this is called from within locked context
    return std::find(used_ports_.begin(), used_ports_.end(), port) != used_ports_.end();
}

MPCWebInterface::MPCWebInterface() {
    // Initialize default configuration
}

MPCWebInterface::~MPCWebInterface() {
    // Release the assigned port when instance is destroyed
    if (config_.port > 0) {
        MPCPortManager::ReleasePort(config_.port);
    }
}

bool MPCWebInterface::Initialize(const WebConfig& config) {
    config_ = config;
    
    AddDebugLog(L"[MPC-WEB] Initializing web interface on " + config_.host + L":" + std::to_wstring(config_.port));
    
    // Test initial connection
    if (TestConnection()) {
        initialized_.store(true);
        available_.store(true);
        AddDebugLog(L"[MPC-WEB] Web interface successfully initialized and available");
        return true;
    } else {
        AddDebugLog(L"[MPC-WEB] Web interface initialization failed - not available yet (MPC-HC may not be running)");
        initialized_.store(false);
        available_.store(false);
        return false;
    }
}

bool MPCWebInterface::IsAvailable() const {
    // Try to initialize if not done yet (when MPC-HC becomes available)
    if (!initialized_.load()) {
        // Try to initialize now that MPC-HC might be running
        if (TestConnection()) {
            initialized_.store(true);
            available_.store(true);
            AddDebugLog(L"[MPC-WEB] Web interface successfully initialized and available on port " + 
                        std::to_wstring(config_.port));
            last_check_time_ = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    }
    
    // Check if we need to retest availability (cache for performance)
    auto now = std::chrono::steady_clock::now();
    if (now - last_check_time_ > check_interval_) {
        CheckAvailability();
        last_check_time_ = now;
    }
    
    return available_.load();
}

void MPCWebInterface::CheckAvailability() const {
    bool was_available = available_.load();
    bool now_available = TestConnection();
    
    if (was_available != now_available) {
        AddDebugLog(L"[MPC-WEB] Availability changed: " + std::wstring(now_available ? L"true" : L"false"));
    }
    
    available_.store(now_available);
}

bool MPCWebInterface::TestConnection() const {
    std::wstring response;
    bool success = SendHTTPRequest(L"/info.html", L"GET", L"", response);
    
    if (success && IsValidResponse(response)) {
        return true;
    }
    
    return false;
}

MPCWebInterface::PlayerState MPCWebInterface::GetPlayerState() const {
    if (!IsAvailable()) {
        return PlayerState::Unknown;
    }
    
    std::wstring response;
    if (SendHTTPRequest(L"/status.html", L"GET", L"", response)) {
        return ParsePlayerState(response);
    }
    
    return PlayerState::Unknown;
}

bool MPCWebInterface::HandleDiscontinuity() {
    if (!IsAvailable()) {
        AddDebugLog(L"[MPC-WEB] Cannot handle discontinuity - web interface not available");
        return false;
    }
    
    AddDebugLog(L"[MPC-WEB] Handling discontinuity - trying stream reopen approach");
    
    // Get current state
    PlayerState current_state = GetPlayerState();
    AddDebugLog(L"[MPC-WEB] Current player state before discontinuity: " + std::to_wstring(static_cast<int>(current_state)));
    
    bool success = false;
    
    // Method 1: Try reopening the current stream/pipe (most robust)
    // This is the preferred approach as it completely refreshes the stream connection
    AddDebugLog(L"[MPC-WEB] Attempting stream reopen for discontinuity recovery");
    
    auto reopen_start = std::chrono::steady_clock::now();
    if (ReopenCurrentStream()) {
        auto reopen_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - reopen_start).count();
        AddDebugLog(L"[MPC-WEB] Stream reopen completed in " + std::to_wstring(reopen_duration) + L"ms");
        
        // Allow time for stream to reopen and stabilize
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Verify the player is responsive after reopening
        PlayerState reopen_state = GetPlayerState();
        if (reopen_state == PlayerState::Playing || reopen_state == PlayerState::Loading) {
            success = true;
            AddDebugLog(L"[MPC-WEB] Discontinuity handled successfully with stream reopen");
        } else {
            AddDebugLog(L"[MPC-WEB] Stream reopen completed but player state unclear, trying fallback");
        }
    } else {
        auto reopen_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - reopen_start).count();
        AddDebugLog(L"[MPC-WEB] Stream reopen failed after " + std::to_wstring(reopen_duration) + L"ms, trying frame step fallback");
    }
    
    // Fallback 1: Frame step approach (if reopen failed)
    if (!success && (current_state == PlayerState::Playing || current_state == PlayerState::Unknown)) {
        AddDebugLog(L"[MPC-WEB] Attempting frame step for discontinuity recovery");
        
        if (FrameStep()) {
            // Brief delay to let frame step complete
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            // Check if player is still responsive and playing
            PlayerState step_state = GetPlayerState();
            if (step_state == PlayerState::Playing || step_state == PlayerState::Paused) {
                success = true;
                AddDebugLog(L"[MPC-WEB] Discontinuity handled successfully with frame step");
            } else {
                AddDebugLog(L"[MPC-WEB] Frame step didn't resolve discontinuity, trying pause/resume");
            }
        } else {
            AddDebugLog(L"[MPC-WEB] Frame step failed, trying pause/resume fallback");
        }
    }
    
    // Fallback 2: Pause/resume cycle (if frame step failed or player was paused/stopped)
    if (!success) {
        if (current_state == PlayerState::Paused || current_state == PlayerState::Stopped) {
            // If already paused/stopped, try to resume
            AddDebugLog(L"[MPC-WEB] Player paused/stopped, attempting to resume for discontinuity recovery");
            
            if (ResumePlayback() || SendPlayCommand()) {
                success = true;
                AddDebugLog(L"[MPC-WEB] Discontinuity handled by resuming paused player");
            }
        } else {
            // Try pause/resume cycle for playing state
            AddDebugLog(L"[MPC-WEB] Attempting pause/resume cycle for discontinuity recovery");
            
            if (PausePlayback()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                if (ResumePlayback()) {
                    success = true;
                    AddDebugLog(L"[MPC-WEB] Discontinuity handled with pause/resume cycle");
                } else {
                    AddDebugLog(L"[MPC-WEB] Failed to resume after discontinuity pause");
                }
            } else {
                AddDebugLog(L"[MPC-WEB] Failed to pause for discontinuity handling");
            }
        }
    }
    
    // Final fallback: Try seeking to beginning if all else failed
    if (!success) {
        AddDebugLog(L"[MPC-WEB] Attempting seek fallback for discontinuity recovery");
        
        if (SeekToBeginning()) {
            success = true;
            AddDebugLog(L"[MPC-WEB] Discontinuity handled with seek fallback");
        } else {
            AddDebugLog(L"[MPC-WEB] All discontinuity recovery methods failed");
        }
    }
    
    // Final verification
    if (success) {
        // Wait a moment and check if player is responsive
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        PlayerState final_state = GetPlayerState();
        AddDebugLog(L"[MPC-WEB] Final player state after discontinuity handling: " + std::to_wstring(static_cast<int>(final_state)));
        
        if (final_state == PlayerState::Unknown) {
            AddDebugLog(L"[MPC-WEB] Warning: Player state unknown after discontinuity handling");
            success = false;
        }
    }
    
    return success;
}

bool MPCWebInterface::ReopenCurrentStream() {
    AddDebugLog(L"[MPC-WEB] Reopening current stream/pipe (using fast timeout for discontinuity recovery)");
    // Use ID_FILE_REOPEN (976) command to reopen current file/stream
    // This completely refreshes the stream connection and resets decoder state
    std::wstring response;
    return SendHTTPCommandFast(L"wm_command=976", response); // 976 is ID_FILE_REOPEN
}

bool MPCWebInterface::PausePlayback() {
    AddDebugLog(L"[MPC-WEB] Sending pause command");
    std::wstring response;
    return SendHTTPCommandFast(L"wm_command=889", response); // 889 is pause command
}

bool MPCWebInterface::ResumePlayback() {
    AddDebugLog(L"[MPC-WEB] Sending resume/play command");
    std::wstring response;
    return SendHTTPCommandFast(L"wm_command=887", response); // 887 is play command
}

bool MPCWebInterface::FrameStep() {
    AddDebugLog(L"[MPC-WEB] Sending frame step command");
    std::wstring response;
    return SendHTTPCommandFast(L"wm_command=891", response); // 891 is ID_PLAY_FRAMESTEP
}

bool MPCWebInterface::SeekToBeginning() {
    AddDebugLog(L"[MPC-WEB] Seeking to beginning");
    std::wstring response;
    return SendHTTPCommandFast(L"wm_command=996", response); // 996 is go to beginning
}

bool MPCWebInterface::RefreshStream() {
    AddDebugLog(L"[MPC-WEB] Refreshing stream");
    // Send refresh/reload command if available
    std::wstring response;
    return SendHTTPCommand(L"wm_command=919", response); // 919 is reload
}

bool MPCWebInterface::SendPlayCommand() {
    std::wstring response;
    return SendHTTPCommand(L"wm_command=887", response); // 887 is play
}

bool MPCWebInterface::SendPauseCommand() {
    std::wstring response;
    return SendHTTPCommand(L"wm_command=889", response); // 889 is pause
}

bool MPCWebInterface::SendStopCommand() {
    std::wstring response;
    return SendHTTPCommand(L"wm_command=890", response); // 890 is stop
}

bool MPCWebInterface::SendCloseCommand() {
    std::wstring response;
    return SendHTTPCommand(L"wm_command=816", response); // 816 is close
}

bool MPCWebInterface::OpenURL(const std::wstring& url) {
    std::wstring encoded_url = UrlEncode(url);
    std::wstring command = L"wm_command=800&filename=" + encoded_url; // 800 is open file
    std::wstring response;
    return SendHTTPCommand(command, response);
}

bool MPCWebInterface::GetPosition(double& position) const {
    std::wstring response;
    if (SendHTTPRequest(L"/status.html", L"GET", L"", response)) {
        return ParsePosition(response, position);
    }
    return false;
}

bool MPCWebInterface::GetDuration(double& duration) const {
    std::wstring response;
    if (SendHTTPRequest(L"/status.html", L"GET", L"", response)) {
        return ParseDuration(response, duration);
    }
    return false;
}

bool MPCWebInterface::SendHTTPCommand(const std::wstring& command, std::wstring& response) const {
    std::wstring path = L"/command.html?" + command;
    return SendHTTPRequest(path, L"GET", L"", response);
}

bool MPCWebInterface::SendHTTPCommandFast(const std::wstring& command, std::wstring& response) const {
    std::wstring path = L"/command.html?" + command;
    return SendHTTPRequestFast(path, L"GET", L"", response);
}

bool MPCWebInterface::SendHTTPRequest(const std::wstring& path, const std::wstring& method, 
                                     const std::wstring& data, std::wstring& response) const {
    for (int attempt = 0; attempt < config_.max_retries; ++attempt) {
        HINTERNET hSession = WinHttpOpen(L"Tardsplaya-MPC/1.0", 
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 
                                        WINHTTP_NO_PROXY_NAME, 
                                        WINHTTP_NO_PROXY_BYPASS, 
                                        0);
        if (!hSession) {
            if (attempt < config_.max_retries - 1) {
                std::this_thread::sleep_for(config_.retry_delay);
                continue;
            }
            return false;
        }
        
        HINTERNET hConnect = WinHttpConnect(hSession, config_.host.c_str(), config_.port, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            if (attempt < config_.max_retries - 1) {
                std::this_thread::sleep_for(config_.retry_delay);
                continue;
            }
            return false;
        }
        
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, 
                                               method.c_str(), 
                                               path.c_str(),
                                               NULL, 
                                               WINHTTP_NO_REFERER, 
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, 
                                               0);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (attempt < config_.max_retries - 1) {
                std::this_thread::sleep_for(config_.retry_delay);
                continue;
            }
            return false;
        }
        
        // Set timeout
        DWORD timeout = static_cast<DWORD>(config_.timeout.count());
        WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        
        // Send request
        BOOL bResult = FALSE;
        if (data.empty()) {
            bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, 
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        } else {
            std::string utf8_data;
            int len = WideCharToMultiByte(CP_UTF8, 0, data.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                utf8_data.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, data.c_str(), -1, &utf8_data[0], len, nullptr, nullptr);
            }
            
            bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        (LPVOID)utf8_data.c_str(), (DWORD)utf8_data.length(), 
                                        (DWORD)utf8_data.length(), 0);
        }
        
        if (bResult) {
            bResult = WinHttpReceiveResponse(hRequest, NULL);
        }
        
        if (bResult) {
            DWORD dwSize = 0;
            std::string response_data;
            
            do {
                DWORD dwDownloaded = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
                    break;
                }
                
                if (dwSize == 0) {
                    break;
                }
                
                std::vector<char> buffer(dwSize + 1);
                if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
                    break;
                }
                
                buffer[dwDownloaded] = '\0';
                response_data.append(buffer.data(), dwDownloaded);
            } while (dwSize > 0);
            
            // Convert to wide string
            if (!response_data.empty()) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, response_data.c_str(), -1, nullptr, 0);
                if (wlen > 0) {
                    response.resize(wlen - 1);
                    MultiByteToWideChar(CP_UTF8, 0, response_data.c_str(), -1, &response[0], wlen);
                }
            }
            
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return true;
        }
        
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        
        if (attempt < config_.max_retries - 1) {
            std::this_thread::sleep_for(config_.retry_delay);
        }
    }
    
    return false;
}

bool MPCWebInterface::SendHTTPRequestFast(const std::wstring& path, const std::wstring& method, 
                                         const std::wstring& data, std::wstring& response) const {
    // Fast configuration for discontinuity recovery - single attempt with very short timeout
    const int fast_max_retries = 1;
    const std::chrono::milliseconds fast_timeout{800}; // 800ms timeout for very fast response
    const std::chrono::milliseconds fast_retry_delay{50}; // Minimal retry delay
    
    for (int attempt = 0; attempt < fast_max_retries; ++attempt) {
        HINTERNET hSession = WinHttpOpen(L"Tardsplaya-MPC/1.0", 
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 
                                        WINHTTP_NO_PROXY_NAME, 
                                        WINHTTP_NO_PROXY_BYPASS, 
                                        0);
        if (!hSession) {
            if (attempt < fast_max_retries - 1) {
                std::this_thread::sleep_for(fast_retry_delay);
                continue;
            }
            return false;
        }
        
        HINTERNET hConnect = WinHttpConnect(hSession, config_.host.c_str(), config_.port, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            if (attempt < fast_max_retries - 1) {
                std::this_thread::sleep_for(fast_retry_delay);
                continue;
            }
            return false;
        }
        
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, 
                                               method.c_str(), 
                                               path.c_str(),
                                               NULL, 
                                               WINHTTP_NO_REFERER, 
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, 
                                               0);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (attempt < fast_max_retries - 1) {
                std::this_thread::sleep_for(fast_retry_delay);
                continue;
            }
            return false;
        }
        
        // Set fast timeout for discontinuity recovery
        DWORD timeout = static_cast<DWORD>(fast_timeout.count());
        WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        
        // Send request
        BOOL bResult = FALSE;
        if (data.empty()) {
            bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, 
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        } else {
            std::string utf8_data;
            int len = WideCharToMultiByte(CP_UTF8, 0, data.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                utf8_data.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, data.c_str(), -1, &utf8_data[0], len, nullptr, nullptr);
            }
            
            bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        (LPVOID)utf8_data.c_str(), (DWORD)utf8_data.length(), 
                                        (DWORD)utf8_data.length(), 0);
        }
        
        if (bResult) {
            bResult = WinHttpReceiveResponse(hRequest, NULL);
        }
        
        if (bResult) {
            DWORD dwSize = 0;
            std::string response_data;
            
            do {
                DWORD dwDownloaded = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
                    break;
                }
                
                if (dwSize == 0) {
                    break;
                }
                
                std::vector<char> buffer(dwSize + 1);
                if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
                    break;
                }
                
                buffer[dwDownloaded] = '\0';
                response_data.append(buffer.data(), dwDownloaded);
            } while (dwSize > 0);
            
            // Convert to wide string
            if (!response_data.empty()) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, response_data.c_str(), -1, nullptr, 0);
                if (wlen > 0) {
                    response.resize(wlen - 1);
                    MultiByteToWideChar(CP_UTF8, 0, response_data.c_str(), -1, &response[0], wlen);
                }
            }
            
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return true;
        }
        
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        
        if (attempt < fast_max_retries - 1) {
            std::this_thread::sleep_for(fast_retry_delay);
        }
    }
    
    return false;
}

MPCWebInterface::PlayerState MPCWebInterface::ParsePlayerState(const std::wstring& response) const {
    if (response.empty()) {
        return PlayerState::Unknown;
    }
    
    // Look for state indicators in the HTML response
    std::wstring lower_response = response;
    std::transform(lower_response.begin(), lower_response.end(), lower_response.begin(), ::towlower);
    
    if (lower_response.find(L"playing") != std::wstring::npos) {
        return PlayerState::Playing;
    } else if (lower_response.find(L"paused") != std::wstring::npos) {
        return PlayerState::Paused;
    } else if (lower_response.find(L"stopped") != std::wstring::npos) {
        return PlayerState::Stopped;
    } else if (lower_response.find(L"loading") != std::wstring::npos || 
               lower_response.find(L"buffering") != std::wstring::npos) {
        return PlayerState::Loading;
    }
    
    return PlayerState::Unknown;
}

bool MPCWebInterface::ParsePosition(const std::wstring& response, double& position) const {
    // This would need to be implemented based on MPC-HC's actual HTML response format
    // For now, return false as a placeholder
    position = 0.0;
    return false;
}

bool MPCWebInterface::ParseDuration(const std::wstring& response, double& duration) const {
    // This would need to be implemented based on MPC-HC's actual HTML response format
    // For now, return false as a placeholder
    duration = 0.0;
    return false;
}

std::wstring MPCWebInterface::UrlEncode(const std::wstring& text) const {
    std::wostringstream encoded;
    encoded << std::hex << std::uppercase;
    
    for (wchar_t c : text) {
        if (isalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'~') {
            encoded << c;
        } else {
            encoded << L'%' << std::setw(2) << std::setfill(L'0') << static_cast<unsigned int>(c);
        }
    }
    
    return encoded.str();
}

bool MPCWebInterface::IsValidResponse(const std::wstring& response) const {
    if (response.empty()) {
        return false;
    }
    
    // Check for HTML content or known MPC-HC response patterns
    std::wstring lower_response = response;
    std::transform(lower_response.begin(), lower_response.end(), lower_response.begin(), ::towlower);
    
    return (lower_response.find(L"<html") != std::wstring::npos ||
            lower_response.find(L"mpc-hc") != std::wstring::npos ||
            lower_response.find(L"media player classic") != std::wstring::npos);
}

// Factory function implementation
std::unique_ptr<MPCWebInterface> CreateMPCWebInterface(const std::wstring& player_path, int port) {
    if (!IsMPCHC(player_path)) {
        return nullptr;
    }
    
    auto web_interface = std::make_unique<MPCWebInterface>();
    
    // Assign port if not specified
    int assigned_port = (port > 0) ? port : MPCPortManager::AssignPort();
    
    // Configure with assigned port
    MPCWebInterface::WebConfig config;
    config.port = assigned_port;
    
    AddDebugLog(L"[MPC-WEB] Creating web interface for port " + std::to_wstring(assigned_port));
    
    // Set the configuration but don't require immediate initialization success
    // The web interface will be initialized later when MPC-HC starts
    web_interface->SetConfig(config);
    
    AddDebugLog(L"[MPC-WEB] Created MPC-HC web interface with assigned port " + 
                std::to_wstring(assigned_port) + L" (will initialize when MPC-HC starts)");
    
    return web_interface;
}

bool IsMPCHC(const std::wstring& player_path) {
    std::wstring lower_path = player_path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::towlower);
    
    return (lower_path.find(L"mpc-hc") != std::wstring::npos ||
            lower_path.find(L"mpc_hc") != std::wstring::npos ||
            lower_path.find(L"mplayerc") != std::wstring::npos);
}