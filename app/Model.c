/**
 * Model.c - DetectX Client Model (Remote Inference via Hub)
 *
 * This is a simplified version that connects to DetectX Hub for remote inference.
 * Unlike the original DetectX, this does NOT perform local inference.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <jpeglib.h>

#include "Model.h"
#include "Hub.h"
#include "Video.h"
#include "cJSON.h"
#include "ACAP.h"
#include "vdo-frame.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}

// Hub connection
static HubContext* hub = NULL;
static HubCapabilities caps = {0};

// Video dimensions (from camera, not model)
static unsigned int videoWidth = 1920;
static unsigned int videoHeight = 1080;

cJSON* Model_Setup(void) {
    LOG_TRACE("<%s\n", __func__);

    // Get Hub settings from config
    cJSON* settings = ACAP_Get_Config("settings");
    if (!settings) {
        LOG_WARN("%s: No settings found\n", __func__);
        return NULL;
    }

    cJSON* hub_config = cJSON_GetObjectItem(settings, "hub");
    if (!hub_config) {
        LOG_WARN("%s: No hub configuration found\n", __func__);
        return NULL;
    }

    // Extract Hub connection details
    cJSON* url_item = cJSON_GetObjectItem(hub_config, "url");
    cJSON* user_item = cJSON_GetObjectItem(hub_config, "username");
    cJSON* pass_item = cJSON_GetObjectItem(hub_config, "password");

    const char* hub_url = url_item && url_item->valuestring ? url_item->valuestring : NULL;
    const char* username = user_item && user_item->valuestring && strlen(user_item->valuestring) > 0
                          ? user_item->valuestring : NULL;
    const char* password = pass_item && pass_item->valuestring && strlen(pass_item->valuestring) > 0
                          ? pass_item->valuestring : NULL;

    if (!hub_url) {
        LOG_WARN("%s: Hub URL not configured\n", __func__);
        return NULL;
    }

    // Initialize Hub connection
    hub = Hub_Init(hub_url, username, password);
    if (!hub) {
        LOG_WARN("%s: Failed to initialize Hub connection\n", __func__);
        return NULL;
    }

    // Query Hub capabilities
    if (!Hub_GetCapabilities(hub, &caps)) {
        LOG_WARN("%s: Failed to get Hub capabilities\n", __func__);
        Hub_Cleanup(hub);
        hub = NULL;
        return NULL;
    }

    LOG("Connected to Hub: %s\n", hub_url);
    LOG("Hub model: %dx%dx%d, %d classes\n",
        caps.model_width, caps.model_height, caps.model_channels, caps.num_classes);

    // Calculate optimal capture resolution based on model input and scale mode
    const char* scale_mode = "balanced";  // default
    cJSON* scale_mode_item = cJSON_GetObjectItem(settings, "scaleMode");
    if (scale_mode_item && scale_mode_item->valuestring) {
        scale_mode = scale_mode_item->valuestring;
    }

    // For square models (1:1 aspect ratio like 960x960)
    if (strcmp(scale_mode, "crop") == 0) {
        // Center crop: capture exactly model input size
        videoWidth = caps.model_width;
        videoHeight = caps.model_height;
    } else if (strcmp(scale_mode, "balanced") == 0) {
        // Balanced: 4:3 center-crop squeezed to 1:1
        // Capture 4:3 aspect with height = model input
        videoHeight = caps.model_height;
        videoWidth = (videoHeight * 4) / 3;
    } else {
        // Letterbox: full 16:9 view
        // Capture 16:9 aspect with height = model input
        videoHeight = caps.model_height;
        videoWidth = (videoHeight * 16) / 9;
    }

    // VDO requires resolutions divisible by 8 for hardware encoding
    videoWidth = (videoWidth / 8) * 8;
    videoHeight = (videoHeight / 8) * 8;

    // Check if settings override video dimensions
    cJSON* video_width = cJSON_GetObjectItem(settings, "videoWidth");
    cJSON* video_height = cJSON_GetObjectItem(settings, "videoHeight");
    if (video_width && video_width->valueint > 0) {
        videoWidth = video_width->valueint;
    }
    if (video_height && video_height->valueint > 0) {
        videoHeight = video_height->valueint;
    }

    LOG("Video capture resolution: %ux%u (scale_mode=%s)\n", videoWidth, videoHeight, scale_mode);

    // Calculate video aspect ratio
    const char* videoAspect = "16:9";  // default
    double aspect = (double)videoWidth / (double)videoHeight;
    if (aspect >= 1.7) {
        videoAspect = "16:9";  // ~1.78
    } else if (aspect >= 1.2 && aspect < 1.5) {
        videoAspect = "4:3";   // ~1.33
    } else if (aspect >= 0.9 && aspect <= 1.1) {
        videoAspect = "1:1";   // ~1.0
    }

    // Create model info JSON for main.c
    cJSON* model = cJSON_CreateObject();
    cJSON_AddNumberToObject(model, "videoWidth", videoWidth);
    cJSON_AddNumberToObject(model, "videoHeight", videoHeight);
    cJSON_AddStringToObject(model, "videoAspect", videoAspect);

    // Add Hub information
    cJSON* hub_info = cJSON_CreateObject();
    cJSON_AddStringToObject(hub_info, "url", hub_url);
    cJSON_AddNumberToObject(hub_info, "model_width", caps.model_width);
    cJSON_AddNumberToObject(hub_info, "model_height", caps.model_height);
    cJSON_AddNumberToObject(hub_info, "classes", caps.num_classes);
    cJSON_AddItemToObject(model, "hub", hub_info);

    // Add class labels
    cJSON* classes = cJSON_CreateArray();
    for (int i = 0; i < caps.num_classes; i++) {
        cJSON_AddItemToArray(classes, cJSON_CreateString(caps.class_labels[i]));
    }
    cJSON_AddItemToObject(model, "classes", classes);

    LOG_TRACE("%s>\n", __func__);
    return model;
}

// Helper: Clamp value to 0-255 range
static inline uint8_t clamp_u8(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

// Convert NV12 (YUV420SP) to RGB24
// NV12 format: Y plane (width x height), followed by interleaved UV plane (width x height/2)
static uint8_t* nv12_to_rgb(const uint8_t* nv12, unsigned int width, unsigned int height) {
    size_t rgb_size = width * height * 3;
    uint8_t* rgb = (uint8_t*)malloc(rgb_size);
    if (!rgb) return NULL;

    const uint8_t* y_plane = nv12;
    const uint8_t* uv_plane = nv12 + (width * height);

    for (unsigned int row = 0; row < height; row++) {
        for (unsigned int col = 0; col < width; col++) {
            unsigned int y_index = row * width + col;
            unsigned int uv_index = (row / 2) * width + (col & ~1);

            int y = y_plane[y_index];
            int u = uv_plane[uv_index] - 128;
            int v = uv_plane[uv_index + 1] - 128;

            // YUV to RGB conversion (ITU-R BT.601)
            int r = y + (1.402 * v);
            int g = y - (0.344 * u) - (0.714 * v);
            int b = y + (1.772 * u);

            rgb[y_index * 3 + 0] = clamp_u8(r);
            rgb[y_index * 3 + 1] = clamp_u8(g);
            rgb[y_index * 3 + 2] = clamp_u8(b);
        }
    }

    return rgb;
}

// Encode RGB24 to JPEG
static uint8_t* rgb_to_jpeg(const uint8_t* rgb, unsigned int width, unsigned int height,
                            int quality, unsigned long* jpeg_size) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    uint8_t* jpeg_buf = NULL;
    unsigned long jpeg_len = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    // Output to memory
    jpeg_mem_dest(&cinfo, &jpeg_buf, &jpeg_len);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    // Write scanlines
    JSAMPROW row_pointer[1];
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = (JSAMPROW)&rgb[cinfo.next_scanline * width * 3];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    *jpeg_size = jpeg_len;
    return jpeg_buf;
}

cJSON* Model_Inference(VdoBuffer* buffer) {
    LOG_TRACE("<%s: Starting inference\n", __func__);

    if (!hub || !buffer) {
        LOG_WARN("%s: Hub not initialized or buffer is NULL (hub=%p, buffer=%p)\n", __func__, hub, buffer);
        return cJSON_CreateArray();
    }

    // Get NV12 data from VDO buffer (this is accessible!)
    uint8_t* nv12_data = (uint8_t*)vdo_buffer_get_data(buffer);
    if (!nv12_data) {
        LOG_WARN("%s: Invalid NV12 buffer\n", __func__);
        return cJSON_CreateArray();
    }

    // NV12 size = width × height × 1.5 (Y plane + UV plane)
    size_t nv12_size = (videoWidth * videoHeight * 3) / 2;
    LOG_TRACE("%s: Got NV12 buffer %p, size %zu\n", __func__, nv12_data, nv12_size);

    // Convert NV12 to RGB
    uint8_t* rgb_data = nv12_to_rgb(nv12_data, videoWidth, videoHeight);
    if (!rgb_data) {
        LOG_WARN("%s: Failed to convert NV12 to RGB\n", __func__);
        return cJSON_CreateArray();
    }

    LOG_TRACE("%s: Converted NV12 to RGB\n", __func__);

    // Encode RGB to JPEG
    unsigned long jpeg_size = 0;
    uint8_t* jpeg_data = rgb_to_jpeg(rgb_data, videoWidth, videoHeight, 90, &jpeg_size);
    free(rgb_data);  // Free RGB buffer

    if (!jpeg_data || jpeg_size == 0) {
        LOG_WARN("%s: Failed to encode JPEG\n", __func__);
        return cJSON_CreateArray();
    }

    LOG_TRACE("%s: Encoded JPEG, size %lu bytes\n", __func__, jpeg_size);

    // Get scale mode from settings
    cJSON* settings = ACAP_Get_Config("settings");
    const char* scale_mode = "balanced";
    if (settings) {
        cJSON* scale_mode_item = cJSON_GetObjectItem(settings, "scaleMode");
        if (scale_mode_item && scale_mode_item->valuestring) {
            scale_mode = scale_mode_item->valuestring;
        }
    }

    LOG_TRACE("%s: Sending to Hub with scale_mode=%s, size=%lu\n", __func__, scale_mode, jpeg_size);

    // Send to Hub for inference
    char* error_msg = NULL;
    cJSON* detections = Hub_InferenceJPEG(hub, jpeg_data, jpeg_size, 0, scale_mode, &error_msg);

    // Free JPEG buffer
    free(jpeg_data);

    if (!detections) {
        if (error_msg) {
            LOG_WARN("%s: Hub inference failed: %s\n", __func__, error_msg);
            free(error_msg);
        } else {
            LOG_WARN("%s: Hub inference failed (no error message)\n", __func__);
        }
        return cJSON_CreateArray();
    }

    LOG_TRACE("%s: Received response from Hub with %d detections\n", __func__, cJSON_GetArraySize(detections));

    // Hub returns detections in original image coordinates (after letterboxing)
    // We need to convert from Hub's coordinate system to our display coordinate system
    // Hub uses bbox_pixels in original image size, we use normalized [0,1] coords

    cJSON* normalized_detections = cJSON_CreateArray();
    cJSON* detection = NULL;
    cJSON_ArrayForEach(detection, detections) {
        cJSON* bbox_pixels = cJSON_GetObjectItem(detection, "bbox_pixels");
        cJSON* image_info = cJSON_GetObjectItem(detection, "image");

        if (!bbox_pixels || !image_info) continue;

        // Get original image dimensions from response
        cJSON* img_width = cJSON_GetObjectItem(image_info, "width");
        cJSON* img_height = cJSON_GetObjectItem(image_info, "height");

        if (!img_width || !img_height) continue;

        double orig_w = img_width->valuedouble;
        double orig_h = img_height->valuedouble;

        // Get bbox in pixels
        cJSON* x_pix = cJSON_GetObjectItem(bbox_pixels, "x");
        cJSON* y_pix = cJSON_GetObjectItem(bbox_pixels, "y");
        cJSON* w_pix = cJSON_GetObjectItem(bbox_pixels, "w");
        cJSON* h_pix = cJSON_GetObjectItem(bbox_pixels, "h");

        if (!x_pix || !y_pix || !w_pix || !h_pix) continue;

        // Convert to normalized coordinates [0,1]
        double x_norm = x_pix->valuedouble / orig_w;
        double y_norm = y_pix->valuedouble / orig_h;
        double w_norm = w_pix->valuedouble / orig_w;
        double h_norm = h_pix->valuedouble / orig_h;

        // Convert to center coordinates (YOLO format)
        double cx = x_norm + w_norm / 2.0;
        double cy = y_norm + h_norm / 2.0;

        // Create normalized detection
        cJSON* norm_det = cJSON_CreateObject();

        // Copy label
        cJSON* label = cJSON_GetObjectItem(detection, "label");
        if (label && label->valuestring) {
            cJSON_AddStringToObject(norm_det, "label", label->valuestring);
        }

        // Copy confidence (convert from 0-1 to 0-1, already normalized)
        cJSON* confidence = cJSON_GetObjectItem(detection, "confidence");
        if (confidence) {
            cJSON_AddNumberToObject(norm_det, "c", confidence->valuedouble);
        }

        // Add normalized coordinates (center x, center y, width, height)
        cJSON_AddNumberToObject(norm_det, "x", cx);
        cJSON_AddNumberToObject(norm_det, "y", cy);
        cJSON_AddNumberToObject(norm_det, "w", w_norm);
        cJSON_AddNumberToObject(norm_det, "h", h_norm);

        cJSON_AddItemToArray(normalized_detections, norm_det);
    }

    cJSON_Delete(detections);

    LOG_TRACE("%s: Received %d detections from Hub\n", __func__,
              cJSON_GetArraySize(normalized_detections));
    LOG_TRACE("%s>\n", __func__);

    return normalized_detections;
}

void Model_Reset(void) {
    // No-op for client (no local model state to reset)
    LOG_TRACE("<%s>\n", __func__);
}

cJSON* Model_Reconnect(void) {
    LOG_TRACE("<%s\n", __func__);

    // Clean up existing connection
    Model_Cleanup();

    // Re-initialize with updated settings
    cJSON* model = Model_Setup();

    LOG_TRACE("%s>\n", __func__);
    return model;
}

void Model_Cleanup(void) {
    LOG_TRACE("<%s\n", __func__);

    if (hub) {
        Hub_FreeCapabilities(&caps);
        Hub_Cleanup(hub);
        hub = NULL;
    }

    LOG_TRACE("%s>\n", __func__);
}
