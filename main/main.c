#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_SSID CONFIG_ESP_WIFI_SSID
#define WIFI_PASS CONFIG_ESP_WIFI_PASS
#define WIFI_RETRY CONFIG_ESP_MAXIMUM_RETRY

// Event group for communucating when Wi-Fi is connected
static EventGroupHandle_t wifiEventGroup;
#define WIFI_CONN_SUCC BIT0
#define WIFI_CONN_FAIL BIT1

static int connRetries = 0;

static void wifiEventHandler(void *arg, esp_event_base_t event_base, int32_t event_id,
                             void *event_data)
{
  // Sanity check
  if (event_base != WIFI_EVENT) {
    return;
  }

  switch (event_id) {
    case WIFI_EVENT_STA_START:
      esp_wifi_connect();
      break;

    case WIFI_EVENT_STA_DISCONNECTED:
      if (connRetries < WIFI_RETRY) {
        esp_wifi_connect();
        connRetries++;
      } else {
        xEventGroupSetBits(wifiEventGroup, WIFI_CONN_FAIL);
      }
      break;

    default:
      break;
  }
}

static void ipEventHandler(void *arg, esp_event_base_t event_base, int32_t event_id,
                           void *event_data)
{
  // Sanity check
  if (event_base != IP_EVENT) {
    return;
  }

  switch (event_id) {
    case IP_EVENT_STA_GOT_IP:
      ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
      ESP_LOGI("WIFI", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
      connRetries = 0;
      xEventGroupSetBits(wifiEventGroup, WIFI_CONN_SUCC);
      break;

    default:
      break;
  }
}

void connectWIFI(void)
{
  /* Let's set up Wi-Fi */
  wifiEventGroup = xEventGroupCreate();

  // Call esp_netif_init() to create LwIP task (TCP/IP Stack)
  ESP_ERROR_CHECK(esp_netif_init());

  // Create default event loop (this is where we get WIFI and IP Events to handle)
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Create default network interface to bind with TCP/IP stack
  esp_netif_create_default_wifi_sta();

  // Create and initialize Wi-Fi driver task with default config
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  /* Now we configure the newly created Wi-Fi driver */
  // Handle any Wi-Fi Event (specifically looking for connect and disconnect events)
  esp_event_handler_instance_t instance_any_id;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                      &wifiEventHandler, NULL, &instance_any_id));

  // Handle station getting an IP address event
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                      &ipEventHandler, NULL, &instance_got_ip));

  wifi_config_t wifiConfig = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASS,
          },
  };

  // Set us to station (client) mode
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  // Load in our config settings
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiConfig));
  // Start the Wi-Fi module
  ESP_ERROR_CHECK(esp_wifi_start());

  /* Now we just wait to connect or fail to connect */
  EventBits_t bits = xEventGroupWaitBits(wifiEventGroup, WIFI_CONN_SUCC | WIFI_CONN_FAIL, pdFALSE,
                                         pdFALSE, portMAX_DELAY);

  // Check the EventBits to see what happened
  if (bits & WIFI_CONN_SUCC) {
    ESP_LOGI("WIFI", "connected to ap SSID:%s", WIFI_SSID);
  } else if (bits & WIFI_CONN_FAIL) {
    ESP_LOGI("WIFI", "Failed to connect to SSID:%s", WIFI_SSID);
  } else {
    ESP_LOGE("WIFI", "UNEXPECTED EVENT");
  }
}

void app_main(void)
{
  // Initialize NVS (Store Wi-Fi creds)
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  connectWIFI();
}
