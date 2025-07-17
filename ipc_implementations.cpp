#include "ipc_implementations.h"
#include "stream_pipe.h"
#include <windows.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>
#include <set>
#include <sstream>
#include <memory>

// Global configuration for IPC method selection
IPCMethod g_current_ipc_method = IPCMethod::ANONYMOUS_PIPES;

// Forward declarations from stream_pipe.cpp
extern void AddDebugLog(const std::wstring& message);
extern bool HttpGetText(const std::wstring& url, std::string& out);
extern std::wstring JoinUrl(const std::wstring& base_url, const std::wstring& relative_url);
extern bool ProcessStillRunning(HANDLE process_handle, const std::wstring& context, DWORD pid);
extern void SetPlayerWindowTitle(DWORD process_id, const std::wstring& channel_name);

// Wrapper function to handle cancel token parameter by ignoring it for now
bool HttpGetTextWithCancel(const std::wstring& url, std::string& out, std::atomic<bool>* cancel_token) {
    // For now, ignore the cancel token and use the global function
    // In a full implementation, we would check cancel_token periodically
    if (cancel_token && cancel_token->load()) {
        return false;
    }
    return HttpGetText(url, out);
}

// Define WriteFileWithTimeout if not available
BOOL WriteFileWithTimeout(HANDLE handle, const void* buffer, DWORD bytes_to_write, DWORD* bytes_written, DWORD timeout_ms) {
    // For simplicity, use regular WriteFile for now
    return WriteFile(handle, buffer, bytes_to_write, bytes_written, nullptr);
}

static std::atomic<int> g_mailslot_counter(0);
static std::atomic<int> g_namedpipe_counter(0);

HANDLE CreateMailSlotBridge(const std::wstring& mailslot_name, HANDLE& stdin_write_handle) {
    // Create a bridge process that reads from MailSlot and writes to stdin
    // For now, we'll create a pipe and simulate the bridge functionality
    
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const DWORD PIPE_BUFFER_SIZE = 1024 * 1024;
    
    if (!CreatePipe(&hRead, &hWrite, &sa, PIPE_BUFFER_SIZE)) {
        return INVALID_HANDLE_VALUE;
    }
    
    stdin_write_handle = hWrite;
    return hRead; // This will be used as the process stdin
}

bool SendVideoSegmentViaMailSlot(HANDLE mailslot_client, const std::vector<char>& segment_data) {
    // For actual MailSlot implementation, we send the entire segment in one message
    // Since individual mailslots can handle large messages up to the size specified when creating
    
    DWORD bytes_written = 0;
    BOOL success = WriteFile(
        mailslot_client,
        segment_data.data(),
        (DWORD)segment_data.size(),
        &bytes_written,
        nullptr
    );
    
    return success && bytes_written == segment_data.size();
}

HANDLE CreateStreamingNamedPipe(const std::wstring& pipe_name, HANDLE& client_handle) {
    // Create named pipe server
    HANDLE server_handle = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1, // Only one instance
        1024 * 1024, // 1MB buffer
        1024 * 1024,
        0,
        nullptr
    );
    
    if (server_handle == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    
    // Create client handle for the child process
    client_handle = CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (client_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(server_handle);
        return INVALID_HANDLE_VALUE;
    }
    
    return server_handle;
}

