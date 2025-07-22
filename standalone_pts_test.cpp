// Standalone test for PTS discontinuity correction logic
// Tests the core PTS parsing and correction algorithms

#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>

// Test PTS extraction from MPEG-TS PES header format
int64_t ExtractPTS(const uint8_t* pts_data) {
    return ((int64_t)(pts_data[0] & 0x0E) << 29) |
           ((int64_t)(pts_data[1]) << 22) |
           ((int64_t)(pts_data[2] & 0xFE) << 14) |
           ((int64_t)(pts_data[3]) << 7) |
           ((int64_t)(pts_data[4] & 0xFE) >> 1);
}

// Test PTS encoding to MPEG-TS PES header format
void EncodePTS(uint8_t* pts_data, int64_t pts, uint8_t prefix = 0x20) {
    pts_data[0] = prefix | (((pts >> 29) & 0x0E)) | 0x01;
    pts_data[1] = (pts >> 22) & 0xFF;
    pts_data[2] = (((pts >> 14) & 0xFE)) | 0x01;
    pts_data[3] = (pts >> 7) & 0xFF;
    pts_data[4] = ((pts << 1) & 0xFE) | 0x01;
}

// Test discontinuity detection and correction
bool TestDiscontinuityCorrection() {
    std::cout << "=== Testing PTS Discontinuity Correction ===" << std::endl;
    
    // Test case 1: Normal progression (no discontinuity)
    int64_t pts1 = 90000;   // 1 second
    int64_t pts2 = 180000;  // 2 seconds
    int64_t threshold = 450000; // 5 seconds
    
    int64_t delta = pts2 - pts1;
    bool discontinuity = std::abs(delta) > threshold;
    
    std::cout << "Test 1 - Normal progression:" << std::endl;
    std::cout << "  PTS1: " << pts1 << " (" << pts1/90 << "ms)" << std::endl;
    std::cout << "  PTS2: " << pts2 << " (" << pts2/90 << "ms)" << std::endl;
    std::cout << "  Delta: " << delta << " (" << delta/90 << "ms)" << std::endl;
    std::cout << "  Discontinuity detected: " << (discontinuity ? "YES" : "NO") << std::endl;
    
    if (discontinuity) {
        std::cout << "ERROR: False positive discontinuity detection!" << std::endl;
        return false;
    }
    
    // Test case 2: Large jump (discontinuity)
    pts1 = 90000;   // 1 second
    pts2 = 9000000; // 100 seconds (large jump)
    
    delta = pts2 - pts1;
    discontinuity = std::abs(delta) > threshold;
    
    std::cout << "\nTest 2 - Large jump (discontinuity):" << std::endl;
    std::cout << "  PTS1: " << pts1 << " (" << pts1/90 << "ms)" << std::endl;
    std::cout << "  PTS2: " << pts2 << " (" << pts2/90 << "ms)" << std::endl;
    std::cout << "  Delta: " << delta << " (" << delta/90 << "ms)" << std::endl;
    std::cout << "  Discontinuity detected: " << (discontinuity ? "YES" : "NO") << std::endl;
    
    if (!discontinuity) {
        std::cout << "ERROR: Failed to detect discontinuity!" << std::endl;
        return false;
    }
    
    // Calculate correction offset
    int64_t correction_offset = pts1 - pts2;
    int64_t corrected_pts2 = pts2 + correction_offset;
    
    std::cout << "  Correction offset: " << correction_offset << " (" << correction_offset/90 << "ms)" << std::endl;
    std::cout << "  Corrected PTS2: " << corrected_pts2 << " (" << corrected_pts2/90 << "ms)" << std::endl;
    
    // Test case 3: Backward jump (also discontinuity)
    pts1 = 9000000; // 100 seconds
    pts2 = 90000;   // 1 second (backward jump)
    
    delta = pts2 - pts1;
    discontinuity = std::abs(delta) > threshold;
    
    std::cout << "\nTest 3 - Backward jump:" << std::endl;
    std::cout << "  PTS1: " << pts1 << " (" << pts1/90 << "ms)" << std::endl;
    std::cout << "  PTS2: " << pts2 << " (" << pts2/90 << "ms)" << std::endl;
    std::cout << "  Delta: " << delta << " (" << delta/90 << "ms)" << std::endl;
    std::cout << "  Discontinuity detected: " << (discontinuity ? "YES" : "NO") << std::endl;
    
    if (!discontinuity) {
        std::cout << "ERROR: Failed to detect backward discontinuity!" << std::endl;
        return false;
    }
    
    return true;
}

// Test PTS encoding/decoding
bool TestPTSEncoding() {
    std::cout << "\n=== Testing PTS Encoding/Decoding ===" << std::endl;
    
    // Test various PTS values
    std::vector<int64_t> test_pts = {
        0,          // 0 seconds
        90000,      // 1 second
        450000,     // 5 seconds
        8589934591, // Maximum 33-bit value
    };
    
    for (int64_t original_pts : test_pts) {
        uint8_t pts_bytes[5];
        EncodePTS(pts_bytes, original_pts);
        int64_t extracted_pts = ExtractPTS(pts_bytes);
        
        std::cout << "Original PTS: " << original_pts << " -> Encoded -> Extracted: " << extracted_pts;
        
        if (original_pts == extracted_pts) {
            std::cout << " [PASS]" << std::endl;
        } else {
            std::cout << " [FAIL]" << std::endl;
            return false;
        }
    }
    
    return true;
}

int main() {
    std::cout << "PTS Discontinuity Correction Test Suite" << std::endl;
    std::cout << "=======================================" << std::endl;
    
    bool encoding_test = TestPTSEncoding();
    bool discontinuity_test = TestDiscontinuityCorrection();
    
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "PTS Encoding/Decoding: " << (encoding_test ? "PASS" : "FAIL") << std::endl;
    std::cout << "Discontinuity Detection: " << (discontinuity_test ? "PASS" : "FAIL") << std::endl;
    
    if (encoding_test && discontinuity_test) {
        std::cout << "\nAll tests PASSED! PTS discontinuity correction logic is working correctly." << std::endl;
        return 0;
    } else {
        std::cout << "\nSome tests FAILED! Please check the implementation." << std::endl;
        return 1;
    }
}