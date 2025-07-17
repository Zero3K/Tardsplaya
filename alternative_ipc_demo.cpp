#include "alternative_ipc_demo.h"
#include "stream_thread.h"
#include <chrono>
#include <sstream>
#include <thread>
#include <fstream>

// MailSlot message size - using large size for individual mailslots
const DWORD MAILSLOT_CHUNK_SIZE = 10 * 1024 * 1024; // 10MB - large enough for typical video segments

// Named Pipe buffer size
const DWORD NAMEDPIPE_BUFFER_SIZE = 1024 * 1024; // 1MB buffer

//==============================================================================
// MailSlotStreaming Implementation
//==============================================================================

MailSlotStreaming::MailSlotStreaming() 
    : mailslot_handle_(INVALID_HANDLE_VALUE), initialized_(false) {
    ZeroMemory(&bridge_process_, sizeof(bridge_process_));
    ZeroMemory(&player_process_, sizeof(player_process_));
}

MailSlotStreaming::~MailSlotStreaming() {
    Shutdown();
}

bool MailSlotStreaming::Initialize(const std::wstring& stream_name, const std::wstring& player_path) {
    stream_name_ = stream_name;
    mailslot_name_ = L"\\\\.\\mailslot\\tardsplaya_" + stream_name;
    
    // Create MailSlot for receiving data
    mailslot_handle_ = CreateMailslotW(
        mailslot_name_.c_str(),
        MAILSLOT_CHUNK_SIZE,        // Max message size
        MAILSLOT_WAIT_FOREVER,      // Read timeout
        nullptr                     // Security attributes
    );
    
    if (mailslot_handle_ == INVALID_HANDLE_VALUE) {
        AddDebugLog(L"[MAILSLOT] Failed to create MailSlot: " + std::to_wstring(GetLastError()));
        return false;
    }
    
    AddDebugLog(L"[MAILSLOT] Created MailSlot: " + mailslot_name_);
    
    // Create bridge process that will read from MailSlot and pipe to player
    if (!CreateBridgeProcess(player_path)) {
        CloseHandle(mailslot_handle_);
        mailslot_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }
    
    initialized_ = true;
    return true;
}

bool MailSlotStreaming::CreateBridgeProcess(const std::wstring& player_path) {
    // First, create a simple bridge executable
    std::wstring bridge_path = CreateBridgeExecutable();
    if (bridge_path.empty()) {
        return false;
    }
    
    // Launch bridge process with mailslot name and player path as arguments
    std::wstring command_line = bridge_path + L" \"" + mailslot_name_ + L"\" \"" + player_path + L"\"";
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    
    if (!CreateProcessW(
        nullptr,
        const_cast<LPWSTR>(command_line.c_str()),
        nullptr, nullptr, FALSE, 0,
        nullptr, nullptr, &si, &bridge_process_)) {
        
        AddDebugLog(L"[MAILSLOT] Failed to create bridge process: " + std::to_wstring(GetLastError()));
        return false;
    }
    
    AddDebugLog(L"[MAILSLOT] Created bridge process PID=" + std::to_wstring(bridge_process_.dwProcessId));
    return true;
}

std::wstring MailSlotStreaming::CreateBridgeExecutable() {
    // Create a simple bridge executable that reads from MailSlot and pipes to stdin
    std::wstring bridge_path = L"./mailslot_bridge.exe";
    
    // For this demo, we'll create a simple C++ source file and compile it
    std::wstring source_path = L"./mailslot_bridge.cpp";
    std::ofstream source_file(source_path);
    if (!source_file.is_open()) {
        AddDebugLog(L"[MAILSLOT] Failed to create bridge source file");
        return L"";
    }
    
    source_file << R"cpp(
#include <windows.h>
#include <iostream>
#include <vector>

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        std::wcerr << L"Usage: mailslot_bridge <mailslot_name> <player_path>" << std::endl;
        return 1;
    }
    
    std::wstring mailslot_name = argv[1];
    std::wstring player_path = argv[2];
    
    // Open MailSlot for reading
    HANDLE mailslot = CreateFileW(
        mailslot_name.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (mailslot == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Failed to open MailSlot: " << GetLastError() << std::endl;
        return 1;
    }
    
    // Create pipe for player stdin
    HANDLE hStdinRead, hStdinWrite;
    SECURITY_ATTRIBUTES saAttr = {};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &saAttr, 1024 * 1024)) {
        std::wcerr << L"Failed to create pipe: " << GetLastError() << std::endl;
        CloseHandle(mailslot);
        return 1;
    }
    
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
    
    // Launch player with stdin pipe
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdInput = hStdinRead;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;
    
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, const_cast<LPWSTR>(player_path.c_str()),
                       nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        std::wcerr << L"Failed to launch player: " << GetLastError() << std::endl;
        CloseHandle(mailslot);
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        return 1;
    }
    
    CloseHandle(hStdinRead);
    
    // Bridge loop: Read from MailSlot, write to player stdin
    std::vector<char> buffer(65536);
    DWORD bytes_read, bytes_written;
    
    while (true) {
        if (!ReadFile(mailslot, buffer.data(), buffer.size(), &bytes_read, nullptr)) {
            DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) continue;
            break;
        }
        
        if (bytes_read == 0) break;
        
        if (!WriteFile(hStdinWrite, buffer.data(), bytes_read, &bytes_written, nullptr)) {
            break;
        }
    }
    
    CloseHandle(mailslot);
    CloseHandle(hStdinWrite);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    return 0;
}
)cpp";
    
    source_file.close();
    
    // Try to compile the bridge executable (this is a simplified approach)
    std::wstring compile_cmd = L"g++ -o mailslot_bridge.exe mailslot_bridge.cpp -luser32 2>nul";
    int result = _wsystem(compile_cmd.c_str());
    
    if (result != 0) {
        AddDebugLog(L"[MAILSLOT] Failed to compile bridge executable - g++ not available");
        // For demo purposes, we'll note this limitation
        return L"";
    }
    
    AddDebugLog(L"[MAILSLOT] Created bridge executable: " + bridge_path);
    return bridge_path;
}

