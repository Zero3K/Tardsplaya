#include "mailslot_comparison.h"
#include "stream_thread.h"
#include <chrono>
#include <sstream>

// MailSlot message size - using conservative limit for demonstration
// Note: Individual mailslots can have larger limits set when created,
// but this doesn't solve the fundamental stdin incompatibility issue
const DWORD MAILSLOT_MAX_MESSAGE_SIZE = 60000; // ~60KB for demonstration purposes

MailSlotComparisonResult TestMailSlotDataTransfer(
    const std::vector<char>& video_data,
    const std::wstring& mailslot_name,
    std::atomic<bool>& cancel_token
) {
    MailSlotComparisonResult result = {};
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Create MailSlot server
    HANDLE mailslot_handle = CreateTestMailSlot(mailslot_name);
    if (mailslot_handle == INVALID_HANDLE_VALUE) {
        result.error_message = L"Failed to create MailSlot: " + std::to_wstring(GetLastError());
        return result;
    }
    
    // Attempt to write video data
    result = WriteVideoSegmentToMailSlot(mailslot_handle, video_data, cancel_token);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    result.time_taken_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    result.total_data_size = video_data.size();
    
    CloseHandle(mailslot_handle);
    return result;
}

HANDLE CreateTestMailSlot(const std::wstring& mailslot_name) {
    // Create MailSlot with maximum message size
    HANDLE mailslot = CreateMailslotW(
        mailslot_name.c_str(),
        MAILSLOT_MAX_MESSAGE_SIZE,  // Max message size
        MAILSLOT_WAIT_FOREVER,      // Read timeout
        nullptr                     // Security attributes
    );
    
    if (mailslot == INVALID_HANDLE_VALUE) {
        AddDebugLog(L"[MAILSLOT] Failed to create MailSlot: " + std::to_wstring(GetLastError()));
    } else {
        AddDebugLog(L"[MAILSLOT] Created MailSlot: " + mailslot_name + 
                   L", MaxMessageSize=" + std::to_wstring(MAILSLOT_MAX_MESSAGE_SIZE));
    }
    
    return mailslot;
}

