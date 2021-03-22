#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nvs_flash.h"

#include "driver/gpio.h"

#include "lwip/sockets.h"
#include "lwip/sys.h"

#define GPIO0   0   // Relay
#define GPIO2   2   // LED

#define WIFI_AP_SSID            "ESP-01S"
#define WIFI_AP_PASS            ""
#define WIFI_AP_MAX_CONNECTION  5

static char wifi_sta_ssid[32] = "SSID";
static char wifi_sta_password[64] = "PASSWORD";

static const char *TAG = "ESP-01S";
const int IPV4_GOTIP_BIT = BIT0;

static nvs_handle nvs;
static EventGroupHandle_t wifi_event_group;

static uint64_t led_period = 250;

#define HTTP_GET    0
#define HTTP_POST   1

static const char *RESPONSE_404 = "HTTP/1.1 404 Not Found\r\n"
    "Server: ESP-01s/0.1.0\r\n"
    "Content-Type: text/html\r\n"
    "Content-Length: 13\r\n"
    "\r\n"
    "404 Not Found";

static const char *RESPONSE_CONTROL = "HTTP/1.1 200 OK\r\n"
    "Server: ESP-01s/0.1.0\r\n"
    "Content-Type: text/html\r\n"
    "Content-Length: 319\r\n"
    "\r\n"
    "<html>"
    "<body>"
    "<a href=\"/c\">Control</a>|<a href=\"/s\">Settings</a>"    
    "<form action=\"/c\" method=\"POST\">"
    "<input type=\"hidden\" name=\"turn\" value=\"0\"/>"
    "<input type=\"submit\" value=\"Turn OFF\"/>"
    "</form>"
    "<form action=\"/c\" method=\"POST\">"
    "<input type=\"hidden\" name=\"turn\" value=\"1\"/>"
    "<input type=\"submit\" value=\"Turn ON\"/>"
    "</form>"
    "</body>"
    "</html>";

static const char *RESPONSE_SETTING = "HTTP/1.1 200 OK\r\n"
    "Server: ESP-01s/0.1.0\r\n"
    "Content-Type: text/html\r\n"
    "Content-Length: 242\r\n"
    "\r\n"
    "<html>"
    "<body>"
    "<a href=\"/c\">Control</a>|<a href=\"/s\">Settings</a>"
    "<form action=\"/s\" method=\"POST\">"
    "<p>SSID:<input type=\"text\" name=\"ssid\"/></p>"
    "<p>PASSWORD:<input type=\"text\" name=\"pass\"/></p>"
    "<input type=\"submit\" value=\"Save\"/>"
    "</form>"
    "</body>"
    "</html>";

char rx_buffer[1024];
char tx_buffer[2048];

static int save_wifi_ssid_pass(char *ssid, char *pass)
{
    esp_err_t err;

    err = nvs_set_str(nvs, "WIFI_SSID", ssid);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nvs_set_str()...0x%x", err);
        return -1;
    }

    err = nvs_set_str(nvs, "WIFI_PASS", pass);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nvs_set_str()...0x%x", err);
        return -1;
    }

    return 0;
}

static int http_request_parse_method(const char *raw, int *method)
{
    if (strncmp(raw, "GET", 3) == 0) {
        *method = HTTP_GET;
    } else if (strncmp(raw, "POST", 4) == 0) {
        *method = HTTP_POST;
    } else {
        *method = -1;
        return -1;
    }
    return 0;
}

static int http_request_parse_url(const char *raw, char *url)
{
    char* p1;
    char* p2;
    int len;
    p1 = strstr(raw, " ");
    if (p1 == NULL) {
        return -1;
    }
    p1++;

    p2 = strstr(p1, " ");
    if (p2 == NULL) {
        return -1;
    }

    len = p2 - p1;
    strncpy(url, p1, len);
    url[len] = 0;

    return 0;
}

static int http_request_parse_content(const char *raw, char *content)
{
    char* p;
    p = strstr(raw, "\r\n\r\n");
    if (p == NULL) {
        return -1;
    }

    if ((p+4) == 0) {
        content[0] = 0;
    } else {
        strcpy(content, p+4);
    }

    return 0;


}

static int http_content_parse_ssid(const char *content, char *ssid, char *pass)
{
    char *p1;
    char *p2;

    p1 = strstr(content, "ssid=");
    if (!p1) {
        return -1;
    }
    p1 += 5;
    p2 = strstr(p1, "&");
    if (p2) {
        strncpy(ssid, p1, p2-p1);
        ssid[p2-p1] = '\0';
    } else {
        strcpy(ssid, p1);
    }

    p1 = strstr(content, "pass=");
    if (!p1) {
        return -1;
    }
    p1 += 5;
    p2 = strstr(p1, "&");
    if (p2) {
        strncpy(pass, p1, p2-p1);
    } else {
        strcpy(pass, p1);
    }

    return 0;
}

