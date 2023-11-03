#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_SSID CONFIG_ESP_WIFI_SSID
#define WIFI_PASS CONFIG_ESP_WIFI_PASS
#define WIFI_RETRY CONFIG_ESP_MAXIMUM_RETRY

#define WEATHER_API_KEY CONFIG_ESP_WEATHER_KEY
#define WEATHER_API_URL "httpbin.org"

// Event group for communucating when Wi-Fi is connected
static EventGroupHandle_t wifiEventGroup;
#define WIFI_CONN_SUCC BIT0
#define WIFI_CONN_FAIL BIT1

static int connRetries = 0;

static const char *REQUEST =
    "GET "
    "/"
    " HTTP/1.0\r\n"
    "Host: " WEATHER_API_URL
    ":"
    "80"
    "\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";

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

void sendHTTPReq(void)
{
  const struct addrinfo hints = {
      .ai_family = AF_INET,
      .ai_socktype = SOCK_STREAM,
  };
  struct addrinfo *res;
  struct in_addr *addr;
  int s, r;
  char recv_buf[64];

  // Get address struct setup (including DNS lookup)
  int err = getaddrinfo(WEATHER_API_URL, "80", &hints, &res);

  if (err != 0 || res == NULL) {
    ESP_LOGE("HTTP", "DNS lookup failed err=%d res=%p", err, res);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    return;
  }

  addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
  ESP_LOGI("HTTP", "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

  // Allocate a socket to ousrself
  s = socket(res->ai_family, res->ai_socktype, 0);
  if (s < 0) {
    ESP_LOGE("HTTP", "Socket allocation failed");
    freeaddrinfo(res);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    return;
  }
  ESP_LOGI("HTTP", "Socket allocated");

  if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
    ESP_LOGE("HTTP", "Socket failed to connect");
    close(s);
    freeaddrinfo(res);
    vTaskDelay(4000 / portTICK_PERIOD_MS);
    return;
  }

  ESP_LOGI("HTTP", "Socket connected");
  freeaddrinfo(res);

  // Write HTTP GET request packet
  if (write(s, REQUEST, strlen(REQUEST)) < 0) {
    ESP_LOGE("HTTP", "Socket sending failure");
    close(s);
    vTaskDelay(4000 / portTICK_PERIOD_MS);
    return;
  }
  ESP_LOGI("HTTP", "Socket sending success");

  // Await response
  struct timeval receiving_timeout;
  receiving_timeout.tv_sec = 5;
  receiving_timeout.tv_usec = 0;
  if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout, sizeof(receiving_timeout)) < 0) {
    ESP_LOGE("HTTP", "Failed to set a timeout for socket receiving");
    close(s);
    vTaskDelay(4000 / portTICK_PERIOD_MS);
    return;
  }
  ESP_LOGI("HTTP", "Successfully set socket timeout");

  // Print response
  do {
    bzero(recv_buf, sizeof(recv_buf));
    r = read(s, recv_buf, sizeof(recv_buf) - 1);
    for (int i = 0; i < r; i++) {
      putchar(recv_buf[i]);
    }
  } while (r > 0);

  ESP_LOGI("HTTP", "... done reading from socket. Last read return=%d errno=%d.", r, errno);
  close(s);
  vTaskDelay(4000 / portTICK_PERIOD_MS);
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
  sendHTTPReq();
}
