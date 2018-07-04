/*
 * Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Additions Copyright 2016 Espressif Systems (Shanghai) PTE LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
/**
 * @file subscribe_publish_sample.c
 * @brief simple MQTT publish and subscribe on the same topic
 *
 * This example takes the parameters from the build configuration and establishes a connection to the AWS IoT MQTT Platform.
 * It subscribes and publishes to the same topic - "test_topic/esp32"
 *
 * Some setup is required. See example README for details.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"

static const char *NETWORK_TAG = "NETWORK";
//static const char *BLE_TAG = "BLE";
static const char *MQTT_TAG = "MQTT";

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID "UFABC"
#define EXAMPLE_WIFI_PASS "85265"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;


static esp_ble_scan_params_t ble_scan_params = {
		.scan_type 			= BLE_SCAN_TYPE_ACTIVE,
		.own_addr_type 		= BLE_ADDR_TYPE_PUBLIC,
		.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
		.scan_interval		= 0x50,
		.scan_window 		= 0x50
};

static int8_t rssi_threshold = -80;

AWS_IoT_Client client;
bool is_client_ready = false;


typedef struct __attribute__((packed)) ble_adv_data
{
	uint32_t pressure;
	int16_t temperature;
	uint16_t humidity;
	uint32_t visible_lux;
	uint32_t infrared_lux;
}ble_adv_data_t;


/* CA Root certificate, device ("Thing") certificate and device
 * ("Thing") key.

   Example can be configured one of two ways:

   "Embedded Certs" are loaded from files in "certs/" and embedded into the app binary.

   "Filesystem Certs" are loaded from the filesystem (SD card, etc.)

   See example README for more details.
*/
extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");

/**
 * @brief Default MQTT HOST URL is pulled from the aws_iot_config.h
 */
char HostAddress[255] = AWS_IOT_MQTT_HOST;

/**
 * @brief Default MQTT port is pulled from the aws_iot_config.h
 */
uint32_t port = AWS_IOT_MQTT_PORT;


static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData) {
    ESP_LOGI(NETWORK_TAG, "Subscribe callback");
    ESP_LOGI(NETWORK_TAG, "%.*s\t%.*s", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);
}