static int http_content_parse_turn(const char *content, char *turn)
{
    char *p1;
    char *p2;

    p1 = strstr(content, "turn=");
    if (!p1) {
        return -1;
    }
    p1 += 5;
    p2 = strstr(p1, "&");
    if (p2) {
        strncpy(turn, p1, p2-p1);
        turn[p2-p1] = '\0';
    } else {
        strcpy(turn, p1);
    }

    return 0;
}

static int http_recv(int s, char *mem, size_t len, int flags)
{
    int offset = 0;
    int l;
    char *p;
    int content_length = 0;
    char *content = NULL;
    while (1) {
        l = recv(s, mem + offset, len - offset, flags);
        if (l < 0) {
            return l;
        }
        if (l == 0) {
            return offset;
        }
        offset += l;
        mem[offset] = '\0';


        if (content == NULL) {
            p = strstr(mem, "\r\n\r\n");
            if (p) {
                content = p + 4;
                p = strstr(mem, "Content-Length: ");
                if (p && strstr(p, "\r\n")) {
                    content_length = atoi(&p[16]);
                }
            }
        }

        if (content) {
            if (offset >= (content - mem) + content_length) {
                return offset;
            }
        }
    }
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START");
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_CONNECTED:
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
        xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, IPV4_GOTIP_BIT);
        break;
    case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "station:"MACSTR" join, AID=%d", MAC2STR(event->event_info.sta_connected.mac), event->event_info.sta_connected.aid);
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "station:"MACSTR"leave, AID=%d", MAC2STR(event->event_info.sta_disconnected.mac), event->event_info.sta_disconnected.aid);
        break;
    case SYSTEM_EVENT_AP_STA_GOT_IP6:
    default:
        break;
    }
    return ESP_OK;
}

void http_server(void)
{
    char addr_str[128];
    int addr_family;
    int ip_protocol;
    char ssid[32];
    char pass[64];

    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(80);
    addr_family = AF_INET;
    ip_protocol = IPPROTO_IP;
    inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return;
    }

    int err = bind(listen_sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        return;
    }

    ESP_LOGI(TAG, "Listen...");
    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occured during listen: errno %d", errno);
        return;
    }

    while (1) {
        struct sockaddr_in sourceAddr;
        uint addrLen = sizeof(sourceAddr);
        int sock = accept(listen_sock, (struct sockaddr *)&sourceAddr, &addrLen);
        if (sock < 0) {
            ESP_LOGE(TAG, "Accept()...failed: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Accepted");

        while (1) {
            int len = http_recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                ESP_LOGI(TAG, "http_recv()...failed");
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed");
                break;
            } else {
                inet_ntoa_r(((struct sockaddr_in *)&sourceAddr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
                rx_buffer[len] = 0;
                ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
                ESP_LOGI(TAG, "%s", rx_buffer);

                int method = -1;
                char url[64];
                char content[128];
                err = http_request_parse_method(rx_buffer, &method);
                if (err < 0) {
                    ESP_LOGW(TAG, "request_parse_method()...failed");
                    break;
                }
                ESP_LOGI(TAG, "method: %d", method);

                err = http_request_parse_url(rx_buffer, url);
                if (err < 0) {
                    ESP_LOGW(TAG, "request_parse_method()...failed");
                    break;
                }
                ESP_LOGI(TAG, "URL: %s", url);

                err = http_request_parse_content(rx_buffer, content);
                if (err < 0) {
                    ESP_LOGW(TAG, "request_parse_content()...failed");
                    break;
                }
                ESP_LOGI(TAG, "content: %s", content);

                if (strcmp(url, "/s") == 0) {
                    if (method == HTTP_POST) {
                        err = http_content_parse_ssid(content, ssid, pass);
                        if (err < 0) {
                            ESP_LOGE(TAG, "Parse POST param failed.");
                            break;
                        }
                        ESP_LOGI(TAG, "Setting wifi...ssid: %s, pass: %s", ssid, pass);
                        save_wifi_ssid_pass(ssid, pass);
                    }
                    len = send(sock, RESPONSE_SETTING, strlen(RESPONSE_SETTING), 0);
                    if (len < 0) {
                        ESP_LOGE(TAG, "send()...failed: errno %d", errno);
                        break;
                    }
                    ESP_LOGI(TAG, "Sent %d bytes to %s.", len, addr_str);
                } else if (strcmp(url, "/c") == 0) {
                    if (method == HTTP_POST) {
                        char turn[10];
                        err = http_content_parse_turn(content, turn);
                        if (err < 0) {
                            ESP_LOGE(TAG, "Parse POST param failed.");
                            break;
                        }
                        ESP_LOGI(TAG, "Control: %s", turn);
                        if (turn[0] == '0') {
                            gpio_set_level(GPIO0, 0);
                        } else if (turn[0] == '1') {
                            gpio_set_level(GPIO0, 1);
                        }
                    }
                    len = send(sock, RESPONSE_CONTROL, strlen(RESPONSE_CONTROL), 0);
                    if (len < 0) {
                        ESP_LOGE(TAG, "send()...failed: errno %d", errno);
                        break;
                    }
                    ESP_LOGI(TAG, "Sent %d bytes to %s.", len, addr_str);
                } else {
                    len = send(sock, RESPONSE_404, strlen(RESPONSE_404), 0);
                    if (len < 0) {
                        ESP_LOGE(TAG, "send()...failed: errno %d", errno);
                        break;
                    }
                    ESP_LOGI(TAG, "Sent %d bytes to %s.", len, addr_str);
                }
                break;
            }
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket...");
            shutdown(sock, 0);
            close(sock);
        }
    }

    if (listen_sock != -1) {
            ESP_LOGE(TAG, "Closing listen socket...");
            close(listen_sock);
    }
}

void init_gpio(void) {
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<GPIO0) | (1ULL<<GPIO2);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
}

