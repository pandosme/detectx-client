/**
 * Hub.h - DetectX Hub client for remote inference
 *
 * Communicates with DetectX Hub server for remote object detection inference.
 */

#ifndef HUB_H
#define HUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

typedef struct HubContext HubContext;

/**
 * Hub capabilities information
 */
typedef struct {
    int model_width;
    int model_height;
    int model_channels;
    int num_classes;
    char** class_labels;
    char* server_version;
    int max_queue_size;
} HubCapabilities;

/**
 * Hub health status
 */
typedef struct {
    bool running;
    int queue_size;
    bool queue_full;
    double avg_inference_ms;
    double min_inference_ms;
    double max_inference_ms;
    int total_requests;
    int successful;
    int failed;
} HubHealth;

/**
 * Initialize Hub connection
 *
 * @param hub_url Base URL of Hub server (e.g., "http://192.168.1.100")
 * @param username Username for digest authentication (can be NULL)
 * @param password Password for digest authentication (can be NULL)
 * @return Hub context or NULL on failure
 */
HubContext* Hub_Init(const char* hub_url, const char* username, const char* password);

/**
 * Query Hub capabilities
 *
 * @param ctx Hub context
 * @param caps Output capabilities structure (caller must free class_labels and server_version)
 * @return true on success
 */
bool Hub_GetCapabilities(HubContext* ctx, HubCapabilities* caps);

/**
 * Query Hub health status
 *
 * @param ctx Hub context
 * @param health Output health structure
 * @return true on success
 */
bool Hub_GetHealth(HubContext* ctx, HubHealth* health);

/**
 * Send JPEG image for inference
 *
 * @param ctx Hub context
 * @param jpeg_data JPEG-encoded image data
 * @param jpeg_size Size of JPEG data in bytes
 * @param image_index Optional image index for batch processing
 * @param error_msg Output error message (caller must free)
 * @return cJSON array of detections or NULL on failure
 *
 * Each detection object contains:
 *   - "label": class label string
 *   - "class_id": class ID integer
 *   - "confidence": detection confidence (0-1)
 *   - "bbox_pixels": {x, y, w, h} in original image pixel coordinates
 *   - "bbox_yolo": {x, y, w, h} in normalized YOLO format
 */
cJSON* Hub_InferenceJPEG(HubContext* ctx, const uint8_t* jpeg_data,
                        size_t jpeg_size, int image_index,
                        const char* scale_mode, char** error_msg);

/**
 * Update Hub connection settings
 *
 * @param ctx Hub context
 * @param hub_url New Hub URL (NULL to keep current)
 * @param username New username (NULL to keep current)
 * @param password New password (NULL to keep current)
 * @return true on success
 */
bool Hub_UpdateSettings(HubContext* ctx, const char* hub_url,
                       const char* username, const char* password);

/**
 * Get last request round-trip time in milliseconds
 *
 * @param ctx Hub context
 * @return Request time in ms, or -1 if no requests made
 */
double Hub_GetLastRequestTime(HubContext* ctx);

/**
 * Check if Hub is reachable and healthy
 *
 * @param ctx Hub context
 * @return true if Hub is accessible
 */
bool Hub_IsAvailable(HubContext* ctx);

/**
 * Cleanup and free Hub context
 *
 * @param ctx Hub context
 */
void Hub_Cleanup(HubContext* ctx);

/**
 * Free capabilities structure
 *
 * @param caps Capabilities to free
 */
void Hub_FreeCapabilities(HubCapabilities* caps);

#endif  // HUB_H
