#include "datapath_bridge.h"
#include "datapath/include/datapath.hpp"
#include "datapath/include/error.hpp"
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

DatapathMediaPlayerBridge::DatapathMediaPlayerBridge()
    : is_running_(false)
    , should_stop_(false)
    , output_handle_(INVALID_HANDLE_VALUE)
{
}

DatapathMediaPlayerBridge::~DatapathMediaPlayerBridge() {
    Stop();
}

int DatapathMediaPlayerBridge::Main(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::wcerr << L"Usage: " << (argc > 0 ? argv[0] : L"DatapathBridge") 
                   << L" <datapath_server_name> [output_file]" << std::endl;
        std::wcerr << L"  datapath_server_name: Name of the Datapath server to connect to" << std::endl;
        std::wcerr << L"  output_file: Optional file to write to (default: stdout)" << std::endl;
        return 1;
    }

    std::wstring server_name = argv[1];
    HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE);

    // Optional: redirect to file if specified
    if (argc >= 3) {
        output_handle = CreateFileW(
            argv[2],
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (output_handle == INVALID_HANDLE_VALUE) {
            std::wcerr << L"Error: Cannot create output file: " << argv[2] << std::endl;
            return 2;
        }
    }

    // Create and start bridge
    DatapathMediaPlayerBridge bridge;
    if (!bridge.Start(server_name, output_handle)) {
        std::wcerr << L"Error: Failed to start bridge for server: " << server_name << std::endl;
        if (output_handle != GetStdHandle(STD_OUTPUT_HANDLE)) {
            CloseHandle(output_handle);
        }
        return 3;
    }

    // Wait for bridge to complete
    while (bridge.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bridge.Stop();

    if (output_handle != GetStdHandle(STD_OUTPUT_HANDLE)) {
        CloseHandle(output_handle);
    }

    return 0;
}

bool DatapathMediaPlayerBridge::Start(const std::wstring& datapath_server_name, HANDLE output_handle) {
    if (is_running_.load()) {
        return false; // Already running
    }

    server_name_ = datapath_server_name;
    output_handle_ = output_handle;
    should_stop_.store(false);

    // Start bridge operation in a separate thread
    std::thread bridge_thread([this]() {
        RunBridgeLoop();
    });
    bridge_thread.detach();

    // Wait a moment to see if startup was successful
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return is_running_.load();
}

void DatapathMediaPlayerBridge::Stop() {
    should_stop_.store(true);
    
    // Wait for the bridge to stop
    int wait_count = 0;
    while (is_running_.load() && wait_count < 50) { // 5 second timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
    }
}

bool DatapathMediaPlayerBridge::IsRunning() const {
    return is_running_.load();
}

bool DatapathMediaPlayerBridge::ConnectToDatapathServer() {
    // Implementation would go here - connect to Datapath server
    // This is a placeholder since we need the actual Datapath client implementation
    
    try {
        std::shared_ptr<datapath::isocket> client_socket;
        std::string server_name_str(server_name_.begin(), server_name_.end());
        
        datapath::error result = datapath::connect(client_socket, server_name_str);
        
        if (result != datapath::error::Success) {
            std::wcerr << L"Failed to connect to Datapath server: " << server_name_ 
                       << L", error: " << static_cast<int>(result) << std::endl;
            return false;
        }

        // Set up event handlers
        client_socket->on_message.add([this](const std::vector<char>& data) {
            OnDataReceived(data);
        });

        client_socket->on_close.add([this]() {
            OnConnectionClosed();
        });

        return true;
    }
    catch (const std::exception&) {
        std::wcerr << L"Exception while connecting to Datapath server: " << server_name_ << std::endl;
        return false;
    }
}

void DatapathMediaPlayerBridge::RunBridgeLoop() {
    is_running_.store(true);

    // Connect to Datapath server
    if (!ConnectToDatapathServer()) {
        std::wcerr << L"Failed to connect to Datapath server: " << server_name_ << std::endl;
        is_running_.store(false);
        return;
    }

    std::wcerr << L"Connected to Datapath server: " << server_name_ << std::endl;

    // Main bridge loop
    while (!should_stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::wcerr << L"Bridge shutting down for server: " << server_name_ << std::endl;
    is_running_.store(false);
}

void DatapathMediaPlayerBridge::OnDataReceived(const std::vector<char>& data) {
    if (output_handle_ == INVALID_HANDLE_VALUE || data.empty()) {
        return;
    }

    // Write data to output handle (stdout or file)
    DWORD bytes_written = 0;
    BOOL result = WriteFile(
        output_handle_,
        data.data(),
        static_cast<DWORD>(data.size()),
        &bytes_written,
        nullptr
    );

    if (!result || bytes_written != data.size()) {
        DWORD error = GetLastError();
        std::wcerr << L"Failed to write data to output, error: " << error << std::endl;
        should_stop_.store(true);
    }
}

void DatapathMediaPlayerBridge::OnConnectionClosed() {
    std::wcerr << L"Datapath connection closed for server: " << server_name_ << std::endl;
    should_stop_.store(true);
}

// Main entry point for standalone executable
#ifdef DATAPATH_BRIDGE_MAIN
int wmain(int argc, wchar_t* argv[]) {
    return DatapathMediaPlayerBridge::Main(argc, argv);
}
#endif