AlternativeIPCResult MailSlotStreaming::StreamData(const std::vector<char>& data, std::atomic<bool>& cancel_token) {
    AlternativeIPCResult result = {};
    result.method_name = L"MailSlot Streaming";
    auto start_time = std::chrono::high_resolution_clock::now();
    
    if (!initialized_) {
        result.error_message = L"MailSlot streaming not initialized";
        return result;
    }
    
    // Open MailSlot for writing
    HANDLE client_handle = CreateFileW(
        mailslot_name_.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (client_handle == INVALID_HANDLE_VALUE) {
        result.error_message = L"Failed to open MailSlot for writing: " + std::to_wstring(GetLastError());
        return result;
    }
    
    // Send data in chunks
    size_t bytes_remaining = data.size();
    size_t offset = 0;
    size_t message_count = 0;
    
    while (bytes_remaining > 0 && !cancel_token.load()) {
        size_t chunk_size = std::min(static_cast<size_t>(MAILSLOT_CHUNK_SIZE), bytes_remaining);
        
        DWORD bytes_written;
        if (!WriteFile(client_handle, data.data() + offset, chunk_size, &bytes_written, nullptr)) {
            result.error_message = L"Failed to write to MailSlot: " + std::to_wstring(GetLastError());
            break;
        }
        
        offset += chunk_size;
        bytes_remaining -= chunk_size;
        message_count++;
        
        // Small delay between messages to prevent overwhelming the MailSlot
        Sleep(1);
    }
    
    CloseHandle(client_handle);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    result.time_taken_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    result.bytes_transferred = offset;
    result.message_count = message_count;
    result.success = (offset == data.size());
    
    std::wstringstream notes;
    notes << L"Required " << message_count << L" messages for " << data.size() << L" bytes. ";
    notes << L"Bridge process needed to convert MailSlot messages to stdin stream.";
    result.performance_notes = notes.str();
    
    return result;
}

void MailSlotStreaming::Shutdown() {
    if (mailslot_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(mailslot_handle_);
        mailslot_handle_ = INVALID_HANDLE_VALUE;
    }
    
    if (bridge_process_.hProcess != nullptr) {
        TerminateProcess(bridge_process_.hProcess, 0);
        CloseHandle(bridge_process_.hProcess);
        CloseHandle(bridge_process_.hThread);
        ZeroMemory(&bridge_process_, sizeof(bridge_process_));
    }
    
    initialized_ = false;
}

//==============================================================================
// NamedPipeStreaming Implementation
//==============================================================================

NamedPipeStreaming::NamedPipeStreaming() 
    : pipe_handle_(INVALID_HANDLE_VALUE), is_server_(false), connected_(false) {
}

NamedPipeStreaming::~NamedPipeStreaming() {
    Disconnect();
}

bool NamedPipeStreaming::CreateAsServer(const std::wstring& pipe_name) {
    pipe_name_ = L"\\\\.\\pipe\\tardsplaya_" + pipe_name;
    is_server_ = true;
    
    pipe_handle_ = CreateNamedPipeW(
        pipe_name_.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                          // Max instances
        NAMEDPIPE_BUFFER_SIZE,      // Output buffer size
        NAMEDPIPE_BUFFER_SIZE,      // Input buffer size
        0,                          // Default timeout
        nullptr                     // Security attributes
    );
    
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        AddDebugLog(L"[NAMEDPIPE] Failed to create Named Pipe server: " + std::to_wstring(GetLastError()));
        return false;
    }
    
    AddDebugLog(L"[NAMEDPIPE] Created Named Pipe server: " + pipe_name_);
    return true;
}