int init_wifi_sta(void) {
    esp_err_t err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init()...0x%x", err);
        return -1;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage()...0x%x", err);
        return -1;
    }

    wifi_config_t config = {
        .sta = {
            .ssid = "",
            .password = "",
        },
    };
    strcpy((char *)config.sta.ssid, wifi_sta_ssid);
    strcpy((char *)config.sta.password, wifi_sta_password);
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_config()...0x%x", err);
        return -1;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "esp_wifi_start()...0x%x", err);
        return -1;
    }

    ESP_LOGI(TAG, "Waiting 30s for IP...");
    EventBits_t event = xEventGroupWaitBits(wifi_event_group, IPV4_GOTIP_BIT, false, true, 30000/portTICK_RATE_MS);
    if (!(event & IPV4_GOTIP_BIT)) {
        ESP_LOGI(TAG, "Get IP timeout.");
        esp_wifi_stop();
        return -1;
    }

    return 0;
}

int init_wifi_ap(void) {
    esp_err_t err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init()...0x%x", err);
        return -1;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage()...0x%x", err);
        return -1;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(WIFI_MODE_AP)...0x%x", err);
        return -1;
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .password = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONNECTION,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(WIFI_AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    err = esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config()...0x%x", err);
        return -1;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start()...0x%x", err);
        return -1;
    }

    return 0;
}

static int load_wifi_ssid_pass(char *ssid, char *pass)
{
    esp_err_t err;

    size_t len;
    
    len = 32;
    err = nvs_get_str(nvs, "WIFI_SSID", ssid, &len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nvs_get_str()...0x%x", err);
        return -1;
    }

    len = 64;
    err = nvs_get_str(nvs, "WIFI_PASS", pass, &len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nvs_get_str()...0x%x", err);
        return -1;
    }

    return 0;
}

static void led_task(void *arg)
{
    for (int i=0;;i++) {
        gpio_set_level(GPIO2, i % 2);
        vTaskDelay(led_period / portTICK_RATE_MS);
    }
}

void app_main(void)
{
    esp_err_t err;

    init_gpio();

    xTaskCreate(led_task, "led_task", 1024, NULL, 5, NULL);

    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init()...0x%x", err);
        return;
    }

    err = nvs_open("Custom", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open()...02x%x", err);
        return;
    }

    if (load_wifi_ssid_pass(wifi_sta_ssid, wifi_sta_password) == -1) {
        ESP_LOGE(TAG, "Load ssid and password failed.");
        if (save_wifi_ssid_pass(wifi_sta_ssid, wifi_sta_password) == -1) {
            ESP_LOGE(TAG, "Save default ssid and password failed.");
        }
    }

    ESP_LOGI(TAG, "    SSID=%s", wifi_sta_ssid);
    ESP_LOGI(TAG, "PASSWORD=%s", wifi_sta_password);

    tcpip_adapter_init();

    wifi_event_group = xEventGroupCreate();

    err = esp_event_loop_init(event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_init()...0x%x", err);
        return;
    }

    ESP_LOGI(TAG, "Starting STA mode...");
    if (init_wifi_sta() != 0) {
        ESP_LOGI(TAG, "Starting AP mode...");
        if (init_wifi_ap() !=0) {
            return;
        }
    }

    led_period = 1000;
    http_server();
}
