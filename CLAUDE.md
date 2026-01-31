# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DetectX Client is a lightweight ACAP (Axis Camera Application Platform) application that captures video frames from Axis network cameras and sends them to a remote DetectX Server for object detection inference. This enables object detection on older Axis cameras (ARTPEC-7, ARTPEC-8) by offloading compute to a remote server.

**Key Distinction**: Unlike the original DetectX project which performs local inference using larod/DLPU, this client performs **remote inference** via HTTP to a DetectX Server.

This is a native C application that integrates with:
- Axis ACAP SDK (version 12.x)
- VDO (Video Device Object) for camera video capture
- libcurl for HTTP communication with DetectX Server
- Paho MQTT for publishing detection results
- libjpeg/libturbojpeg for JPEG encoding
- FastCGI for web UI backend

## Building and Development

### Build Commands

**Build for all architectures (default):**
```bash
./build.sh
# or with clean build:
./build.sh --clean
```
This builds both aarch64 (ARTPEC-8/9) and armv7hf (ARTPEC-7) packages.

**Build for specific architecture only:**
```bash
./build.sh --arch aarch64   # ARTPEC-8/9 only
./build.sh --arch armv7hf   # ARTPEC-7 only
```

This creates `.eap` files (Axis application packages) ready for installation on cameras.

**Clean build artifacts:**
```bash
cd app && make clean
```

### Build Process

1. `build.sh` uses Docker with the Axis ACAP SDK image
2. The Dockerfile copies `app/` contents and architecture-specific JPEG libraries
3. Runs `acap-build` to compile and package
4. Output is copied back as `.eap` files (e.g., `DetectX_Client_1_0_0_aarch64.eap`)
5. No model files are included - the server handles inference

**Important**: The build requires architecture-specific libjpeg and libturbojpeg libraries in `app/lib/{arch}/`:
- `app/lib/aarch64/` - Currently populated with aarch64 libraries
- `app/lib/armv7hf/` - **Needs armv7hf libraries for ARTPEC-7 builds**

To build for armv7hf, you need to obtain armv7hf-compiled versions of:
- `libjpeg.so.62` (and symlink `libjpeg.so`)
- `libturbojpeg.so.0` (and symlink `libturbojpeg.so`)

### Key Build Dependencies

The Makefile links against these packages (via pkg-config):
- `gio-2.0`, `gio-unix-2.0` - GLib I/O and event loop
- `vdostream` - Video capture
- `fcgi` - FastCGI for HTTP endpoints
- `axevent` - Axis event system
- `libcurl` - HTTP client for server communication

Also links statically: `libjpeg`, `libturbojpeg` (in `app/lib/`)

## Architecture

### Core Components

**main.c**
- Application entry point and main event loop
- Manages application configuration (settings.json, events.json, mqtt.json) through ACAP wrapper
- Coordinates frame capture, inference requests, and output
- Implements adaptive capture rate (speeds up when detections are active)

**Hub.c/h**
- HTTP client for communicating with DetectX Server
- `Hub_Init()`: Establishes connection to server
- `Hub_GetCapabilities()`: Queries server for model info (input size, classes)
- `Hub_InferenceJPEG()`: Sends JPEG image, receives cJSON array of detections
- `Hub_GetHealth()`: Checks server health and queue status
- Uses libcurl with optional HTTP digest authentication

**Model.c/h**
- Coordinates remote inference workflow (does NOT perform local inference)
- `Model_Setup()`: Connects to Hub, queries capabilities, calculates optimal video resolution
- `Model_Inference()`: Captures frame, converts NV12→RGB→JPEG, sends to Hub, returns detections
- `Model_Reset()`: Clears crop cache between inference cycles
- Handles three scale modes: crop, balanced, letterbox

**Video.c/h**
- Abstracts VDO (Video Device Object) frame capture
- `Video_Start_YUV()`: Initialize YUV/NV12 video stream (JPEG buffers are not memory-accessible)
- `Video_Capture_YUV()`: Capture single frame as VdoBuffer
- Returns buffers in NV12 format for RGB conversion

**imgprovider.c/imgutils.c/preprocess.c**
- `nv12_to_rgb()`: Software conversion from NV12/YUV to RGB24 (~30ms)
- `rgb_to_jpeg()`: JPEG encoding using libjpeg (~80ms)
- These modules handle the YUV→RGB→JPEG pipeline required because VDO JPEG buffers are in DMA memory and not accessible from userspace

