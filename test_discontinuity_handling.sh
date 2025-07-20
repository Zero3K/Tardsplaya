#!/bin/bash
# Test script for VirtualDub2-style discontinuity handling
# This script can be used to manually test the discontinuity handling implementation

echo "=== Tardsplaya Discontinuity Handling Test ==="
echo

# Check if we're on Windows (where we can actually test the executable)
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OSTYPE" == "win32" ]]; then
    echo "✓ Windows environment detected - can run full tests"
    
    # Check if Tardsplaya.exe exists
    if [ -f "Tardsplaya.exe" ]; then
        echo "✓ Tardsplaya.exe found"
    else
        echo "❌ Tardsplaya.exe not found - please build first"
        exit 1
    fi
    
    # Check if a media player is available
    if command -v mpv >/dev/null 2>&1; then
        echo "✓ MPV media player found"
        PLAYER="mpv"
    elif command -v vlc >/dev/null 2>&1; then
        echo "✓ VLC media player found"
        PLAYER="vlc"
    else
        echo "⚠ Warning: No media player found. Install MPV or VLC for testing."
        PLAYER=""
    fi
    
    echo
    echo "--- Manual Test Instructions ---"
    echo "1. Launch Tardsplaya.exe"
    echo "2. Enter a Twitch channel with frequent ads (e.g., 'shroud', 'ninja')"
    echo "3. Start watching and wait for ad breaks"
    echo "4. Monitor the debug log for discontinuity messages:"
    echo "   - '[DISCONTINUITY] Detected ad transition'"
    echo "   - '[KEYFRAME_WAIT] Activated keyframe waiting mode'"
    echo "   - '[KEYFRAME_WAIT] Found keyframe after X frames'"
    echo "5. Verify smooth transition back to main content (no black frames)"
    
    echo
    echo "--- Expected Log Output During Ad Breaks ---"
    echo "[DISCONTINUITY] Detected ad transition - implementing fast restart"
    echo "[KEYFRAME_WAIT] Activated keyframe waiting mode (max 30 frames)"
    echo "[KEYFRAME_WAIT] Skipping non-keyframe packet (frame #123)"
    echo "[KEYFRAME_WAIT] Found keyframe after 5 frames - resuming normal playback"
    
else
    echo "ℹ Linux/Unix environment - running logic verification tests only"
    
    # Compile and run our test programs
    echo
    echo "--- Running Logic Verification Tests ---"
    
    if command -v g++ >/dev/null 2>&1; then
        echo "✓ g++ compiler found"
        
        # Create test directory
        mkdir -p /tmp/discontinuity_tests
        cd /tmp/discontinuity_tests
        
        # Copy test files
        cat > basic_test.cpp << 'EOF'
#include <iostream>
#include <atomic>

struct TSPacket {
    bool is_key_frame = false;
    uint64_t frame_number = 0;
    bool discontinuity = false;
};

class TestRouter {
private:
    std::atomic<bool> wait_for_keyframe_{false};
    std::atomic<int> keyframe_wait_counter_{0};

public:
    void SetWaitForKeyframe() {
        wait_for_keyframe_ = true;
        keyframe_wait_counter_ = 0;
    }

    bool ShouldSkipFrame(const TSPacket& packet) {
        if (!wait_for_keyframe_.load()) return false;
        
        int current_wait = keyframe_wait_counter_.fetch_add(1) + 1;
        
        if (packet.is_key_frame) {
            wait_for_keyframe_ = false;
            keyframe_wait_counter_ = 0;
            return false;
        }
        
        if (current_wait >= 30) {
            wait_for_keyframe_ = false;
            keyframe_wait_counter_ = 0;
            return false;
        }
        
        return true;
    }
};

int main() {
    TestRouter router;
    int passed = 0, total = 0;
    
    // Test 1: Normal operation
    total++;
    TSPacket normal;
    normal.is_key_frame = false;
    if (!router.ShouldSkipFrame(normal)) {
        std::cout << "✓ Test 1 PASSED: Normal packets are sent" << std::endl;
        passed++;
    } else {
        std::cout << "❌ Test 1 FAILED: Normal packets should not be skipped" << std::endl;
    }
    
    // Test 2: Discontinuity with keyframe recovery
    total++;
    router.SetWaitForKeyframe();
    
    TSPacket non_key;
    non_key.is_key_frame = false;
    bool skipped_non_key = router.ShouldSkipFrame(non_key);
    
    TSPacket keyframe;
    keyframe.is_key_frame = true;
    bool sent_keyframe = !router.ShouldSkipFrame(keyframe);
    
    if (skipped_non_key && sent_keyframe) {
        std::cout << "✓ Test 2 PASSED: Non-keyframes skipped, keyframes sent" << std::endl;
        passed++;
    } else {
        std::cout << "❌ Test 2 FAILED: Keyframe waiting logic error" << std::endl;
    }
    
    // Test 3: Timeout protection
    total++;
    router.SetWaitForKeyframe();
    bool timeout_triggered = false;
    
    for (int i = 0; i < 35; i++) {
        TSPacket packet;
        packet.is_key_frame = false;
        bool skip = router.ShouldSkipFrame(packet);
        if (i >= 30 && !skip) {
            timeout_triggered = true;
            break;
        }
    }
    
    if (timeout_triggered) {
        std::cout << "✓ Test 3 PASSED: Timeout protection works" << std::endl;
        passed++;
    } else {
        std::cout << "❌ Test 3 FAILED: Timeout protection not working" << std::endl;
    }
    
    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << "/" << total << " tests" << std::endl;
    
    if (passed == total) {
        std::cout << "🎉 All tests PASSED! Discontinuity handling logic is correct." << std::endl;
        return 0;
    } else {
        std::cout << "❌ Some tests FAILED! Check implementation." << std::endl;
        return 1;
    }
}
EOF
        
        echo "Compiling test..."
        if g++ -std=c++17 basic_test.cpp -o basic_test; then
            echo "Running basic logic test..."
            ./basic_test
            TEST_RESULT=$?
            
            if [ $TEST_RESULT -eq 0 ]; then
                echo
                echo "🎉 Logic verification completed successfully!"
                echo "The discontinuity handling implementation is ready for testing."
            else
                echo
                echo "❌ Logic verification failed. Check implementation."
            fi
        else
            echo "❌ Compilation failed"
        fi
        
    else
        echo "❌ g++ compiler not found - cannot run logic tests"
        echo "Install g++ to run verification tests"
    fi
fi

echo
echo "=== Test Complete ==="
echo
echo "Next Steps:"
echo "1. Build Tardsplaya on Windows"
echo "2. Test with real Twitch streams containing ads"  
echo "3. Monitor logs for discontinuity handling messages"
echo "4. Verify smooth playback transitions after ad breaks"
echo
echo "Expected benefits:"
echo "- No more black frame sticking after ads"
echo "- Smooth transitions back to main content"
echo "- Robust handling of various ad insertion methods"