void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(NETWORK_TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
        return;
    }

    if(aws_iot_is_autoreconnect_enabled(pClient)) {
        ESP_LOGI(NETWORK_TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
        ESP_LOGW(NETWORK_TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
        if(NETWORK_RECONNECTED == rc) {
            ESP_LOGW(NETWORK_TAG, "Manual Reconnect Successful");
        } else {
            ESP_LOGW(NETWORK_TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
}

void aws_iot_task(void *param) {
    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

    IoT_Error_t rc = FAILURE;

    ESP_LOGI(NETWORK_TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = HostAddress;
    mqttInitParams.port = port;

    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
    mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;

    mqttInitParams.mqttCommandTimeout_ms = 20000;
    mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    mqttInitParams.disconnectHandlerData = NULL;

    rc = aws_iot_mqtt_init(&client, &mqttInitParams);
    if(SUCCESS != rc) {
        ESP_LOGE(NETWORK_TAG, "aws_iot_mqtt_init returned error : %d ", rc);
        abort();
    }

    /* Wait for WiFI to show as connected */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);

    connectParams.keepAliveIntervalInSec = 10;
    connectParams.isCleanSession = true;
    connectParams.MQTTVersion = MQTT_3_1_1;
    /* Client ID is set in the menuconfig of the example */
    connectParams.pClientID = CONFIG_AWS_EXAMPLE_CLIENT_ID;
    connectParams.clientIDLen = (uint16_t) strlen(CONFIG_AWS_EXAMPLE_CLIENT_ID);
    connectParams.isWillMsgPresent = false;

    ESP_LOGI(NETWORK_TAG, "Connecting to AWS...");
    do {
        rc = aws_iot_mqtt_connect(&client, &connectParams);
        if(SUCCESS != rc) {
            ESP_LOGE(NETWORK_TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
    } while(SUCCESS != rc);

    /*
     * Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
     *  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
     *  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
     */
    rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
    if(SUCCESS != rc) {
        ESP_LOGE(NETWORK_TAG, "Unable to set Auto Reconnect to true - %d", rc);
        abort();
    }

    ESP_LOGI(NETWORK_TAG, "AWS ready...");
    is_client_ready = true;

    while((NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc)) {

        //Max time the yield function will wait for read messages

        if(NETWORK_ATTEMPTING_RECONNECT == rc) {
        	ESP_LOGI(NETWORK_TAG, "AWS disconnect...");
        	is_client_ready = false;
            // If the client is attempting to reconnect we will skip the rest of the loop.
            continue;
        }
        else if(!is_client_ready)
        {
        	is_client_ready = true;
        	ESP_LOGI(NETWORK_TAG, "AWS reconnected...");
        }
        vTaskDelay(1000 / portTICK_RATE_MS);
    }

    ESP_LOGE(NETWORK_TAG, "An error occurred in the main loop.");
    abort();
}

static void initialize_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(NETWORK_TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static void publish_sensor_data(ble_adv_data_t * advdata)
{
	char cPayload[200];

    const char *TOPIC = "sensor/data";
    const int TOPIC_LEN = strlen(TOPIC);
	IoT_Publish_Message_Params paramsQOS0;

	if(!is_client_ready) return;

	sprintf(cPayload, "{\n\"temperature\":%d,"
			"\n\"humidity\":%d,"
			"\n\"pressure\":%d,"
			"\n\"visible_lux\":%d,"
			"\n\"ir_lux\":%d\n}",
    		advdata->temperature,
			advdata->humidity,
			advdata->pressure,
			advdata->visible_lux,
			advdata->infrared_lux);

    paramsQOS0.qos = QOS0;
    paramsQOS0.payload = (void *) cPayload;
    paramsQOS0.payloadLen = strlen(cPayload);
    paramsQOS0.isRetained = 0;

    ESP_LOGI(MQTT_TAG, "Publishing to AWS:\n");
    printf(cPayload);
    printf("\n--------------------------\n");
    aws_iot_mqtt_publish(&client, TOPIC, TOPIC_LEN, &paramsQOS0);
}

ble_adv_data_t decode_sensor_data(esp_ble_gap_cb_param_t *scan_rst)
{
	ble_adv_data_t adv_data;
	memcpy(&adv_data,
		   scan_rst->scan_rst.ble_adv + 7,
		   sizeof(ble_adv_data_t));
	return adv_data;
}

bool msd_nordic_filter(esp_ble_gap_cb_param_t *scan_rst)
{
	static uint8_t ref_data[3] = {0xFF, 0x59, 0x00};
	int32_t result = memcmp(&ref_data[0],
			                scan_rst->scan_rst.ble_adv + 4,
							3);
	if(result != 0)
	{
		return false;
	}
	return true;
}

void set_rssi_filter(int8_t rssi)
{
	rssi_threshold = rssi;
}

bool scan_rssi_filter(esp_ble_gap_cb_param_t *scan_rst)
{
	if(scan_rst->scan_rst.rssi > rssi_threshold)
	{
		return true;
	}
	return false;
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event,
		               esp_ble_gap_cb_param_t *param){
	switch(event)	{
	case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
		esp_ble_gap_start_scanning(0);
		break;

	case ESP_GAP_BLE_SCAN_RESULT_EVT:
		switch(param->scan_rst.search_evt)
		{
		case ESP_GAP_SEARCH_INQ_RES_EVT:
			if(msd_nordic_filter(param)){
				ble_adv_data_t adv_data;
				adv_data = decode_sensor_data(param);
				publish_sensor_data(&adv_data);
			}
			break;

		default:
			break;
		}
		break;
	default:
		break;
	}
}

static void ble_init()
{
	ESP_ERROR_CHECK(nvs_flash_init());
	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	esp_bt_controller_init(&bt_cfg);
	esp_bt_controller_enable(ESP_BT_MODE_BLE);

	esp_bluedroid_init();
	esp_bluedroid_enable();

	esp_ble_gap_register_callback(esp_gap_cb);

	esp_ble_gap_set_scan_params(&ble_scan_params);
}



void app_main()
{

    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    initialize_wifi();
    ESP_LOGI("Start up", "Wifi Connected");
    sleep(10);
    xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 9216, NULL, 5, NULL, 1);

	// initialize BLE
	ble_init();
}