**Output.c/h** (split into multiple files)
- `Output.c`: Main entry point, processes detections received from server
- `Output_crop_cache.c`: Manages in-memory cache of recent detection crops for web UI
- `Output_http.c`: Posts detection crops to external HTTP endpoints
- `Output_helpers.c`: Event state management (transition debouncing, minimum duration)
- Publishes detections and events to MQTT
- Manages ONVIF event states per label

**MQTT.c/h**
- Paho MQTT client wrapper (version 2.0)
- Handles connection, reconnection, publishing
- `MQTT_Publish()`, `MQTT_Publish_JSON()`, `MQTT_Publish_Binary()`: Publish to topics
- Topics: `{pretopic}/detection/{serial}`, `{pretopic}/event/{serial}/{label}/{state}`, `{pretopic}/crop/{serial}`

**ACAP.c/h**
- Wrapper around Axis ACAP SDK
- Handles HTTP/FastCGI endpoints for web UI backend (registered via `ACAP_HTTP_Node()`)
- Manages persistent JSON configuration files
- Provides device information and status API

**CERTS.c/h**
- Certificate management for MQTT TLS connections

### Configuration Files

**app/manifest.json**
- ACAP package metadata: name "DetectX Client", version, vendor
- HTTP endpoint definitions (FastCGI nodes: app, settings, status, device, model, mqtt, certs, crops)

**app/settings/settings.json**
- Hub connection: URL, username, password, capture rate, adaptive rate
- Scale mode: crop (1:1 center), balanced (4:3→1:1), letterbox (16:9 with padding)
- Detection parameters: confidence threshold, AOI (area of interest), minimum size
- Event settings: stabilization time, minimum event duration, prioritize
- Cropping configuration: active, throttle, output methods (MQTT/HTTP/SD)

**app/settings/mqtt.json**
- MQTT broker connection: address, port, username, password
- TLS configuration
- Topic prefix (pretopic)
- Device metadata: name, location

**app/settings/events.json**
- Event state tracking (typically empty at startup)

### Data Flow

1. **Setup**: `Model_Setup()` connects to Hub, queries capabilities (model size, classes)
2. **Capture**: `Video_Capture_YUV()` gets NV12 frame from camera
3. **Conversion**: Software converts NV12 → RGB24 (~30ms)
4. **Encoding**: Software encodes RGB → JPEG (~80ms)
5. **Inference**: `Hub_InferenceJPEG()` sends JPEG to server via HTTP POST (~235-430ms)
6. **Filtering**: main.c applies AOI, confidence, size filters to returned detections
7. **Output**: `Output(processedDetections)` handles:
   - MQTT detection messages (bounding boxes)
   - Event state management (transition debouncing, per-label state)
   - MQTT event messages (label active/inactive)
   - Crop generation and caching (if enabled)
   - HTTP posting of crops (if configured)
8. **Reset**: `Model_Reset()` cleans up crop buffers
9. Loop repeats with adaptive timing (faster when detections are active)

**Total latency per frame**: ~350-550ms (measured on ARTPEC-9 @ 960x960)

### Web UI

Located in `app/html/`:
- `index.html` - Main detection visualization page
- `mqtt.html` - MQTT configuration
- `advanced.html` - Event/label settings
- `cropping.html` - Detection export configuration
- `about.html` - Status and diagnostics
- `crops.html` - View cached detection crops
- Uses jQuery, Bootstrap, and custom media-stream-player for video overlay

Backend endpoints (FastCGI) are handled in ACAP.c callback functions that return JSON.

## Common Development Tasks

### Modifying Scale Modes

Scale modes control how camera frames are preprocessed before sending to server:
- **crop**: Center crop to 1:1 aspect (captures exactly model input size)
- **balanced**: Capture 4:3 aspect, server squeezes to 1:1
- **letterbox**: Capture 16:9 aspect, server adds padding

Implementation in `Model.c` lines 94-108 calculates capture resolution based on scale mode.

### Adding a New HTTP Endpoint

1. Add entry to `app/manifest.json` httpConfig array
2. Register handler in main.c or ACAP.c: `ACAP_HTTP_Node("endpoint", callback_function)`
3. Implement callback with signature: `void callback(ACAP_HTTP_Response response, const ACAP_HTTP_Request request)`

