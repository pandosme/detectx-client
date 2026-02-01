#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>

#include "ACAP.h"
#include "Model.h"
#include "Video.h"
#include "cJSON.h"
#include "Output.h"
#include "MQTT.h"


#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define APP_PACKAGE	"detectx_client"

cJSON* settings = 0;
cJSON* model = 0;
cJSON* eventsTransition = 0;
cJSON* eventLabelCounter = 0;
GTimer *cleanupTransitionTimer = 0;

// Adaptive capture rate control
static unsigned int capture_rate_ms = 1000;
static unsigned int min_capture_rate_ms = 100;
static gboolean adaptive_rate_enabled = TRUE;

// Store last captured JPEG for UI display
static unsigned char* last_jpeg_data = NULL;
static size_t last_jpeg_size = 0;
static int last_jpeg_width = 0;
static int last_jpeg_height = 0;
static GMutex jpeg_mutex;
static guint capture_timer_id = 0;

// Function to store the inference JPEG (called from Model.c)
void StoreInferenceJPEG(const unsigned char* jpeg_data, size_t jpeg_size, int width, int height) {
	if (!jpeg_data || jpeg_size == 0) {
		return;
	}

	g_mutex_lock(&jpeg_mutex);

	// Free old JPEG if exists
	if (last_jpeg_data) {
		free(last_jpeg_data);
		last_jpeg_data = NULL;
		last_jpeg_size = 0;
	}

	// Allocate and copy new JPEG
	last_jpeg_data = (unsigned char*)malloc(jpeg_size);
	if (last_jpeg_data) {
		memcpy(last_jpeg_data, jpeg_data, jpeg_size);
		last_jpeg_size = jpeg_size;
		last_jpeg_width = width;
		last_jpeg_height = height;
		LOG_TRACE("Stored inference JPEG: %zu bytes (%dx%d)\n", jpeg_size, width, height);
	} else {
		LOG_WARN("Failed to allocate memory for inference JPEG\n");
	}

	g_mutex_unlock(&jpeg_mutex);
}

// Function to get a copy of the stored inference JPEG (called from Output.c)
// Returns malloc'd buffer that caller must free, or NULL if no JPEG available
unsigned char* GetInferenceJPEG(size_t* out_size, int* out_width, int* out_height) {
	if (!out_size) return NULL;

	g_mutex_lock(&jpeg_mutex);

	if (!last_jpeg_data || last_jpeg_size == 0) {
		g_mutex_unlock(&jpeg_mutex);
		*out_size = 0;
		if (out_width) *out_width = 0;
		if (out_height) *out_height = 0;
		return NULL;
	}

	// Allocate and copy JPEG data
	unsigned char* jpeg_copy = (unsigned char*)malloc(last_jpeg_size);
	if (jpeg_copy) {
		memcpy(jpeg_copy, last_jpeg_data, last_jpeg_size);
		*out_size = last_jpeg_size;
		if (out_width) *out_width = last_jpeg_width;
		if (out_height) *out_height = last_jpeg_height;
	} else {
		*out_size = 0;
	}

	g_mutex_unlock(&jpeg_mutex);
	return jpeg_copy;
}

void
ConfigUpdate( const char *setting, cJSON* data) {
	LOG_TRACE("<%s\n",__func__);
	if(!setting || !data)
		return;
	char *json = cJSON_PrintUnformatted(data);
	if( json ) {
		LOG("Config updated: %s: %s\n",setting,json);
		free(json);
	}

	// Auto-reconnect when hub settings are updated
	if (strcmp(setting, "hub") == 0) {
		LOG("Hub settings changed, reconnecting...\n");

		cJSON* new_model = Model_Reconnect();

		if (new_model) {
			// Update global model reference
			// Don't delete old model - ACAP_Set_Config will handle it
			model = new_model;
			ACAP_Set_Config("model", model);

			// Update status
			ACAP_STATUS_SetString("model", "status", "Hub reconnected");
			ACAP_STATUS_SetBool("model", "state", 1);

			LOG("Hub reconnected successfully\n");
		} else {
			ACAP_STATUS_SetString("model", "status", "Hub reconnection failed");
			ACAP_STATUS_SetBool("model", "state", 0);
			LOG_WARN("Hub reconnection failed\n");
		}
	}

	LOG_TRACE("%s>\n",__func__);
}

