#pragma once
#include <string>
#include <atomic>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/**
 * Helper utility for bridging Datapath IPC to media players via named pipes
 * This creates a standalone executable that can be used by media players
 * to read from Datapath servers through a named pipe interface.
 */
class DatapathMediaPlayerBridge {
public:
    DatapathMediaPlayerBridge();
    ~DatapathMediaPlayerBridge();

    /**
     * Main entry point for the bridge application
     * Usage: DatapathBridge.exe <datapath_server_name> <named_pipe_path>
     * @param argc Command line argument count
     * @param argv Command line arguments
     * @return Exit code (0 = success, non-zero = error)
     */
    static int Main(int argc, wchar_t* argv[]);

    /**
     * Start the bridge between Datapath client and stdout
     * @param datapath_server_name Name of the Datapath server to connect to
     * @param output_handle Handle to write data to (typically stdout)
     * @return true on success, false on failure
     */
    bool Start(const std::wstring& datapath_server_name, HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE));

    /**
     * Stop the bridge operation
     */
    void Stop();

    /**
     * Check if the bridge is currently running
     * @return true if running, false otherwise
     */
    bool IsRunning() const;

private:
    std::atomic<bool> is_running_;
    std::atomic<bool> should_stop_;
    HANDLE output_handle_;
    std::wstring server_name_;

    // Bridge implementation
    bool ConnectToDatapathServer();
    void RunBridgeLoop();
    void OnDataReceived(const std::vector<char>& data);
    void OnConnectionClosed();
};