### Modifying Detection Logic

Key functions to modify:
- **main.c `ImageProcess()`**: Pre-output filtering (AOI, confidence thresholds)
- **Model.c `Model_Inference()`**: Frame preprocessing and Hub communication
- **Output.c `Output()`**: Output routing and event state logic
- **Hub.c `Hub_InferenceJPEG()`**: HTTP request formatting, response parsing

### Changing Video Capture Format

Currently uses NV12/YUV format because VDO JPEG buffers are in hardware DMA memory and not accessible from userspace (`memcpy()` fails with `EFAULT`). If you need to change this:
1. Modify `Video.c` to use different VDO format
2. Update conversion functions in `imgutils.c` accordingly
3. Ensure buffer memory is accessible before attempting to read

### Adding New Configuration Settings

1. Add fields to `app/settings/settings.json` (or create new JSON file)
2. Include file in Dockerfile `acap-build` command with `-a` flag (line 23-26)
3. Access in code via: `cJSON* cfg = ACAP_Get_Config("settings")`
4. Update web UI to expose new settings

## Platform-Specific Notes

**ARTPEC-7 vs ARTPEC-8/9:**
- ARTPEC-7 (armv7hf): Older cameras, slower CPU for RGB conversion/JPEG encoding
- ARTPEC-8/9 (aarch64): Faster CPU, supports higher resolution and frame rates
- Choose capture resolution based on target platform capabilities

**Performance considerations:**
- NV12 → RGB conversion is CPU-intensive (10-20% CPU per capture)
- Reduce capture rate or resolution if CPU usage is too high
- Adaptive capture rate automatically speeds up when detections are active

## Debugging and Troubleshooting

**View application logs:**
On camera via SSH: `journalctl -f -u detectx_client.service`

**Check Hub connectivity:**
Web UI "About" page shows Hub connection status and inference time

**Common issues:**
- Hub not reachable: Check network, firewall, Hub URL in settings
- Slow inference: Check Hub server performance, network latency
- High CPU usage: Reduce capture rate or resolution
- Memory leaks: Recent fixes addressed crop caching issues

**Testing Hub connection:**
```bash
# From camera
curl http://hub-ip:port/local/detectx/capabilities
curl http://hub-ip:port/local/detectx/health
```

## Key Differences from Original DetectX

This is DetectX **Client**, not the original DetectX:

| Feature | DetectX (original) | DetectX Client (this repo) |
|---------|-------------------|----------------------------|
| Inference | Local (larod/DLPU) | Remote (HTTP to server) |
| Target platforms | ARTPEC-8/9 only | ARTPEC-7/8/9 |
| Model files | Bundled in .eap | On server only |
| Latency | ~50-200ms | ~350-550ms |
| Dependencies | larod, libtensorflow | libcurl, libjpeg |
| Use case | High-performance cameras | Legacy camera fleet |

## DetectX Server Integration

This client is **completely dependent** on DetectX Server for inference. Understanding the server API is critical for development.

### Server Requirements