// Model endpoint handler for reconnect functionality
static void
ACAP_ENDPOINT_model(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);

	if (!method) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid Request Method");
		return;
	}

	// Handle GET request - return current model info
	if (strcmp(method, "GET") == 0) {
		if (model) {
			ACAP_HTTP_Respond_JSON(response, model);
		} else {
			ACAP_HTTP_Respond_Error(response, 503, "Hub not connected");
		}
		return;
	}

	// Handle POST request - reconnect to Hub
	if (strcmp(method, "POST") == 0) {
		// Verify content type
		const char* contentType = ACAP_HTTP_Get_Content_Type(request);
		if (!contentType || strcmp(contentType, "application/json") != 0) {
			ACAP_HTTP_Respond_Error(response, 415, "Unsupported Media Type - Use application/json");
			return;
		}

		// Check for POST data
		if (!request->postData || request->postDataLength == 0) {
			ACAP_HTTP_Respond_Error(response, 400, "Missing POST data");
			return;
		}

		// Parse POST data
		cJSON* params = cJSON_Parse(request->postData);
		if (!params) {
			ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON data");
			return;
		}

		// Check for action
		cJSON* action = cJSON_GetObjectItem(params, "action");
		if (!action || !action->valuestring) {
			cJSON_Delete(params);
			ACAP_HTTP_Respond_Error(response, 400, "Missing action field");
			return;
		}

		if (strcmp(action->valuestring, "reconnect") == 0) {
			LOG("Reconnecting to Hub...\n");

			// Reconnect to Hub
			cJSON* new_model = Model_Reconnect();

			if (new_model) {
				// Update global model reference
				// Don't delete old model - ACAP_Set_Config will handle it
				model = new_model;
				ACAP_Set_Config("model", model);

				// Update status
				ACAP_STATUS_SetString("model", "status", "Hub reconnected");
				ACAP_STATUS_SetBool("model", "state", 1);

				LOG("Hub reconnected successfully\n");
				ACAP_HTTP_Respond_JSON(response, model);
			} else {
				ACAP_STATUS_SetString("model", "status", "Hub reconnection failed");
				ACAP_STATUS_SetBool("model", "state", 0);
				LOG_WARN("Hub reconnection failed\n");
				ACAP_HTTP_Respond_Error(response, 503, "Hub reconnection failed");
			}
		} else {
			cJSON_Delete(params);
			ACAP_HTTP_Respond_Error(response, 400, "Unknown action");
			return;
		}

		cJSON_Delete(params);
		return;
	}

	// Handle unsupported methods
	ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed - Use GET or POST");
}

// Snapshot endpoint handler - serves the last inference JPEG
static void
ACAP_ENDPOINT_snapshot(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);

	if (!method || strcmp(method, "GET") != 0) {
		ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed - Use GET");
		return;
	}

	g_mutex_lock(&jpeg_mutex);

	if (!last_jpeg_data || last_jpeg_size == 0) {
		g_mutex_unlock(&jpeg_mutex);
		ACAP_HTTP_Respond_Error(response, 404, "No inference JPEG available");
		return;
	}

	// Copy JPEG data while holding the lock
	unsigned char* jpeg_copy = (unsigned char*)malloc(last_jpeg_size);
	size_t jpeg_size = last_jpeg_size;

	if (jpeg_copy) {
		memcpy(jpeg_copy, last_jpeg_data, last_jpeg_size);
	}

	g_mutex_unlock(&jpeg_mutex);

	if (!jpeg_copy) {
		ACAP_HTTP_Respond_Error(response, 500, "Memory allocation failed");
		return;
	}

	// Send JPEG response with headers and data
	ACAP_HTTP_Header_FILE(response, "snapshot.jpg", "image/jpeg", jpeg_size);
	ACAP_HTTP_Respond_Data(response, jpeg_size, jpeg_copy);
	free(jpeg_copy);
}


VdoMap *capture_VDO_map = NULL;

int inferenceCounter = 0;
unsigned int inferenceAverage = 0;


