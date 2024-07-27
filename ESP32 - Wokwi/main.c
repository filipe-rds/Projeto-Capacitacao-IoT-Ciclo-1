//  Aluno : Filipe da Silva Rodrigues

#include <stdio.h>
#include <string.h> 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include <Arduino.h>

// Semáforo usado no TCP
static SemaphoreHandle_t tcp_mutex;

// Configurações do sensor de temperatura
#define NTC_PIN GPIO_NUM_34
const float BETA = 3950;
static float global_temperature = 0.0;

// Configurações do módulo Wifi

#define SSID "Wokwi-GUEST"
#define PASSPHARSE ""
#define TCPServerIP "159.203.79.141"
#define PORT 50000


static const char *id = "20222370009";

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
static const char *TAG="tcp_client";

static int sock = -1;


// Assinaturas 
void loginTask(void *pvParam);
void aliveTask(void *pvParam);
float readTemperatureTask();
void sendDataTask(float data, int sock);
void setup();
void app_main();



void wifi_connect() {
    wifi_config_t cfg = {
        .sta = {
            .ssid = SSID,
            .password = PASSPHARSE,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static esp_err_t event_handler(void *ctx, system_event_t *event) {
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void) {
    esp_log_level_set("wifi", ESP_LOG_NONE); // disable wifi driver logging
    tcpip_adapter_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}


void loginTask(void *pvParam){
    int *pSock = (int *)pvParam;

    char rx_buffer[128];
    char tx_buffer[128];
    char host_ip[] = TCPServerIP;
    int addr_family = 0;
    int ip_protocol = 0;
    struct sockaddr_in tcpServerAddr;
    tcpServerAddr.sin_addr.s_addr = inet_addr(TCPServerIP);
    tcpServerAddr.sin_family = AF_INET;
    tcpServerAddr.sin_port = htons(PORT);
    int s, r;
    float data;
    
    xEventGroupWaitBits(wifi_event_group,CONNECTED_BIT,false,true,portMAX_DELAY);

    *pSock =  socket(AF_INET, SOCK_STREAM, 0);

    if (*pSock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return ;
    }
    ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, PORT);

    int err = connect(*pSock, (struct sockaddr *)&tcpServerAddr, sizeof(tcpServerAddr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
        return;
    }
    
    ESP_LOGI(TAG, "Successfully connected");

    err = send(*pSock, id, strlen(id), 0);
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
    }
    else {
      int len = recv(*pSock, rx_buffer, sizeof(rx_buffer) - 1, 0);
      // Error occurred during receiving
      if (len < 0) {
          ESP_LOGE(TAG, "recv failed: errno %d", errno);
      }
      // Data received
      else {
          rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
          if(strcmp(rx_buffer,"ok") == 0){
            printf("Login realizado com sucesso\n");
          }
          else {
            printf("Falha no login");
          }
      }
    }

    if (*pSock != -1) {
        while (1) {  // Loop infinito após o login

          // Processamento e envio de dados
            data = readTemperatureTask();
            sendDataTask(data,*pSock);
            vTaskDelay(pdMS_TO_TICKS(60000));  // Atraso de 60 segundos
        }
    } else {
        ESP_LOGE(TAG, "Conexão com o socket não está estabelecida.");
    }

    // Se a execução chegar aqui, algo deu errado, e a tarefa pode ser encerrada ou reiniciada.
    ESP_LOGE(TAG, "Encerrando a tarefa loginTask.");
    vTaskDelete(NULL);
}

void sendDataTask(float data, int sock) {
    char tx_buffer[128];

    xSemaphoreTake(tcp_mutex, portMAX_DELAY);

    // Verificar se o socket está estabelecido
    if (sock != -1) {
        // Formatar a temperatura como uma string
        snprintf(tx_buffer, sizeof(tx_buffer), "%.2f", data);

        // Enviar dados para o Proxy, utilizando a variável global de temperatura formatada como string
        int err = send(sock, tx_buffer, strlen(tx_buffer), 0);

        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending temperature: errno %d", errno);
            
        } else {
            // Dados enviados com sucesso
            ESP_LOGI(TAG, "Dados enviados com sucesso: %s", tx_buffer);
        }
    } else {
        ESP_LOGE(TAG, "Conexão com o socket não está estabelecida.");
    }
    xSemaphoreGive(tcp_mutex);
}


void aliveTask(void *pvParam) {
    int *pSock = (int *)pvParam;

    char alive_msg[] = "alive";

    for (;;) {
        xSemaphoreTake(tcp_mutex, portMAX_DELAY);

        // Verificar se a conexão está estabelecida (supondo que 'sock' seja a variável do socket)
        if (sock != -1) {
            int err = send(*pSock, alive_msg, strlen(alive_msg), 0);
            if (err < 0) {
                ESP_LOGE(TAG, "Error occurred during sending alive message: errno %d", errno);
                // Lida com erro de envio, reconecta se necessário
                // ...
            } else {
                // Mensagem alive enviada com sucesso
                ESP_LOGI(TAG, "Mensagem alive enviada com sucesso");
            }
        } else {
            ESP_LOGE(TAG, "Conexão com o socket não está estabelecida.");
            // Lida com a ausência de conexão, reconecta se necessário
            // ...
        }

        xSemaphoreGive(tcp_mutex);
        vTaskDelay(pdMS_TO_TICKS(10000));  // Atraso de 10 segundos
    }
}


// Configurações do módulo de temperatura


float readTemperatureTask() {

  int analogValue = analogRead(NTC_PIN);
  float celsius = 1 / (log(1 / (4095.0 / analogValue - 1)) / BETA + 1.0 / 298.15) - 273.15;

  printf("Temperatura: %.2f ℃\n", celsius);
  return celsius;

}

void setup() {
  
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    wifi_event_group = xEventGroupCreate();
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
    initialise_wifi();
    tcp_mutex = xSemaphoreCreateMutex();
    if (tcp_mutex == NULL) {
        printf("Erro ao criar Mutex\n");
    }

}

void app_main() {	
    setup();
    xTaskCreate(&loginTask, "loginTask", 4096, &sock, 5, NULL);
    xTaskCreate(&aliveTask, "aliveTask", 4096, &sock, 5, NULL);
}