- **Platform**: ARTPEC-9 camera only (server performs local inference via larod/DLPU)
- **Network**: LAN recommended for optimal performance (~235-430ms latency)
- **Port**: 8080 (default)
- **Authentication**: Optional HTTP digest auth
- **Repository**: [https://github.com/pandosme/detectx-server](https://github.com/pandosme/detectx-server)

### Server API Endpoints

All endpoints are under `/local/detectx`:

**GET `/local/detectx/capabilities`**
- Returns model information and supported input formats
- Response includes:
  - `model_name`, `model_width`, `model_height`, `model_channels`
  - `classes`: array of `{id, name}` objects (e.g., 90 COCO classes)
  - `input_formats`: supported preprocessing modes
  - `server_version`, `max_queue_size`
- Called by `Hub_GetCapabilities()` during client setup

**POST `/local/detectx/inference-jpeg`**
- Performs inference on JPEG images
- Request: Binary JPEG data (max 10 MB)
- Optional query param: `?index=N` for dataset validation
- Response: JSON array of detections or 204 No Content
- Called by `Hub_InferenceJPEG()` for each frame

**GET `/local/detectx/health`**
- Returns server status and performance statistics
- Response includes:
  - `running`: boolean status
  - `queue_size`, `queue_full`
  - `avg_inference_ms`, `min_inference_ms`, `max_inference_ms`
  - `total_requests`, `successful`, `failed`
- Called by `Hub_GetHealth()` for monitoring

### Inference Request/Response Format

**Request (JPEG):**
```http
POST /local/detectx/inference-jpeg HTTP/1.1
Content-Type: image/jpeg
Content-Length: 123456

<binary JPEG data>
```

**Response (Success):**
```json
[
  {
    "index": 0,
    "label": "person",
    "class_id": 0,
    "confidence": 0.87,
    "bbox_pixels": {"x": 100, "y": 150, "w": 200, "h": 300},
    "bbox_yolo": {"x": 0.5, "y": 0.4, "w": 0.3, "h": 0.5}
  }
]
```

**Response (No Detections):** 204 No Content
**Response (Queue Full):** 503 Service Unavailable

### Scale Modes

The server supports three preprocessing modes (sent via query parameter):

- **crop**: Center crop to 1:1 aspect ratio
- **balanced**: 4:3 crop then stretch to 1:1
- **letterbox**: Preserve aspect with padding (slowest)

Client selects mode in `settings.json` → server preprocesses accordingly.

### Client-Server Integration Points

**Hub.c functions that call server:**
- `Hub_Init()`: Initializes libcurl connection
- `Hub_GetCapabilities()`: GET `/capabilities` → parses model info
- `Hub_InferenceJPEG()`: POST `/inference-jpeg` → parses detections
- `Hub_GetHealth()`: GET `/health` → parses status

**Model.c functions that use Hub:**
- `Model_Setup()`: Calls `Hub_GetCapabilities()` to configure video resolution
- `Model_Inference()`: Converts frame to JPEG, calls `Hub_InferenceJPEG()`
- `Model_Reconnect()`: Re-establishes connection when settings change

### Development Setup

**Running DetectX Server locally for testing:**
1. Install server on an ARTPEC-9 camera (e.g., Q1659, Q6155-E)
2. Configure server to use desired model (default: YOLOv8 COCO 90 classes)
3. Ensure server is accessible from client camera network
4. Configure client settings.json with server URL:
   ```json
   {
     "hub": {
       "url": "http://192.168.1.100:8080",
       "username": "",
       "password": ""
     }
   }
   ```

**Testing server connectivity:**
```bash
# From client camera or development machine
curl http://192.168.1.100:8080/local/detectx/capabilities
curl http://192.168.1.100:8080/local/detectx/health

# Test inference with sample image
curl -X POST \
  -H "Content-Type: image/jpeg" \
  --data-binary @test.jpg \
  http://192.168.1.100:8080/local/detectx/inference-jpeg
```

### Server Model Support

**Default model:**
- YOLOv8 TensorFlow Lite INT8 quantized
- 640×640 input resolution
- 90 COCO classes (person, car, bicycle, etc.)
- ~2.1 MB file size
- Requires ARTPEC-9 DLPU acceleration

**Custom models:**
Server can be updated with custom TFLite INT8 models. Client automatically adapts to server model dimensions via capabilities endpoint.

### Troubleshooting Integration Issues

**Client can't connect to server:**
1. Check `journalctl -f -u detectx_client.service` for Hub connection errors
2. Verify server URL is correct in settings.json
3. Test server reachability: `curl http://server-ip:8080/local/detectx/health`
4. Check firewall rules between client and server cameras

**Inference timeout or slow responses:**
1. Check server queue status via `/health` endpoint
2. Reduce client capture rate to avoid overwhelming server
3. Verify network latency is acceptable (LAN recommended)
4. Check server logs: `journalctl -f -u detectx_server.service`

**Detection results seem wrong:**
1. Verify scale mode matches expectations (crop vs balanced vs letterbox)
2. Check if server model changed (client may need reconnect)
3. Inspect raw JSON response from server for debugging
4. Verify confidence thresholds are appropriate

**Server queue full (503 errors):**
1. Increase client capture rate (reduce frequency)
2. Check if multiple clients are overwhelming single server
3. Enable adaptive capture rate to reduce load when no detections

## Related Projects

- **DetectX Server**: [https://github.com/pandosme/detectx-server](https://github.com/pandosme/detectx-server) - **Required** server component
- **Original DetectX**: [https://github.com/pandosme/DetectX](https://github.com/pandosme/DetectX) - Local inference version