gboolean
ImageProcess(gpointer data) {
	LOG_TRACE("<%s: Called (settings=%p, model=%p)\n",__func__, settings, model);
	const char* label = "Undefined";
    struct timeval startTs, endTs;

	LOG_TRACE("%s: Start\n",__func__);

	if( !settings || !model ) {
		LOG_TRACE("%s: Removing source - settings or model is NULL\n", __func__);
		return G_SOURCE_REMOVE;
	}

	LOG_TRACE("%s: Capturing RGB frame\n",__func__);
	VdoBuffer* buffer = Video_Capture_RGB();

	if( !buffer ) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("Image capture failed\n");
		return G_SOURCE_REMOVE;
	}

	LOG_TRACE("%s: Image\n",__func__);
    gettimeofday(&startTs, NULL);
	cJSON* detections = Model_Inference(buffer);
    gettimeofday(&endTs, NULL);
	LOG_TRACE("%s: Done\n",__func__);

	unsigned int inferenceTime = (unsigned int)(((endTs.tv_sec - startTs.tv_sec) * 1000) + ((endTs.tv_usec - startTs.tv_usec) / 1000));
	inferenceCounter++;
	inferenceAverage += inferenceTime;
	if( inferenceCounter >= 10 ) {
		unsigned int avg = inferenceAverage / 10;
		ACAP_STATUS_SetNumber(  "model", "averageTime", avg );

		// Adaptive capture rate: use 4x the average response time (or 2x minimum)
		if (adaptive_rate_enabled && avg > 0) {
			unsigned int new_rate = avg * 4;
			if (new_rate < min_capture_rate_ms) {
				new_rate = min_capture_rate_ms;
			}
			if (new_rate != capture_rate_ms) {
				capture_rate_ms = new_rate;
				LOG("Adaptive rate: %u ms (based on avg response: %u ms)\n", capture_rate_ms, avg);
			}
		}

		inferenceCounter = 0;
		inferenceAverage = 0;
	}

	double timestamp = ACAP_DEVICE_Timestamp();

	//Apply Transform detection data and apply user filters
	cJSON* processedDetections = cJSON_CreateArray();

	// Get video dimensions for coordinate scaling
	unsigned int videoWidth = cJSON_GetObjectItem(model, "videoWidth") ?
	                          cJSON_GetObjectItem(model, "videoWidth")->valueint : 1000;
	unsigned int videoHeight = cJSON_GetObjectItem(model, "videoHeight") ?
	                           cJSON_GetObjectItem(model, "videoHeight")->valueint : 1000;

	// AOI and Size are always in display space (16:9)
	cJSON* aoi = cJSON_GetObjectItem(settings,"aoi");
	if(!aoi) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No aoi settings\n");
		return G_SOURCE_REMOVE;
	}
	unsigned int x1 = cJSON_GetObjectItem(aoi,"x1")?cJSON_GetObjectItem(aoi,"x1")->valueint:100;
	unsigned int y1 = cJSON_GetObjectItem(aoi,"y1")?cJSON_GetObjectItem(aoi,"y1")->valueint:100;
	unsigned int x2 = cJSON_GetObjectItem(aoi,"x2")?cJSON_GetObjectItem(aoi,"x2")->valueint:900;
	unsigned int y2 = cJSON_GetObjectItem(aoi,"y2")?cJSON_GetObjectItem(aoi,"y2")->valueint:900;

	cJSON* size = cJSON_GetObjectItem(settings,"size");
	if(!size) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No size settings\n");
		return G_SOURCE_REMOVE;
	}
	unsigned int minWidth = cJSON_GetObjectItem(size,"x2")->valueint - cJSON_GetObjectItem(size,"x1")->valueint;
	unsigned int minHeight = cJSON_GetObjectItem(size,"y2")->valueint - cJSON_GetObjectItem(size,"y1")->valueint;

	int confidenceThreshold = cJSON_GetObjectItem(settings,"confidence")?cJSON_GetObjectItem(settings,"confidence")->valueint:0.5;
		
	cJSON* detection = detections->child;
	while(detection) {
		unsigned cx = 0;
		unsigned cy = 0;
		unsigned width = 0;
		unsigned height = 0;
		unsigned c = 0;
		label = "Undefined";
		cJSON* property = detection->child;
		while(property) {
			if( strcmp("c",property->string) == 0 ) {
				property->valueint = property->valuedouble * 100;
				property->valuedouble = property->valueint;
				c = property->valueint;
			}
			if( strcmp("x",property->string) == 0 ) {
				property->valueint = property->valuedouble * videoWidth;
				property->valuedouble = property->valueint;
				cx += property->valueint;
			}
			if( strcmp("y",property->string) == 0 ) {
				property->valueint = property->valuedouble * videoHeight;
				property->valuedouble = property->valueint;
				cy += property->valueint;
			}
			if( strcmp("w",property->string) == 0 ) {
				property->valueint = property->valuedouble * videoWidth;
				width = property->valueint;
				property->valuedouble = property->valueint;
				cx += property->valueint / 2;
			}
			if( strcmp("h",property->string) == 0 ) {
				property->valueint = property->valuedouble * videoHeight;
				height = property->valueint;
				property->valuedouble = property->valueint;
				cy += property->valueint / 2;
			}
			if( strcmp("label",property->string) == 0 ) {
				label = property->valuestring;
			}
			property = property->next;
		}
		
		//FILTER DETECTIONS
		// Coordinates are in capture space [0-1000] and match the displayed image
		// No transformation needed since overlay displays the captured image
		unsigned int display_cx = cx;
		unsigned int display_cy = cy;
		unsigned int display_width = width;
		unsigned int display_height = height;

		int insert = 0;
		if( c >= confidenceThreshold && display_cx >= x1 && display_cx <= x2 && display_cy >= y1 && display_cy <= y2 )
			insert = 1;
		if( display_width < minWidth || display_height < minHeight )
			insert = 0;
		cJSON* ignore = cJSON_GetObjectItem(settings,"ignore");
		if( insert && ignore && ignore->type == cJSON_Array && cJSON_GetArraySize(ignore) > 0 ) {
			cJSON* ignoreLabel = ignore->child;
			while( ignoreLabel && insert ) {
				if( strcmp( label, ignoreLabel->valuestring) == 0 )
					insert = 0;
				ignoreLabel = ignoreLabel->next;
			}
		}
		//Add custom filter here.  Set "insert = 0" if you want to exclude the detection

		if( insert ) {
			cJSON_AddNumberToObject( detection, "timestamp", timestamp );
			cJSON_AddItemToArray(processedDetections, cJSON_Duplicate(detection,1));
		}
		detection = detection->next;
	}

	cJSON_Delete( detections );

	Output( processedDetections );
	Model_Reset();

	cJSON_Delete(processedDetections);
	LOG_TRACE("%s>\n",__func__);
	return G_SOURCE_CONTINUE;
}