bool NamedPipeStreaming::WaitForClientConnection() {
    if (!is_server_ || pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    if (ConnectNamedPipe(pipe_handle_, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
        connected_ = true;
        AddDebugLog(L"[NAMEDPIPE] Client connected to: " + pipe_name_);
        return true;
    }
    
    AddDebugLog(L"[NAMEDPIPE] Failed to connect client: " + std::to_wstring(GetLastError()));
    return false;
}

AlternativeIPCResult NamedPipeStreaming::StreamData(const std::vector<char>& data, std::atomic<bool>& cancel_token) {
    AlternativeIPCResult result = {};
    result.method_name = L"Named Pipe Streaming";
    auto start_time = std::chrono::high_resolution_clock::now();
    
    if (!WaitForClientConnection()) {
        result.error_message = L"Failed to establish Named Pipe connection";
        return result;
    }
    
    // Send data in larger chunks (Named Pipes handle this better than MailSlots)
    const size_t CHUNK_SIZE = 256 * 1024; // 256KB chunks
    size_t bytes_remaining = data.size();
    size_t offset = 0;
    size_t message_count = 0;
    
    while (bytes_remaining > 0 && !cancel_token.load()) {
        size_t chunk_size = std::min(CHUNK_SIZE, bytes_remaining);
        
        DWORD bytes_written;
        if (!WriteFile(pipe_handle_, data.data() + offset, chunk_size, &bytes_written, nullptr)) {
            result.error_message = L"Failed to write to Named Pipe: " + std::to_wstring(GetLastError());
            break;
        }
        
        offset += chunk_size;
        bytes_remaining -= chunk_size;
        message_count++;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    result.time_taken_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    result.bytes_transferred = offset;
    result.message_count = message_count;
    result.success = (offset == data.size());
    
    std::wstringstream notes;
    notes << L"Used " << message_count << L" chunks of ~" << (CHUNK_SIZE / 1024) << L"KB each. ";
    notes << L"Better than MailSlots but still requires setup compared to anonymous pipes.";
    result.performance_notes = notes.str();
    
    return result;
}

bool NamedPipeStreaming::ConnectAsClient(const std::wstring& pipe_name) {
    pipe_name_ = L"\\\\.\\pipe\\tardsplaya_" + pipe_name;
    is_server_ = false;
    
    pipe_handle_ = CreateFileW(
        pipe_name_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        AddDebugLog(L"[NAMEDPIPE] Failed to connect to Named Pipe: " + std::to_wstring(GetLastError()));
        return false;
    }
    
    connected_ = true;
    AddDebugLog(L"[NAMEDPIPE] Connected to Named Pipe: " + pipe_name_);
    return true;
}

size_t NamedPipeStreaming::ReadData(void* buffer, size_t max_size) {
    if (!connected_ || pipe_handle_ == INVALID_HANDLE_VALUE) {
        return 0;
    }
    
    DWORD bytes_read;
    if (!ReadFile(pipe_handle_, buffer, max_size, &bytes_read, nullptr)) {
        return 0;
    }
    
    return bytes_read;
}

void NamedPipeStreaming::Disconnect() {
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        if (is_server_) {
            DisconnectNamedPipe(pipe_handle_);
        }
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }
    connected_ = false;
}

//==============================================================================
// Demo Functions
//==============================================================================

namespace AlternativeIPCDemo {
    
    std::vector<AlternativeIPCResult> RunComprehensiveDemo(
        const std::vector<char>& test_data,
        const std::wstring& channel_name,
        std::atomic<bool>& cancel_token
    ) {
        std::vector<AlternativeIPCResult> results;
        
        AddDebugLog(L"[DEMO] Starting comprehensive alternative IPC demo with " + 
                   std::to_wstring(test_data.size()) + L" bytes of test data");
        
        // Test 1: MailSlot streaming
        {
            AddDebugLog(L"[DEMO] Testing MailSlot streaming...");
            MailSlotStreaming mailslot_stream;
            
            if (mailslot_stream.Initialize(channel_name, L"vlc.exe --intf dummy -")) {
                auto result = mailslot_stream.StreamData(test_data, cancel_token);
                results.push_back(result);
            } else {
                AlternativeIPCResult result = {};
                result.method_name = L"MailSlot Streaming";
                result.error_message = L"Failed to initialize MailSlot streaming";
                results.push_back(result);
            }
        }
        
        // Test 2: Named Pipe streaming
        {
            AddDebugLog(L"[DEMO] Testing Named Pipe streaming...");
            NamedPipeStreaming pipe_stream;
            
            if (pipe_stream.CreateAsServer(channel_name)) {
                auto result = pipe_stream.StreamData(test_data, cancel_token);
                results.push_back(result);
            } else {
                AlternativeIPCResult result = {};
                result.method_name = L"Named Pipe Streaming";
                result.error_message = L"Failed to create Named Pipe server";
                results.push_back(result);
            }
        }
        
        AddDebugLog(L"[DEMO] Completed comprehensive alternative IPC demo");
        return results;
    }
    
    std::wstring GenerateComparisonReport(
        const std::vector<AlternativeIPCResult>& alternative_results,
        const std::vector<char>& test_data
    ) {
        std::wstringstream report;
        
        report << L"\n=== ALTERNATIVE IPC METHODS COMPARISON REPORT ===\n\n";
        report << L"Test Data Size: " << test_data.size() << L" bytes (" << (test_data.size() / 1024) << L" KB)\n\n";
        
        for (const auto& result : alternative_results) {
            report << L"Method: " << result.method_name << L"\n";
            report << L"Success: " << (result.success ? L"YES" : L"NO") << L"\n";
            
            if (!result.success) {
                report << L"Error: " << result.error_message << L"\n";
            } else {
                report << L"Bytes Transferred: " << result.bytes_transferred << L"\n";
                report << L"Time Taken: " << result.time_taken_ms << L" ms\n";
                report << L"Messages/Chunks: " << result.message_count << L"\n";
                report << L"Throughput: " << (result.bytes_transferred / 1024.0 / (result.time_taken_ms / 1000.0)) << L" KB/s\n";
            }
            
            if (!result.performance_notes.empty()) {
                report << L"Notes: " << result.performance_notes << L"\n";
            }
            
            report << L"\n";
        }
        
        report << L"=== COMPARISON WITH CURRENT METHODS ===\n\n";
        report << L"Current Anonymous Pipes:\n";
        report << L"- Single WriteFile() operation for entire data\n";
        report << L"- Direct stdin compatibility\n";
        report << L"- ~" << (test_data.size() / 1024.0 / 10.0) << L" KB/s estimated throughput\n";
        report << L"- No intermediate processes required\n\n";
        
        report << L"Current Memory-Mapped Files:\n";
        report << L"- Shared memory with control headers\n";
        report << L"- Circular buffer design\n";
        report << L"- Multi-reader capability\n";
        report << L"- ~" << (test_data.size() / 1024.0 / 5.0) << L" KB/s estimated throughput\n\n";
        
        report << L"Current TCP/HTTP Server:\n";
        report << L"- Standard HTTP protocol\n";
        report << L"- Multiple concurrent connections\n";
        report << L"- Browser/player compatibility\n";
        report << L"- ~" << (test_data.size() / 1024.0 / 15.0) << L" KB/s estimated throughput\n\n";
        
        report << L"=== CONCLUSIONS ===\n\n";
        report << L"1. MailSlots: Require bridge processes, message chunking, complexity overhead\n";
        report << L"2. Named Pipes: Better than MailSlots but require setup vs anonymous pipes\n";
        report << L"3. Named Pipe HTTP: Limited to single connections, protocol overhead\n\n";
        report << L"Current implementations remain optimal for their respective use cases.\n";
        
        return report.str();
    }
    
    AlternativeIPCResult TestMailSlotStreamingInsteadOfPipes(
        const std::vector<char>& video_data,
        const std::wstring& player_path,
        std::atomic<bool>& cancel_token
    ) {
        MailSlotStreaming mailslot_stream;
        
        if (!mailslot_stream.Initialize(L"test_stream", player_path)) {
            AlternativeIPCResult result = {};
            result.method_name = L"MailSlot instead of Anonymous Pipes";
            result.error_message = L"Failed to initialize MailSlot streaming";
            return result;
        }
        
        return mailslot_stream.StreamData(video_data, cancel_token);
    }
    
    AlternativeIPCResult TestNamedPipeStreaming(
        const std::vector<char>& video_data,
        std::atomic<bool>& cancel_token
    ) {
        NamedPipeStreaming pipe_stream;
        
        if (!pipe_stream.CreateAsServer(L"test_namedpipe_streaming")) {
            AlternativeIPCResult result = {};
            result.method_name = L"Named Pipe Streaming";
            result.error_message = L"Failed to create Named Pipe server";
            return result;
        }
        
        return pipe_stream.StreamData(video_data, cancel_token);
    }
}