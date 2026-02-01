/**
 * Hub.c - DetectX Hub client implementation
 */

#include "Hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/time.h>
#include <curl/curl.h>


#define LOG(fmt, args...)    { LOG_TRACE(fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { LOG_TRACE(fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define HUB_TIMEOUT_SECS 30
#define HUB_CONNECTTIMEOUT_SECS 10

struct HubContext {
    char* hub_url;
    char* username;
    char* password;
    CURL* curl;
    double last_request_time_ms;
    bool available;
};

/* Response buffer for curl */
typedef struct {
    char* data;
    size_t size;
} ResponseBuffer;

/* Callback for curl to write response data */
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    ResponseBuffer* mem = (ResponseBuffer*)userp;

    char* ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        LOG_WARN("Hub: realloc failed in write_callback");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

HubContext* Hub_Init(const char* hub_url, const char* username, const char* password) {
    if (!hub_url) {
        LOG_WARN("Hub_Init: hub_url is NULL");
        return NULL;
    }

    HubContext* ctx = calloc(1, sizeof(HubContext));
    if (!ctx) {
        LOG_WARN("Hub_Init: failed to allocate context");
        return NULL;
    }

    ctx->hub_url = strdup(hub_url);
    ctx->username = username ? strdup(username) : NULL;
    ctx->password = password ? strdup(password) : NULL;
    ctx->last_request_time_ms = -1;
    ctx->available = false;

    /* Initialize curl */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    ctx->curl = curl_easy_init();
    if (!ctx->curl) {
        LOG_WARN("Hub_Init: curl_easy_init failed");
        free(ctx->hub_url);
        free(ctx->username);
        free(ctx->password);
        free(ctx);
        return NULL;
    }

    LOG_TRACE("Hub: initialized with URL %s", hub_url);
    return ctx;
}

bool Hub_UpdateSettings(HubContext* ctx, const char* hub_url,
                       const char* username, const char* password) {
    if (!ctx) return false;

    if (hub_url) {
        free(ctx->hub_url);
        ctx->hub_url = strdup(hub_url);
    }

    if (username) {
        free(ctx->username);
        ctx->username = strdup(username);
    }

    if (password) {
        free(ctx->password);
        ctx->password = strdup(password);
    }

    LOG_TRACE("Hub: updated settings, URL=%s", ctx->hub_url);
    return true;
}

static cJSON* hub_request_json(HubContext* ctx, const char* endpoint) {
    if (!ctx || !endpoint) return NULL;

    struct timeval start, end;
    gettimeofday(&start, NULL);

    ResponseBuffer resp = {0};
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->hub_url, endpoint);

    curl_easy_reset(ctx->curl);
    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, (void*)&resp);
    curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, HUB_TIMEOUT_SECS);
    curl_easy_setopt(ctx->curl, CURLOPT_CONNECTTIMEOUT, HUB_CONNECTTIMEOUT_SECS);

    if (ctx->username && ctx->password) {
        curl_easy_setopt(ctx->curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(ctx->curl, CURLOPT_USERNAME, ctx->username);
        curl_easy_setopt(ctx->curl, CURLOPT_PASSWORD, ctx->password);
    }

    CURLcode res = curl_easy_perform(ctx->curl);

    gettimeofday(&end, NULL);
    ctx->last_request_time_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                                (end.tv_usec - start.tv_usec) / 1000.0;

    if (res != CURLE_OK) {
        LOG_WARN("Hub: request to %s failed: %s", url, curl_easy_strerror(res));
        free(resp.data);
        ctx->available = false;
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        LOG_WARN("Hub: request to %s returned HTTP %ld", url, http_code);
        free(resp.data);
        ctx->available = false;
        return NULL;
    }

    cJSON* json = cJSON_Parse(resp.data);
    free(resp.data);

    if (!json) {
        LOG_WARN("Hub: failed to parse JSON response from %s", url);
        ctx->available = false;
        return NULL;
    }

    ctx->available = true;
    return json;
}