void HTTP_ENDPOINT_eventsTransition(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {
	if( !eventsTransition )
		eventsTransition = cJSON_CreateObject();
	ACAP_HTTP_Respond_JSON(  response, eventsTransition);
}

static GMainLoop *main_loop = NULL;

static gboolean
signal_handler(gpointer user_data) {
    LOG("Received SIGTERM, initiating shutdown\n");
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}

void
Main_MQTT_Subscription_Message(const char *topic, const char *payload) {
	LOG("Message arrived: %s %s\n",topic,payload);
}

void Main_MQTT_Status(int state) {
    char topic[64];
    cJSON* message = 0;
	LOG_TRACE("<%s\n",__func__);

    switch (state) {
        case MQTT_INITIALIZING:
            LOG("%s: Initializing\n", __func__);
            break;
        case MQTT_CONNECTING:
            LOG("%s: Connecting\n", __func__);
            break;
        case MQTT_CONNECTED:
            LOG("%s: Connected\n", __func__);
            sprintf(topic, "connect/%s", ACAP_DEVICE_Prop("serial"));
            message = cJSON_CreateObject();
            cJSON_AddTrueToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_DISCONNECTING:
            sprintf(topic, "connect/%s", ACAP_DEVICE_Prop("serial"));
            message = cJSON_CreateObject();
            cJSON_AddFalseToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_RECONNECTED:
            LOG("%s: Reconnected\n", __func__);
            break;
        case MQTT_DISCONNECTED:
            LOG("%s: Disconnect\n", __func__);
            break;
    }
	LOG_TRACE("%s>\n",__func__);
	
}

static gboolean
MAIN_STATUS_Timer() {
	ACAP_STATUS_SetNumber("device", "cpu", ACAP_DEVICE_CPU_Average());	
	ACAP_STATUS_SetNumber("device", "network", ACAP_DEVICE_Network_Average());
	return TRUE;
}


int
Setup_SD_Card() {
    const char* sd_mount = "/var/spool/storage/SD_DISK";
    const char* detectx_dir = "/var/spool/storage/SD_DISK/detectx";

    struct stat sb;

    // Check if SD mount point exists and is a directory
    if (stat(sd_mount, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        ACAP_STATUS_SetBool("SDCARD", "available", 0);
        LOG("SD Card not detected");
        return 0;
    }

    // Check if DetectX directory exists
    if (stat(detectx_dir, &sb) != 0) {
        // Not found: try to create the directory with appropriate access rights
        if (mkdir(detectx_dir, 0770) != 0) {
            ACAP_STATUS_SetBool("SDCARD", "available", 0);
	        LOG_WARN("SD Card detected but could not create directory %s: %s\n", detectx_dir, strerror(errno));
            return 0;
        }
    } else if (!S_ISDIR(sb.st_mode)) {
        // Exists but is not a directory
        ACAP_STATUS_SetBool("SDCARD", "available", 0);
        LOG_WARN("Error: SD Card structure propblem\n");
        return 0;
    }

    ACAP_STATUS_SetBool("SDCARD", "available", 1);
	LOG("SD Card is ready to be used\n");
    return 1;
}

int main(void) {
	setbuf(stdout, NULL);
	unsigned int videoWidth = 800;
	unsigned int videoHeight = 600;

	openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);

	// Initialize JPEG storage mutex
	g_mutex_init(&jpeg_mutex);

	ACAP( APP_PACKAGE, ConfigUpdate );
	LOG("------------ %s ----------\n",APP_PACKAGE);

	// Register model endpoint for reconnect functionality
	ACAP_HTTP_Node("model", ACAP_ENDPOINT_model);

	// Register snapshot endpoint to serve inference JPEG
	ACAP_HTTP_Node("snapshot", ACAP_ENDPOINT_snapshot);

	settings = ACAP_Get_Config("settings");
	if(!settings) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No settings found\n");
		return 1;
	}

//	Setup_SD_Card();

	eventLabelCounter = cJSON_CreateObject();

	model = Model_Setup();
	const char* json = cJSON_PrintUnformatted(model);
	if( json ) {
		LOG("Model settings: %s\n",json);
		free( (void*)json );
	}

	if (model) {
		// Set initial model status for UI
		ACAP_STATUS_SetString("model", "status", "Hub connected");
		ACAP_STATUS_SetBool("model", "state", 1);
		ACAP_STATUS_SetNumber("model", "averageTime", 0);
	} else {
		ACAP_STATUS_SetString("model", "status", "Hub connection failed");
		ACAP_STATUS_SetBool("model", "state", 0);
	}

	videoWidth = cJSON_GetObjectItem(model,"videoWidth")?cJSON_GetObjectItem(model,"videoWidth")->valueint:1920;
	videoHeight = cJSON_GetObjectItem(model,"videoHeight")?cJSON_GetObjectItem(model,"videoHeight")->valueint:1080;

	// Read adaptive rate settings
	cJSON* hub_config = cJSON_GetObjectItem(settings, "hub");
	if (hub_config) {
		cJSON* rate = cJSON_GetObjectItem(hub_config, "captureRateMs");
		if (rate && rate->valueint > 0) {
			capture_rate_ms = rate->valueint;
		}
		cJSON* adaptive = cJSON_GetObjectItem(hub_config, "adaptiveRate");
		if (adaptive) {
			adaptive_rate_enabled = cJSON_IsTrue(adaptive);
		}
	}
	LOG("Capture rate: %u ms (adaptive: %s)\n", capture_rate_ms, adaptive_rate_enabled ? "enabled" : "disabled");

	if( model ) {
		ACAP_Set_Config("model", model );
		if( Video_Start_RGB( videoWidth, videoHeight ) ) {
			LOG("Video %ux%u started (JPEG)\n",videoWidth,videoHeight);
		} else {
			LOG_WARN("Video stream for image capture failed\n");
		}
		// Start timer-based capture instead of idle callback
		capture_timer_id = g_timeout_add(capture_rate_ms, ImageProcess, NULL);
	} else {
		LOG_WARN("Model setup failed\n");
	}
	ACAP_Set_Config("model",model);
	Output_init();
	MQTT_Init( Main_MQTT_Status, Main_MQTT_Subscription_Message  );	
	ACAP_Set_Config("mqtt", MQTT_Settings() );
	
	ACAP_DEVICE_CPU_Average();
	ACAP_DEVICE_Network_Average();
	g_timeout_add_seconds( 60 , MAIN_STATUS_Timer, NULL );

    LOG("Entering main loop\n");
	main_loop = g_main_loop_new(NULL, FALSE);
    GSource *signal_source = g_unix_signal_source_new(SIGTERM);
    if (signal_source) {
		g_source_set_callback(signal_source, signal_handler, NULL, NULL);
		g_source_attach(signal_source, NULL);
	} else {
		LOG_WARN("Signal detection failed");
	}
	LOG_TRACE("%s>\n",__func__);	
    g_main_loop_run(main_loop);
	LOG("Terminating and cleaning up %s\n",APP_PACKAGE);

	// Remove capture timer
	if (capture_timer_id > 0) {
		g_source_remove(capture_timer_id);
		capture_timer_id = 0;
	}

	Main_MQTT_Status(MQTT_DISCONNECTING); //Send graceful disconnect message
	MQTT_Cleanup();
    ACAP_Cleanup();
	Model_Cleanup();
    closelog();


    return 0;
}
