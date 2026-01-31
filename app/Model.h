#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>
#include "imgprovider.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes and configures the DetectX Hub client for remote inference.
 *
 * This function connects to the DetectX Hub server, queries its capabilities,
 * and sets up the video capture pipeline. It reads Hub connection settings
 * from the configuration file (hub.url, hub.username, hub.password).
 * It must be called before any inference operations.
 *
 * @return Pointer to a cJSON object containing Hub and video configuration data,
 *         or NULL on failure. This object can be used for downstream configuration needs.
 *         Do not free this object; management is handled internally.
 */
cJSON* Model_Setup(void);

/**
 * @brief Perform remote inference via DetectX Hub and return detected objects.
 *
 * This function extracts the JPEG image from the VDO buffer and sends it to the
 * DetectX Hub server for inference. It transforms the returned detections from
 * Hub's pixel coordinates to normalized [0,1] coordinates.
 *
 * Returns a cJSON array of detection objects. Each detection object includes:
 *   - "label": Detected object class as a string
 *   - "c": Confidence value, 0–100 (integer)
 *   - "x", "y", "w", "h": Detection region (normalized to [0,1], center coordinates)
 *   - "timestamp": Epoch milliseconds of detection
 *
 * @param image  The input JPEG image buffer from VDO. Ownership is not transferred.
 * @return A cJSON array of detection objects. The caller is responsible for freeing this array (cJSON_Delete).
 *         Returns NULL on error (Hub unavailable, network error, etc.)
 */
cJSON* Model_Inference(VdoBuffer* image);

/**
 * @brief Reconnect to Hub with updated settings.
 *
 * Closes existing Hub connection, re-reads settings, and reconnects.
 * Allows updating Hub connection without full application restart.
 *
 * @return Pointer to updated model config JSON, or NULL on failure.
 */
cJSON* Model_Reconnect(void);

/**
 * @brief Clean up and free all Hub client resources.
 *
 * Call this once on shutdown to properly release Hub connection and all associated resources.
 */
void Model_Cleanup(void);

/**
 * @brief Reset/cleanup per-inference state.
 *
 * Currently a no-op for the Hub client, but maintained for API compatibility.
 * May be used in future for cleanup of per-inference buffers or state.
 */
void Model_Reset(void);

#ifdef __cplusplus
}
#endif

#endif // MODEL_H