MailSlotComparisonResult WriteVideoSegmentToMailSlot(
    HANDLE mailslot_handle,
    const std::vector<char>& segment_data,
    std::atomic<bool>& cancel_token
) {
    MailSlotComparisonResult result = {};
    
    if (segment_data.empty()) {
        result.success = true;
        return result;
    }
    
    // MailSlots require chunking large data into small messages
    size_t total_size = segment_data.size();
    size_t bytes_remaining = total_size;
    size_t offset = 0;
    
    AddDebugLog(L"[MAILSLOT] Attempting to send " + std::to_wstring(total_size) + 
               L" bytes via MailSlot (chunked into ~" + std::to_wstring(MAILSLOT_MAX_MESSAGE_SIZE) + L" byte messages)");
    
    while (bytes_remaining > 0 && !cancel_token.load()) {
        size_t chunk_size = std::min((size_t)MAILSLOT_MAX_MESSAGE_SIZE, bytes_remaining);
        
        // Create MailSlot client handle for writing
        HANDLE client_handle = CreateFileW(
            L"\\\\.\\mailslot\\tardsplaya_test",
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        
        if (client_handle == INVALID_HANDLE_VALUE) {
            result.error_message = L"Failed to open MailSlot for writing: " + std::to_wstring(GetLastError());
            AddDebugLog(L"[MAILSLOT] " + result.error_message);
            return result;
        }
        
        DWORD bytes_written = 0;
        BOOL write_result = WriteFile(
            client_handle,
            segment_data.data() + offset,
            (DWORD)chunk_size,
            &bytes_written,
            nullptr
        );
        
        CloseHandle(client_handle);
        
        if (!write_result || bytes_written != chunk_size) {
            result.error_message = L"Failed to write to MailSlot: " + std::to_wstring(GetLastError()) +
                                 L", Expected=" + std::to_wstring(chunk_size) + 
                                 L", Written=" + std::to_wstring(bytes_written);
            AddDebugLog(L"[MAILSLOT] " + result.error_message);
            return result;
        }
        
        result.bytes_written += bytes_written;
        result.messages_sent++;
        offset += chunk_size;
        bytes_remaining -= chunk_size;
        
        // Log progress for large segments
        if (result.messages_sent % 10 == 0) {
            AddDebugLog(L"[MAILSLOT] Sent " + std::to_wstring(result.messages_sent) + 
                       L" messages, " + std::to_wstring(result.bytes_written) + L"/" + 
                       std::to_wstring(total_size) + L" bytes");
        }
    }
    
    if (cancel_token.load()) {
        result.error_message = L"Operation cancelled";
        return result;
    }
    
    result.success = true;
    AddDebugLog(L"[MAILSLOT] Successfully sent " + std::to_wstring(result.bytes_written) + 
               L" bytes in " + std::to_wstring(result.messages_sent) + L" messages");
    
    return result;
}

std::wstring GenerateComparisonReport(
    const MailSlotComparisonResult& mailslot_result,
    size_t pipe_buffer_size,
    bool pipe_success
) {
    std::wostringstream report;
    
    report << L"=== IPC Mechanism Comparison Report ===\n\n";
    
    // MailSlot Results
    report << L"MAILSLOT APPROACH:\n";
    report << L"  Success: " << (mailslot_result.success ? L"YES" : L"NO") << L"\n";
    report << L"  Data Size: " << mailslot_result.total_data_size << L" bytes\n";
    report << L"  Messages Required: " << mailslot_result.messages_sent << L"\n";
    report << L"  Bytes Written: " << mailslot_result.bytes_written << L" bytes\n";
    report << L"  Time Taken: " << mailslot_result.time_taken_ms << L" ms\n";
    if (!mailslot_result.error_message.empty()) {
        report << L"  Error: " << mailslot_result.error_message << L"\n";
    }
    
    // Pipe Results (for comparison)
    report << L"\nANONYMOUS PIPE APPROACH (current):\n";
    report << L"  Success: " << (pipe_success ? L"YES" : L"NO") << L"\n";
    report << L"  Buffer Size: " << pipe_buffer_size << L" bytes (1MB)\n";
    report << L"  Messages Required: 1 (continuous stream)\n";
    report << L"  Can be used as stdin: YES\n";
    report << L"  Supports large data: YES\n";
    
    // Analysis
    report << L"\n=== TECHNICAL ANALYSIS ===\n\n";
    
    report << L"WHY MAILSLOTS ARE NOT SUITABLE:\n";
    report << L"  1. PRIMARY ISSUE - stdin Incompatibility:\n";
    report << L"     - MailSlots cannot be used as process stdin\n";
    report << L"     - Media players expect continuous stdin streams\n";
    report << L"     - Would require intermediate conversion process\n\n";
    
    report << L"  2. Message Size Considerations:\n";
    report << L"     - Broadcast messages: limited to 400 bytes\n";
    report << L"     - Individual mailslots: can be larger (this test uses 60KB)\n";
    report << L"     - Video segments: typically 1-10MB each\n";
    report << L"     - Required " << mailslot_result.messages_sent << L" messages for this segment\n\n";
    
    report << L"  2. Cannot be used as stdin for processes\n";
    report << L"     - Media players expect continuous stdin streams\n";
    report << L"     - MailSlots provide discrete message delivery\n";
    report << L"     - Would require intermediate buffer process\n\n";
    
    report << L"  3. Performance overhead\n";
    report << L"     - Each message requires separate WriteFile call\n";
    report << L"     - Network overhead for each message\n";
    report << L"     - Complex message reassembly required\n\n";
    
    report << L"  4. Designed for different use case\n";
    report << L"     - MailSlots: Discrete notifications/messages\n";
    report << L"     - Current need: Continuous data streaming\n\n";
    
    report << L"WHY ANONYMOUS PIPES ARE SUPERIOR:\n";
    report << L"  1. Direct stdin integration with media players\n";
    report << L"  2. Large buffer support (1MB+)\n";
    report << L"  3. Continuous streaming without message boundaries\n";
    report << L"  4. Optimal for large data transfer\n";
    report << L"  5. Standard IPC mechanism for this use case\n\n";
    
    report << L"=== RECOMMENDATION ===\n";
    report << L"Continue using anonymous pipes for video streaming IPC.\n";
    report << L"MailSlots are not appropriate for this application's needs.\n";
    
    return report.str();
}