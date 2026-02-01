# DetectX Client

**Bring AI-powered object detection to your entire Axis camera fleet** — without replacing hardware.

DetectX Client enables any Axis camera (ARTPEC-7, ARTPEC-8, or ARTPEC-9) to perform object detection by sending video frames to a central [DetectX Server](https://github.com/pandosme/detectx-server) for inference. The server runs on a single ARTPEC-9 camera and can serve multiple client cameras simultaneously.

## Why DetectX Client?

### The Problem
- You have a fleet of older Axis cameras (ARTPEC-7/8)
- You want object detection but can't afford to replace all cameras with ARTPEC-9
- Managing separate models on each camera is impractical

### The Solution
Install DetectX Server on **one** ARTPEC-9 camera, install DetectX Client on **all other cameras**, and centralize your AI inference. Your entire camera fleet now has object detection capabilities.

## Use Cases

| Scenario | Setup |
|----------|-------|
| **Legacy Camera Upgrade** | 20 ARTPEC-7 cameras + 1 ARTPEC-9 server = 21 cameras with object detection |
| **Cost Optimization** | Buy one high-end ARTPEC-9 camera instead of upgrading entire fleet |
| **Centralized Model Management** | Update model once on server, all clients benefit immediately |
| **Hybrid Deployment** | Server can also run DetectX Client locally for self-contained operation |

## Features

- ✅ **Remote Inference**: Offload compute to DetectX Server via HTTP
- ✅ **MQTT Integration**: Publish detection results in real-time
- ✅ **Web UI**: Live detection overlay with configurable area-of-interest
- ✅ **Event System**: Trigger ONVIF events based on detected objects
- ✅ **Crop Export**: Save detected objects as JPEG crops (MQTT/HTTP/SD card)
- ✅ **Adaptive Capture**: Automatically speeds up when detections are active
- ✅ **Multi-Platform**: Works on ARTPEC-7, ARTPEC-8, and ARTPEC-9

## Quick Start

### 1. Prerequisites

- **DetectX Server** installed on an ARTPEC-9 camera ([installation guide](https://github.com/pandosme/detectx-server))
- **MQTT Broker** (optional, for publishing results)
- **Docker** (for building the client application)

### 2. Build

```bash
# Clone repository
git clone https://github.com/pandosme/detectx-client
cd detectx-client

# Build for your camera architecture
./build.sh --arch aarch64   # For ARTPEC-8/9
# OR
./build.sh --arch armv7hf   # For ARTPEC-7

# Output: DetectX_Client_1_0_0_<arch>.eap
```

### 3. Install

**Via Camera Web Interface:**
1. Navigate to `http://<camera-ip>` → **Settings** → **Apps**
2. Click **Add** → Upload `DetectX_Client_1_0_0_<arch>.eap`
3. Click **Start**

**Via Command Line:**
```bash
scp DetectX_Client_1_0_0_aarch64.eap root@<camera-ip>:/tmp/
ssh root@<camera-ip> "eap-install.sh install /tmp/DetectX_Client_1_0_0_aarch64.eap"
ssh root@<camera-ip> "systemctl start detectx_client.service"
```

### 4. Configure

Open the camera web interface at `http://<camera-ip>` → **Apps** → **DetectX Client** → **Open**

**Minimum Required Settings:**
- **Hub URL**: `http://<server-camera-ip>:8080` (DetectX Server address)
- **Confidence Threshold**: `30` (minimum detection confidence, 0-100)

**Optional Settings:**
- **MQTT Broker**: Enable to publish detection results
- **Area of Interest**: Define detection region (drag to select)
- **Ignore Labels**: Exclude specific object classes
- **Cropping**: Export detected objects as image crops

## Configuration Guide

### Hub Connection

```json
{
  "hub": {
    "url": "http://192.168.1.100:8080",
    "username": "",           // Optional: HTTP digest auth
    "password": "",           // Optional: HTTP digest auth
    "captureRateMs": 1000,    // Capture interval (ms)
    "adaptiveRate": true      // Speed up when detections found
  }
}
```

### Detection Settings

```json
{
  "confidence": 30,           // Minimum confidence (0-100)
  "scaleMode": "balanced",    // Preprocessing: crop|balanced|letterbox
  "aoi": {                    // Area of interest (0-1000 scale)
    "x1": 0, "y1": 0,
    "x2": 1000, "y2": 1000
  },
  "ignore": ["person"]        // Labels to ignore
}
```

### MQTT Publishing

```json
{
  "mqtt": {
    "broker": "mqtt://192.168.1.50:1883",
    "username": "",
    "password": "",
    "pretopic": "detectx"     // Topic prefix: {pretopic}/detection/{serial}
  }
}
```

### Cropping Output

```json
{
  "cropping": {
    "active": true,
    "leftborder": 10,         // Padding around detection (pixels)
    "rightborder": 10,
    "topborder": 10,
    "bottomborder": 10,
    "mqtt": true,             // Publish crops via MQTT
    "http": false,            // POST crops to HTTP endpoint
    "sdcard": false,          // Save crops to SD card
    "throttle": 500           // Minimum ms between crop exports
  }
}
```

## MQTT Topics

Detection results are published to:

```
{pretopic}/detection/{camera-serial}
```

**Payload Example:**
```json
{
  "label": "car",
  "c": 87,                    // Confidence (0-100)
  "x": 0.54,                  // Center X (normalized 0-1)
  "y": 0.32,                  // Center Y (normalized 0-1)
  "w": 0.15,                  // Width (normalized 0-1)
  "h": 0.08,                  // Height (normalized 0-1)
  "timestamp": 1738449823456
}
```

Event state changes:
```
{pretopic}/event/{camera-serial}/{label}/true   # Object appeared
{pretopic}/event/{camera-serial}/{label}/false  # Object disappeared
```

Cropped images (when enabled):
```
{pretopic}/crop/{camera-serial}
```

## Web Interface

Access the web UI at `http://<camera-ip>` → **Apps** → **DetectX Client** → **Open**

**Pages:**
- **Home**: Live detection overlay with bounding boxes
- **MQTT**: Configure MQTT broker connection
- **Advanced**: Event stabilization and label management
- **Cropping**: Configure detection crop export
- **Crops**: View recent detection crops
- **About**: System status and server connection info

## Supported Cameras

| Architecture | Axis Chips | Example Models |
|--------------|------------|----------------|
| **aarch64** | ARTPEC-8, ARTPEC-9 | P3255-LVE, Q1659, Q6155-E |
| **armv7hf** | ARTPEC-7 | M3046-V, P1455-LE, Q6075-E |

## Troubleshooting

### Connection Issues

**Problem**: Client can't connect to server
```bash
# Test server connectivity from camera
ssh root@<camera-ip>
curl http://<server-ip>:8080/local/detectx/capabilities
curl http://<server-ip>:8080/local/detectx/health
```

**Solution**: Check network connectivity, firewall rules, and server URL in settings

### No Detections

**Problem**: Client connects but no detections appear

1. Check confidence threshold (try lowering to 20)
2. Verify area-of-interest covers the scene
3. Check server logs for errors:
   ```bash
   ssh root@<server-ip> "journalctl -u detectx_server.service -f"
   ```

### Performance Issues

**Problem**: High CPU usage or slow response

- Reduce capture rate: increase `captureRateMs` (default: 1000)
- Check server queue status: `curl http://<server-ip>:8080/local/detectx/health`
- Reduce number of concurrent clients per server

### View Logs

```bash
# Client logs
ssh root@<camera-ip> "journalctl -u detectx_client.service -f"

# Server logs
ssh root@<server-ip> "journalctl -u detectx_server.service -f"
```

## How It Works

```
┌─────────────────┐
│  Client Camera  │
│  (ARTPEC-7/8/9) │
└────────┬────────┘
         │
         │ 1. Capture frame (NV12/YUV)
         │ 2. Convert to JPEG
         │
         ├─── HTTP POST ─────────────┐
         │    (JPEG image)            │
         │                            ▼
         │                     ┌──────────────┐
         │                     │   DetectX    │
         │                     │    Server    │
         │                     │  (ARTPEC-9)  │
         │                     └──────┬───────┘
         │                            │
         │◄── JSON Response ──────────┘
         │    (detections)
         │
         ├──► MQTT Broker
         │    (results)
         │
         └──► Web UI
              (overlay)
```

**Detection Flow:**
1. Client captures video frame every ~1 second
2. Frame sent to DetectX Server via HTTP POST
3. Server performs inference using DLPU acceleration
4. Server returns detected objects (label, confidence, bounding box)
5. Client publishes results via MQTT and displays on web UI
6. Client triggers ONVIF events based on detected labels

## Architecture & Development

For detailed architecture, build process, and development information, see [CLAUDE.md](CLAUDE.md).

## Related Projects

- **DetectX Server**: [https://github.com/pandosme/detectx-server](https://github.com/pandosme/detectx-server) - Required inference server
- **Original DetectX**: [https://github.com/pandosme/DetectX](https://github.com/pandosme/DetectX) - All-in-one solution for ARTPEC-9

## License

Apache License 2.0

## Author

Fredrik Persson ([pandosme](https://github.com/pandosme))

## Support

- **Issues**: [GitHub Issues](https://github.com/pandosme/detectx-client/issues)
- **Discussions**: [GitHub Discussions](https://github.com/pandosme/detectx-client/discussions)
- **Documentation**: [Axis Developer Portal](https://www.axis.com/developer-community)
