# DetectX Client

ACAP application for Axis cameras that performs remote object detection by streaming frames to a DetectX Server.

## Overview

DetectX Client is a lightweight ACAP (Axis Camera Application Platform) application that captures video frames from an Axis camera and sends them to a DetectX Server for inference. The server can detect objects using any TFLite INT8 model (ships with a COCO example model supporting 90 classes). Results are published via MQTT and displayed on the camera's web interface.

## Relationship to DetectX

This project is based on [DetectX](https://github.com/pandosme/DetectX), enabling object detection on older Axis cameras (ARTPEC-7, ARTPEC-8) by using remote inference.

**How it Extends DetectX:**
- **DetectX** (original): All-in-one solution for ARTPEC-9 cameras
- **DetectX Client**: Lightweight client for ANY Axis camera (ARTPEC-7/8/9)
  - Sends frames to [DetectX Server](https://github.com/pandosme/detectx-server) for inference
  - Enables older cameras to perform object detection
  - Can run on the same camera as the server OR on separate cameras

**Use Case**: Upgrade your entire camera fleet to object detection without replacing hardware - just install the server on one ARTPEC-9 camera and clients on all other cameras.

## Features

- **Remote Inference**: Offloads compute to DetectX Server
- **YUV → JPEG Pipeline**: Hardware-accelerated video capture with software JPEG encoding
- **Multiple Scale Modes**: Crop, letterbox, and balanced preprocessing
- **MQTT Integration**: Real-time detection publishing
- **Web UI**: Live detection overlay and configuration
- **Adaptive Capture**: Adjusts capture rate based on detection activity

## Architecture

```
┌──────────────┐
│ Axis Camera  │
│   (ACAP)     │
└──────┬───────┘
       │
       ├─► VDO Stream (NV12/YUV)
       │   └─► RGB Conversion
       │       └─► JPEG Encoding
       │
       ├─► HTTP POST ───────────┐
       │                        │
       │                        ▼
       │                 ┌──────────────┐
       │                 │   DetectX    │
       │                 │    Server    │
       │                 └──────┬───────┘
       │                        │
       ├─◄ Detections ──────────┘
       │
       ├─► MQTT Broker (results)
       │
       └─► Web UI (overlay)
```

## Supported Platforms

| Architecture | Axis Chips | Camera Examples |
|--------------|------------|-----------------|
| **aarch64** | ARTPEC-8, ARTPEC-9 | P3255-LVE, Q1659, Q6155-E |
| **armv7hf** | ARTPEC-7 | M3046-V, P1455-LE, Q6075-E |

## Requirements

- **Camera**: Axis camera with ACAP SDK 3.5+ support
- **Server**: Running [DetectX Server](https://github.com/pandosme/detectx-server)
- **MQTT Broker**: For publishing detection results (optional)
- **Build Tools**: Docker and ACAP SDK

## Quick Start

### 1. Build the Application

**For ARTPEC-8/9 (aarch64):**
```bash
./build.sh --clean
# or specify architecture explicitly:
docker build --build-arg CHIP=aarch64 -t detectx-client .
```

**For ARTPEC-7 (armv7hf):**
```bash
docker build --build-arg CHIP=armv7hf -t detectx-client .
```

This produces: `DetectX_Client_1_0_0_<arch>.eap`

### 2. Install on Camera

**Via Web Interface:**
1. Open camera web interface: `http://<camera-ip>`
2. Go to **Settings → Apps**
3. Click **Add** and upload the `.eap` file
4. Click **Start**

**Via Command Line:**
```bash
# Install
scp DetectX_Client_1_0_0_aarch64.eap root@<camera-ip>:/tmp/
ssh root@<camera-ip> "eap-install.sh install /tmp/DetectX_Client_1_0_0_aarch64.eap"

# Start
ssh root@<camera-ip> "systemctl start detectx_client.service"
```

### 3. Configure

Edit settings via the camera's web interface under **Apps → DetectX Client → Settings**:

```json
{
  "hub": {
    "url": "http://server-ip:8080",
    "username": "",
    "password": ""
  },
  "scaleMode": "crop",
  "captureRate": 10000,
  "adaptiveCaptureEnabled": true,
  "mqtt": {
    "enabled": true,
    "broker": "mqtt://broker-ip:1883",
    "topic": "axis/detections"
  }
}
```

## Configuration Options

### Hub Settings
- `url`: DetectX Server endpoint (e.g., `http://192.168.1.100:8080`)
- `username`: Optional HTTP digest auth username
- `password`: Optional HTTP digest auth password

### Detection Settings
- `scaleMode`: Preprocessing mode
  - `crop`: Center crop to model aspect ratio (1:1)
  - `balanced`: 4:3 crop then stretch to 1:1
  - `letterbox`: Preserve aspect with padding (not recommended)
- `captureRate`: Milliseconds between captures (default: 10000 = 10s)
- `adaptiveCaptureEnabled`: Speed up when detections found

### MQTT Settings
- `enabled`: Enable MQTT publishing
- `broker`: MQTT broker URL
- `topic`: Topic for detection messages

## How It Works

### YUV → JPEG Pipeline

The application uses a unique approach to handle VDO buffer limitations:

1. **Capture in NV12/YUV**: VDO provides frames in YUV format
   - YUV buffers are in accessible memory ✓
   - JPEG buffers are in hardware DMA memory (not accessible) ✗

2. **Convert NV12 → RGB**: Software conversion (~30ms)
   ```c
   uint8_t* rgb = nv12_to_rgb(yuv_buffer, width, height);
   ```

3. **Encode RGB → JPEG**: libjpeg compression (~80ms)
   ```c
   uint8_t* jpeg = rgb_to_jpeg(rgb, width, height, quality);
   ```

4. **Send to Server**: HTTP POST (~235-430ms)
   ```c
   Hub_InferenceJPEG(hub, jpeg, jpeg_size, 0, scale_mode, &error);
   ```

**Total latency**: ~350-550ms per frame

### Why Not JPEG Directly?

VDO can output JPEG, but the buffer memory is hardware-encoded and not accessible from userspace:
- `vdo_buffer_get_data()` returns a pointer to DMA memory
- Any `memcpy()` or `write()` operations fail with `EFAULT` (Bad address)
- Solution: Use accessible YUV format and encode ourselves

## Development

### Building from Source

```bash
# Clone repository
git clone https://github.com/pandosme/detectx-client
cd detectx-client

# Build for your target
./build.sh --clean

# Extract for debugging
CONTAINER_ID=$(docker create detectx-client)
docker cp $CONTAINER_ID:/opt/app ./build
docker rm $CONTAINER_ID
```

### Viewing Logs

```bash
# SSH to camera
ssh root@<camera-ip>

# View application logs
journalctl -u detectx_client.service -f

# Or via syslog
tail -f /var/log/syslog | grep detectx_client
```

### File Structure

```
client/
├── app/
│   ├── main.c           # Application entry point
│   ├── Model.c          # Inference coordination
│   │   ├── NV12 → RGB conversion
│   │   ├── RGB → JPEG encoding
│   │   └── Hub communication
│   ├── Hub.c            # HTTP client for server
│   ├── Video.c          # VDO frame capture
│   ├── imgprovider.c    # VDO stream management
│   ├── Output.c         # Web UI and overlays
│   └── MQTT.c           # MQTT publishing
├── settings/
│   ├── settings.json    # Default configuration
│   ├── events.json      # Event definitions
│   └── mqtt.json        # MQTT settings
├── build.sh             # Build script
├── Dockerfile           # Multi-arch build
└── manifest.json        # ACAP metadata
```

## Troubleshooting

### Application Won't Start

```bash
# Check logs
ssh root@<camera-ip> "journalctl -u detectx_client.service -n 50"

# Common issues:
# - Server URL unreachable → Check network/firewall
# - Invalid settings.json → Restore defaults via web UI
```

### No Detections Received

```bash
# Check server connectivity
curl http://server-ip:8080/local/detectx/health

# Check logs for Hub errors
ssh root@<camera-ip> "journalctl -u detectx_client.service | grep 'Hub:'"

# Verify scale mode matches server expectations
```

### High CPU Usage

- NV12 → RGB conversion is CPU-intensive
- Normal: 10-20% CPU per capture
- Reduce `captureRate` or resolution if needed

### Memory Issues

- Each frame uses ~2.7 MB RGB + ~200 KB JPEG
- Frames are freed immediately after sending
- Check for memory leaks: `top -p $(pidof detectx_client)`

## Performance Metrics

| Operation | Time | CPU |
|-----------|------|-----|
| VDO Capture (NV12) | ~5ms | Low |
| NV12 → RGB | ~30ms | High |
| RGB → JPEG | ~80ms | Medium |
| HTTP POST | ~235-430ms | Low |
| **Total** | **~350-550ms** | **15-25%** |

*Measured on ARTPEC-9 @ 960x960 resolution*

## Known Limitations

1. **YUV Only**: Must use YUV capture (JPEG buffers not accessible)
2. **Software Encoding**: JPEG encoding is CPU-bound
3. **Single Stream**: Only one inference stream at a time
4. **No Local Inference**: Requires DetectX Server for processing

## License

[Your License Here]

## Links

- **Original Project**: [DetectX](https://github.com/pandosme/DetectX) by Fredrik Persson
- **Server**: [DetectX Server](https://github.com/pandosme/detectx-server)
- **Issues**: [Report bugs](https://github.com/pandosme/detectx-client/issues)
- **ACAP Documentation**: [Axis Developer Portal](https://www.axis.com/developer-community)