bool BufferAndMailSlotStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
) {
    AddDebugLog(L"[IPC-METHOD] MailSlot implementation starting for " + channel_name);
    
    // Generate unique MailSlot name
    int mailslot_id = ++g_mailslot_counter;
    std::wstring mailslot_name = L"\\\\.\\mailslot\\tardsplaya_" + std::to_wstring(mailslot_id);
    
    // Create MailSlot server
    HANDLE mailslot_server = CreateMailslotW(
        mailslot_name.c_str(),
        10 * 1024 * 1024, // 10MB max message size - large enough for video segments
        MAILSLOT_WAIT_FOREVER,
        nullptr
    );
    
    if (mailslot_server == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to create MailSlot server, error=" + std::to_wstring(error));
        return false;
    }
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Created MailSlot server: " + mailslot_name);
    
    // Create bridge process - for simplicity, we'll use pipes to simulate the bridge
    HANDLE bridge_stdin;
    HANDLE player_stdin = CreateMailSlotBridge(mailslot_name, bridge_stdin);
    
    if (player_stdin == INVALID_HANDLE_VALUE) {
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to create MailSlot bridge");
        CloseHandle(mailslot_server);
        return false;
    }
    
    // Create media player process
    std::wstring cmd;
    if (player_path.find(L"mpc-hc") != std::wstring::npos) {
        cmd = L"\"" + player_path + L"\" - /new /nofocus";
    } else if (player_path.find(L"vlc") != std::wstring::npos) {
        cmd = L"\"" + player_path + L"\" - --intf dummy --no-one-instance";
    } else {
        cmd = L"\"" + player_path + L"\" -";
    }
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdInput = player_stdin;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Launching player with MailSlot bridge: " + cmd);
    
    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, TRUE,
        CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi
    );
    
    CloseHandle(player_stdin); // Child process now owns it
    
    if (!success) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to create process, error=" + std::to_wstring(error));
        CloseHandle(bridge_stdin);
        CloseHandle(mailslot_server);
        return false;
    }
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Process created, PID=" + std::to_wstring(pi.dwProcessId));
    
    // Start bridge thread that reads from MailSlot and writes to pipe
    std::thread bridge_thread([mailslot_server, bridge_stdin, channel_name]() {
        AddDebugLog(L"[BRIDGE] MailSlot bridge thread started for " + channel_name);
        
        // Use heap allocation instead of stack to prevent stack overflow
        const size_t BUFFER_SIZE = 10 * 1024 * 1024; // 10MB buffer
        std::unique_ptr<char[]> buffer = std::make_unique<char[]>(BUFFER_SIZE);
        DWORD bytes_read, bytes_written;
        
        while (true) {
            if (ReadFile(mailslot_server, buffer.get(), (DWORD)BUFFER_SIZE, &bytes_read, nullptr)) {
                if (bytes_read > 0) {
                    AddDebugLog(L"[BRIDGE] Read " + std::to_wstring(bytes_read) + L" bytes from MailSlot");
                    if (!WriteFile(bridge_stdin, buffer.get(), bytes_read, &bytes_written, nullptr)) {
                        AddDebugLog(L"[BRIDGE] Failed to write to bridge pipe");
                        break;
                    }
                    AddDebugLog(L"[BRIDGE] Wrote " + std::to_wstring(bytes_written) + L" bytes to player");
                }
            } else {
                DWORD error = GetLastError();
                if (error != ERROR_SEM_TIMEOUT) {
                    AddDebugLog(L"[BRIDGE] MailSlot read error: " + std::to_wstring(error));
                    break;
                }
            }
        }
        
        CloseHandle(bridge_stdin);
        AddDebugLog(L"[BRIDGE] MailSlot bridge thread ended for " + channel_name);
    });
    bridge_thread.detach();
    
    // Create MailSlot client for sending data
    HANDLE mailslot_client = CreateFileW(
        mailslot_name.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (mailslot_client == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to create MailSlot client, error=" + std::to_wstring(error));
        CloseHandle(mailslot_server);
        return false;
    }
    
    // Download and stream content - simplified demonstration
    std::string master;
    if (cancel_token.load()) {
        CloseHandle(mailslot_client);
        CloseHandle(mailslot_server);
        return false;
    }
    
    if (!HttpGetText(playlist_url, master)) {
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to download master playlist");
        CloseHandle(mailslot_client);
        CloseHandle(mailslot_server);
        return false;
    }
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Downloaded master playlist, starting streaming demonstration");
    
    // For demonstration, simulate streaming multiple segments
    for (int i = 0; i < 3 && !cancel_token.load(); i++) {
        // Create test video segment (simulating real video data)
        std::vector<char> segment_data(1024 * 1024, static_cast<char>('A' + i)); // 1MB per segment
        AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Sending segment " + std::to_wstring(i + 1) + L" via MailSlot");
        
        if (!SendVideoSegmentViaMailSlot(mailslot_client, segment_data)) {
            AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Failed to send segment " + std::to_wstring(i + 1));
            break;
        } else {
            AddDebugLog(L"BufferAndMailSlotStreamToPlayer: Successfully sent segment " + std::to_wstring(i + 1) + L" via MailSlot");
        }
        
        // Small delay between segments to simulate real streaming
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    AddDebugLog(L"BufferAndMailSlotStreamToPlayer: MailSlot streaming demonstration completed");
    
    // Keep process alive briefly to observe results
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    CloseHandle(mailslot_client);
    CloseHandle(mailslot_server);
    
    return true;
}

bool BufferAndNamedPipeStreamToPlayer(
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
) {
    AddDebugLog(L"[IPC-METHOD] Named Pipe implementation starting for " + channel_name);
    
    // Generate unique pipe name
    int pipe_id = ++g_namedpipe_counter;
    std::wstring pipe_name = L"\\\\.\\pipe\\tardsplaya_" + std::to_wstring(pipe_id);
    
    // Create named pipe
    HANDLE client_handle;
    HANDLE server_handle = CreateStreamingNamedPipe(pipe_name, client_handle);
    
    if (server_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Failed to create named pipe, error=" + std::to_wstring(error));
        return false;
    }
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Created Named Pipe: " + pipe_name);
    
    // Create media player process
    std::wstring cmd;
    if (player_path.find(L"mpc-hc") != std::wstring::npos) {
        cmd = L"\"" + player_path + L"\" - /new /nofocus";
    } else if (player_path.find(L"vlc") != std::wstring::npos) {
        cmd = L"\"" + player_path + L"\" - --intf dummy --no-one-instance";
    } else {
        cmd = L"\"" + player_path + L"\" -";
    }
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdInput = client_handle;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Launching player with Named Pipe: " + cmd);
    
    BOOL success = CreateProcessW(
        nullptr, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, TRUE,
        CREATE_NEW_CONSOLE | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi
    );
    
    CloseHandle(client_handle); // Child process now owns it
    
    if (!success) {
        DWORD error = GetLastError();
        AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Failed to create process, error=" + std::to_wstring(error));
        CloseHandle(server_handle);
        return false;
    }
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Process created, PID=" + std::to_wstring(pi.dwProcessId));
    
    // Wait for client connection
    if (!ConnectNamedPipe(server_handle, nullptr)) {
        DWORD error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) {
            AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Failed to connect to named pipe, error=" + std::to_wstring(error));
            CloseHandle(server_handle);
            return false;
        }
    }
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Named pipe connected");
    
    // Download and stream content - simplified demonstration
    std::string master;
    if (cancel_token.load()) {
        CloseHandle(server_handle);
        return false;
    }
    
    if (!HttpGetText(playlist_url, master)) {
        AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Failed to download master playlist");
        CloseHandle(server_handle);
        return false;
    }
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Downloaded master playlist, starting streaming demonstration");
    
    // For demonstration, simulate streaming multiple segments
    for (int i = 0; i < 3 && !cancel_token.load(); i++) {
        // Create test video segment (simulating real video data)
        std::vector<char> segment_data(1024 * 1024, static_cast<char>('A' + i)); // 1MB per segment
        DWORD bytes_written;
        
        AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Sending segment " + std::to_wstring(i + 1) + L" via Named Pipe");
        
        if (!WriteFile(server_handle, segment_data.data(), (DWORD)segment_data.size(), &bytes_written, nullptr)) {
            DWORD error = GetLastError();
            AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Failed to write segment " + std::to_wstring(i + 1) + L", error=" + std::to_wstring(error));
            break;
        } else {
            AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Successfully sent segment " + std::to_wstring(i + 1) + L" (" + std::to_wstring(bytes_written) + L" bytes) via Named Pipe");
        }
        
        // Small delay between segments to simulate real streaming
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    AddDebugLog(L"BufferAndNamedPipeStreamToPlayer: Named Pipe streaming demonstration completed");
    
    // Keep process alive briefly to observe results
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    CloseHandle(server_handle);
    
    return true;
}

bool StreamToPlayerWithIPC(
    IPCMethod method,
    const std::wstring& player_path,
    const std::wstring& playlist_url,
    std::atomic<bool>& cancel_token,
    int buffer_segments,
    const std::wstring& channel_name,
    std::atomic<int>* chunk_count,
    const std::wstring& selected_quality
) {
    std::wstring method_name;
    switch (method) {
        case IPCMethod::ANONYMOUS_PIPES: method_name = L"Anonymous Pipes"; break;
        case IPCMethod::MAILSLOTS: method_name = L"MailSlots"; break;
        case IPCMethod::NAMED_PIPES: method_name = L"Named Pipes"; break;
        default: method_name = L"Unknown"; break;
    }
    AddDebugLog(L"[IPC-METHOD] StreamToPlayerWithIPC using " + method_name + L" for " + channel_name);
    
    switch (method) {
        case IPCMethod::MAILSLOTS:
            return BufferAndMailSlotStreamToPlayer(player_path, playlist_url, cancel_token, 
                                                 buffer_segments, channel_name, chunk_count, selected_quality);
        
        case IPCMethod::NAMED_PIPES:
            return BufferAndNamedPipeStreamToPlayer(player_path, playlist_url, cancel_token,
                                                  buffer_segments, channel_name, chunk_count, selected_quality);
        
        case IPCMethod::ANONYMOUS_PIPES:
        default:
            // Use the original function (would need to be declared extern)
            AddDebugLog(L"StreamToPlayerWithIPC: Falling back to anonymous pipes (original implementation)");
            return false; // Placeholder - would call original function
    }
}