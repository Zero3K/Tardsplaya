// Tardsplaya Browser Player - MediaSource API Implementation
let mediaSource = null;
let sourceBuffer = null;
let isPlaying = false;
let segmentQueue = [];
let isUpdating = false;
let segmentIndex = 0;

function updateStatus(message) {
    document.getElementById('status').textContent = 'Status: ' + message;
}

function startPlayback() {
    if (!('MediaSource' in window)) {
        updateStatus('MediaSource API not supported in this browser');
        return;
    }
    
    const videoElement = document.getElementById('videoPlayer');
    
    // Create MediaSource
    mediaSource = new MediaSource();
    videoElement.src = URL.createObjectURL(mediaSource);
    
    mediaSource.addEventListener('sourceopen', function() {
        updateStatus('MediaSource opened, setting up buffer...');
        
        try {
            // Use MP2T format for MPEG-TS streams
            sourceBuffer = mediaSource.addSourceBuffer('video/mp2t; codecs="avc1.42E01E,mp4a.40.2"');
            
            sourceBuffer.addEventListener('updateend', function() {
                isUpdating = false;
                // Process next segment in queue
                processSegmentQueue();
            });
            
            sourceBuffer.addEventListener('error', function(e) {
                console.error('SourceBuffer error:', e);
                updateStatus('SourceBuffer error - check console');
            });
            
            updateStatus('Buffer ready, starting stream fetch...');
            fetchSegments();
            
        } catch (e) {
            console.error('Failed to create SourceBuffer:', e);
            updateStatus('Failed to create SourceBuffer: ' + e.message);
        }
    });
    
    mediaSource.addEventListener('error', function(e) {
        console.error('MediaSource error:', e);
        updateStatus('MediaSource error - check console');
    });
    
    isPlaying = true;
}

function fetchSegments() {
    if (!isPlaying || !sourceBuffer) return;
    
    fetch('/stream.ts?segment=' + segmentIndex, { 
        method: 'GET',
        cache: 'no-cache'
    })
    .then(response => {
        if (!response.ok) {
            throw new Error('Network response was not ok');
        }
        return response.arrayBuffer();
    })
    .then(data => {
        if (data.byteLength > 0) {
            segmentQueue.push(new Uint8Array(data));
            processSegmentQueue();
            updateStatus('Received segment ' + segmentIndex + ' (' + data.byteLength + ' bytes)');
            segmentIndex++;
        } else {
            updateStatus('No data available, retrying...');
        }
        
        // Continue fetching segments
        if (isPlaying) {
            setTimeout(fetchSegments, 1000); // Fetch every second
        }
    })
    .catch(error => {
        console.error('Fetch error:', error);
        updateStatus('Fetch error: ' + error.message);
        
        // Retry after error
        if (isPlaying) {
            setTimeout(fetchSegments, 2000);
        }
    });
}

function processSegmentQueue() {
    if (isUpdating || segmentQueue.length === 0 || !sourceBuffer) {
        return;
    }
    
    const segment = segmentQueue.shift();
    isUpdating = true;
    
    try {
        sourceBuffer.appendBuffer(segment);
        updateStatus('Processing segment (' + segment.length + ' bytes)');
    } catch (e) {
        console.error('Failed to append buffer:', e);
        updateStatus('Failed to append buffer: ' + e.message);
        isUpdating = false;
    }
}

function stopPlayback() {
    isPlaying = false;
    
    if (mediaSource && mediaSource.readyState === 'open') {
        try {
            mediaSource.endOfStream();
        } catch (e) {
            console.warn('Error ending stream:', e);
        }
    }
    
    const videoElement = document.getElementById('videoPlayer');
    videoElement.src = '';
    
    mediaSource = null;
    sourceBuffer = null;
    segmentQueue = [];
    isUpdating = false;
    segmentIndex = 0;
    
    updateStatus('Playback stopped');
}

function toggleFullscreen() {
    const video = document.getElementById('videoPlayer');
    if (video.requestFullscreen) {
        video.requestFullscreen();
    } else if (video.webkitRequestFullscreen) {
        video.webkitRequestFullscreen();
    } else if (video.msRequestFullscreen) {
        video.msRequestFullscreen();
    }
}

// Auto-start playback when page loads
window.addEventListener('load', function() {
    updateStatus('Page loaded, ready to start');
    setTimeout(startPlayback, 1000);
});

// Handle page unload
window.addEventListener('beforeunload', function() {
    stopPlayback();
});