bool Hub_GetCapabilities(HubContext* ctx, HubCapabilities* caps) {
    if (!ctx || !caps) return false;

    memset(caps, 0, sizeof(HubCapabilities));

    cJSON* json = hub_request_json(ctx, "/local/detectx/capabilities");
    if (!json) return false;

    cJSON* model = cJSON_GetObjectItem(json, "model");
    if (!model) {
        LOG_WARN("Hub: capabilities missing 'model' object");
        cJSON_Delete(json);
        return false;
    }

    cJSON* width = cJSON_GetObjectItem(model, "input_width");
    cJSON* height = cJSON_GetObjectItem(model, "input_height");
    cJSON* channels = cJSON_GetObjectItem(model, "channels");
    cJSON* classes = cJSON_GetObjectItem(model, "classes");
    cJSON* max_queue = cJSON_GetObjectItem(model, "max_queue_size");
    cJSON* version = cJSON_GetObjectItem(json, "version");

    if (!width || !height || !channels || !classes) {
        LOG_WARN("Hub: capabilities missing required fields");
        cJSON_Delete(json);
        return false;
    }

    caps->model_width = width->valueint;
    caps->model_height = height->valueint;
    caps->model_channels = channels->valueint;
    caps->num_classes = cJSON_GetArraySize(classes);
    caps->max_queue_size = max_queue ? max_queue->valueint : 10;
    caps->server_version = version && version->valuestring ? strdup(version->valuestring) : strdup("unknown");

    /* Extract class labels */
    caps->class_labels = calloc(caps->num_classes, sizeof(char*));
    for (int i = 0; i < caps->num_classes; i++) {
        cJSON* class_obj = cJSON_GetArrayItem(classes, i);
        cJSON* name = cJSON_GetObjectItem(class_obj, "name");
        if (name && name->valuestring) {
            caps->class_labels[i] = strdup(name->valuestring);
        } else {
            caps->class_labels[i] = strdup("unknown");
        }
    }

    LOG_TRACE("Hub: capabilities - model %dx%dx%d, %d classes",
           caps->model_width, caps->model_height, caps->model_channels, caps->num_classes);

    cJSON_Delete(json);
    return true;
}

bool Hub_GetHealth(HubContext* ctx, HubHealth* health) {
    if (!ctx || !health) return false;

    memset(health, 0, sizeof(HubHealth));

    cJSON* json = hub_request_json(ctx, "/local/detectx/health");
    if (!json) return false;

    cJSON* running = cJSON_GetObjectItem(json, "running");
    cJSON* queue_size = cJSON_GetObjectItem(json, "queue_size");
    cJSON* queue_full = cJSON_GetObjectItem(json, "queue_full");
    cJSON* timing = cJSON_GetObjectItem(json, "timing");
    cJSON* stats = cJSON_GetObjectItem(json, "statistics");

    health->running = running && cJSON_IsTrue(running);
    health->queue_size = queue_size ? queue_size->valueint : 0;
    health->queue_full = queue_full && cJSON_IsTrue(queue_full);

    if (timing) {
        cJSON* avg = cJSON_GetObjectItem(timing, "average_ms");
        cJSON* min = cJSON_GetObjectItem(timing, "min_ms");
        cJSON* max = cJSON_GetObjectItem(timing, "max_ms");
        health->avg_inference_ms = avg ? avg->valuedouble : 0;
        health->min_inference_ms = min ? min->valuedouble : 0;
        health->max_inference_ms = max ? max->valuedouble : 0;
    }

    if (stats) {
        cJSON* total = cJSON_GetObjectItem(stats, "total_requests");
        cJSON* success = cJSON_GetObjectItem(stats, "successful");
        cJSON* failed = cJSON_GetObjectItem(stats, "failed");
        health->total_requests = total ? total->valueint : 0;
        health->successful = success ? success->valueint : 0;
        health->failed = failed ? failed->valueint : 0;
    }

    cJSON_Delete(json);
    return true;
}

// Read callback for curl - reads data in chunks from VDO buffer
typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
} ReadContext;

static size_t read_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    ReadContext* ctx = (ReadContext*)userdata;
    size_t bytes_to_read = size * nitems;
    size_t bytes_remaining = ctx->size - ctx->pos;

    if (bytes_remaining == 0) {
        return 0;  // EOF
    }

    if (bytes_to_read > bytes_remaining) {
        bytes_to_read = bytes_remaining;
    }

    // Try to read in small chunks to avoid issues with special memory
    memcpy(buffer, ctx->data + ctx->pos, bytes_to_read);
    ctx->pos += bytes_to_read;

    return bytes_to_read;
}

cJSON* Hub_InferenceJPEG(HubContext* ctx, const uint8_t* jpeg_data,
                        size_t jpeg_size, int image_index,
                        const char* scale_mode, char** error_msg) {
    if (!ctx || !jpeg_data || jpeg_size == 0) {
        if (error_msg) *error_msg = strdup("Invalid parameters");
        return NULL;
    }

    struct timeval start, end;
    gettimeofday(&start, NULL);

    ResponseBuffer resp = {0};
    char url[512];
    if (scale_mode && strlen(scale_mode) > 0) {
        snprintf(url, sizeof(url), "%s/local/detectx/inference-jpeg?index=%d&scale_mode=%s",
                 ctx->hub_url, image_index, scale_mode);
    } else {
        snprintf(url, sizeof(url), "%s/local/detectx/inference-jpeg?index=%d",
                 ctx->hub_url, image_index);
    }

    LOG_TRACE("Hub: Sending inference request to %s (size=%zu)", url, jpeg_size);

    // Use read callback instead of POSTFIELDS to handle special VDO memory
    ReadContext read_ctx = {
        .data = jpeg_data,
        .size = jpeg_size,
        .pos = 0
    };

    curl_easy_reset(ctx->curl);
    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(ctx->curl, CURLOPT_READFUNCTION, read_callback);
    curl_easy_setopt(ctx->curl, CURLOPT_READDATA, &read_ctx);
    curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDSIZE, (long)jpeg_size);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, (void*)&resp);
    curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, HUB_TIMEOUT_SECS);
    curl_easy_setopt(ctx->curl, CURLOPT_CONNECTTIMEOUT, HUB_CONNECTTIMEOUT_SECS);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: image/jpeg");
    curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, headers);

    if (ctx->username && ctx->password) {
        curl_easy_setopt(ctx->curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(ctx->curl, CURLOPT_USERNAME, ctx->username);
        curl_easy_setopt(ctx->curl, CURLOPT_PASSWORD, ctx->password);
    }

    CURLcode res = curl_easy_perform(ctx->curl);
    curl_slist_free_all(headers);

    gettimeofday(&end, NULL);
    ctx->last_request_time_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                                (end.tv_usec - start.tv_usec) / 1000.0;

    LOG_TRACE("Hub: Request completed in %.2f ms (curl_result=%d)", ctx->last_request_time_ms, res);

    if (res != CURLE_OK) {
        if (error_msg) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Request failed: %s", curl_easy_strerror(res));
            *error_msg = strdup(buf);
        }
        LOG_WARN("Hub: inference request failed: %s", curl_easy_strerror(res));
        free(resp.data);
        ctx->available = false;
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code == 204) {
        /* No detections */
        free(resp.data);
        ctx->available = true;
        return cJSON_CreateArray();
    }

    if (http_code != 200) {
        if (error_msg) {
            char buf[256];
            snprintf(buf, sizeof(buf), "HTTP %ld", http_code);
            *error_msg = strdup(buf);
        }
        LOG_WARN("Hub: inference request returned HTTP %ld", http_code);
        free(resp.data);
        ctx->available = (http_code == 503);  /* Queue full - Hub is alive */
        return NULL;
    }

    // Log the raw response for debugging
    syslog(LOG_INFO, "Hub: Raw server response: %.500s%s",
           resp.data ? resp.data : "(null)",
           (resp.data && strlen(resp.data) > 500) ? "..." : "");

    cJSON* json = cJSON_Parse(resp.data);
    free(resp.data);

    if (!json) {
        if (error_msg) *error_msg = strdup("Failed to parse response");
        LOG_WARN("Hub: failed to parse inference response");
        ctx->available = false;
        return NULL;
    }

    // Extract the detections array from the wrapper object
    cJSON* detections = cJSON_GetObjectItem(json, "detections");
    if (!detections || !cJSON_IsArray(detections)) {
        if (error_msg) *error_msg = strdup("Response missing detections array");
        LOG_WARN("Hub: response missing detections array");
        cJSON_Delete(json);
        ctx->available = false;
        return NULL;
    }

    // Detach the array from the root object so we can delete the wrapper
    cJSON* detections_array = cJSON_DetachItemFromObject(json, "detections");
    cJSON_Delete(json);  // Delete the wrapper object

    // Log parsed detection count
    int detection_count = cJSON_GetArraySize(detections_array);
    syslog(LOG_INFO, "Hub: Parsed %d detections from response", detection_count);

    ctx->available = true;
    return detections_array;
}

double Hub_GetLastRequestTime(HubContext* ctx) {
    return ctx ? ctx->last_request_time_ms : -1;
}

bool Hub_IsAvailable(HubContext* ctx) {
    if (!ctx) return false;

    /* Quick health check */
    HubHealth health;
    if (Hub_GetHealth(ctx, &health)) {
        return health.running;
    }

    return false;
}

void Hub_FreeCapabilities(HubCapabilities* caps) {
    if (!caps) return;

    if (caps->class_labels) {
        for (int i = 0; i < caps->num_classes; i++) {
            free(caps->class_labels[i]);
        }
        free(caps->class_labels);
    }

    free(caps->server_version);
    memset(caps, 0, sizeof(HubCapabilities));
}

void Hub_Cleanup(HubContext* ctx) {
    if (!ctx) return;

    if (ctx->curl) {
        curl_easy_cleanup(ctx->curl);
    }
    curl_global_cleanup();

    free(ctx->hub_url);
    free(ctx->username);
    free(ctx->password);
    free(ctx);

    LOG_TRACE("Hub: cleanup complete");
}
