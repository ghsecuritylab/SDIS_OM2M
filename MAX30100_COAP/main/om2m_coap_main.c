#include <netdb.h>
#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "nvs_flash.h"

#include <om2m/coap.h>

#include "max30100.h"
#include "om2m_coap_config.h"
#include "cJSON.h"

#include <time.h>
#include <stdlib.h>

#define RETRANSMISSION 5000
#define COAP_SERVER_PORT 5683

#define SENSOR          // Enable use of sensor values, if disabled "Communication Test" sent to broker
#define E2E             // Enable to measure end-to-end delay, reduces verbosity
//#define DEBUG_SENSOR  // Disables middlware usage, use only to test sensor communication

#define TEST_CONDITION(condition, true, false) \
  if (condition)                               \
  {                                            \
    true;                                      \
  }                                            \
  else                                         \
  {                                            \
    false;                                     \
  }
#if !defined(E2E)
#define TEST_ASSERT(condition, line, message, true) TEST_CONDITION(condition, true, {ESP_LOGW(line, message);return; })
#else
#define TEST_ASSERT(condition, line, message, true) TEST_CONDITION(condition, true, { return; })
#endif
#define TEST_ASSERT_NULL(pointer, line, message, action) TEST_ASSERT(((pointer) == NULL), line, message, action)
#define TEST_ASSERT_NOT_NULL(pointer, line, message, action) TEST_ASSERT(((pointer) != NULL), line, message, action)

#define TEST_RESPONSE_SET_BIT(response, group, bit)                    \
  {                                                                    \
    TEST_CONDITION(response->hdr->code == COAP_RESPONSE_CODE(403) ||   \
                       response->hdr->code == COAP_RESPONSE_CODE(201), \
                   {xEventGroupSetBits(group, bit);return; },                                                \
                   return;);                                           \
  }

#define TEST_JSON_ASSERT(object, value)                                               \
  {                                                                                   \
    TEST_ASSERT_NOT_NULL(object, value, "Not JSON", /*printf(cJSON_Print(object))*/); \
  }
#define min(a, b) ((a) < (b) ? (a) : (b))

#if !defined(E2E)
const static char *TAG = "coap_om2m_client";
#endif
unsigned int wait_seconds = 90; /* default timeout in seconds */
coap_tick_t max_wait;           /* global timeout (changed by set_timeout()) */

unsigned int obs_seconds = 30; /* default observe time */
coap_tick_t obs_wait = 0;      /* timeout for current subscription */

static inline void set_timeout(coap_tick_t *timer, const unsigned int seconds)
{
  coap_ticks(timer);
  *timer += seconds * COAP_TICKS_PER_SECOND;
}
uint64_t nanos(struct timeval *ts)
{
  return ts->tv_sec * (uint64_t)1000000000L + ts->tv_usec * (uint64_t)1000L;
}
static inline uint64_t get_timestamp()
{
  struct timeval tv;
  struct timezone tz;
  gettimeofday(&tv, &tz);
  return nanos(&tv);
}

// Sensor variables
uint16_t ir_buffer[MAX30100_FIFO_DEPTH];
uint16_t red_buffer[MAX30100_FIFO_DEPTH];
size_t data_len = 0;
double avg;
double diff_avg = 0, alpha = 0.1;

// CoAP/OM2M variables
static EventGroupHandle_t coap_group;
coap_context_t *ctx = NULL;
coap_address_t dst_addr, src_addr;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const static int CONNECTED_BIT = BIT0;
unsigned int AE_BIT = BIT1;
unsigned int CNT_BIT = BIT2;
unsigned int CTRL_BIT = BIT3;
unsigned int SUB_BIT = BIT4;
unsigned int BUFFER_BIT = BIT5;

// Auxiliary functions
static void create_entity(char *entity);
static void create_container(char *entity, char *container);
static void create_sub(char *entity, char *container, char *sub_entity, char *sub_name);

void adjust_current()
{
  while (1)
  {
    max30100_adjust_current();
    vTaskDelay(CURRENT_ADJUSTMENT_PERIOD_MS / portTICK_RATE_MS);
  }
}

/**
 * Reads MAX30100 FIFO data
 * Calculates average HR every 160 ms
 * */
void max30100_updater()
{
  int i;
  while (1)
  {
    max30100_update(ir_buffer, red_buffer, &data_len);
    if (data_len)
    {
      xEventGroupSetBits(coap_group, BUFFER_BIT);

      int sum = 0;
      for (i = 0; i < data_len; i++)
        sum += ir_buffer[i];
      avg = sum / data_len;
    }
    vTaskDelay(160 / portTICK_RATE_MS);
  }
  vTaskDelete(NULL);
}

/**
 * Handles requests received
 * Obtains contents from subscribed containers
 * */
static void request_hanlder(struct coap_context_t *ctx,
                            coap_pdu_t *request)
{

  char *data;
  size_t data_len;
  coap_get_data(request, &data_len, (unsigned char **)&data);

  cJSON *requestJson = cJSON_Parse(data);
  TEST_JSON_ASSERT(requestJson, "requestJson");

  cJSON *m2m_sgn = cJSON_GetObjectItem(requestJson, "m2m:sgn");
  cJSON_Delete(requestJson);
  TEST_JSON_ASSERT(m2m_sgn, "m2m:sgn");

  cJSON *m2m_nev = cJSON_GetObjectItem(m2m_sgn, "m2m:nev");
  cJSON_Delete(m2m_sgn);
  TEST_JSON_ASSERT(m2m_nev, "m2m:nev");

  cJSON *m2m_rep = cJSON_GetObjectItem(m2m_nev, "m2m:rep");
  cJSON_Delete(m2m_nev);
  TEST_JSON_ASSERT(m2m_rep, "m2m:rep");

  cJSON *m2m_cin = cJSON_GetObjectItem(m2m_rep, "m2m:cin");
  cJSON_Delete(m2m_rep);
  TEST_JSON_ASSERT(m2m_cin, "m2m:cin");

  cJSON *con = cJSON_GetObjectItem(m2m_cin, "con");
  cJSON_Delete(m2m_cin);
  TEST_JSON_ASSERT(con, "con");

  char *con_str = cJSON_GetStringValue(con);
  cJSON_Delete(con);
  free(con_str);
}

/**
 * Handles responses received
 * */
static void message_handler(struct coap_context_t *ctx,
                            const coap_endpoint_t *local_interface,
                            const coap_address_t *remote, coap_pdu_t *sent,
                            coap_pdu_t *received, const coap_tid_t id)
{
  char *data = NULL;
  size_t data_len;
  int has_data = coap_get_data(received, &data_len, (unsigned char **)&data);

  if (!(xEventGroupGetBits(coap_group) & AE_BIT))
  {
    TEST_RESPONSE_SET_BIT(received, coap_group, AE_BIT);
  }
  else if (!(xEventGroupGetBits(coap_group) & CNT_BIT))
  {
    TEST_RESPONSE_SET_BIT(received, coap_group, CNT_BIT);
  }
  else if (!(xEventGroupGetBits(coap_group) & SUB_BIT))
  {
    TEST_RESPONSE_SET_BIT(received, coap_group, SUB_BIT);
  }
  else
  {
    if (COAP_RESPONSE_CLASS(received->hdr->code) == COAP_RESPONSE_CLASS(COAP_RESPONSE_201))
    { // Published
      if (has_data)
      {
        double current_timestamp = get_timestamp();

        cJSON *created = cJSON_Parse(data);
        TEST_JSON_ASSERT(created, "Created");

        cJSON *m2m_cin = cJSON_GetObjectItem(created, "m2m:cin");
        cJSON_Delete(created);
        TEST_JSON_ASSERT(m2m_cin, "m2m:cin");

        cJSON *rn = cJSON_GetObjectItem(m2m_cin, "rn");
        TEST_JSON_ASSERT(rn, "rn");

        char *rn_str = cJSON_GetStringValue(rn);
        if (rn_str[0] != 'd')
        {
          cJSON_Delete(rn);
          cJSON_Delete(m2m_cin);
          free(rn_str);
          return;
        }

        cJSON *con = cJSON_GetObjectItem(m2m_cin, "con");
        cJSON_Delete(m2m_cin);
        TEST_JSON_ASSERT(con, "con");

        char *con_str = cJSON_GetStringValue(con);
        cJSON_Delete(con);

        uint64_t received;
        sscanf(con_str, "%lld", (long long *)&received);
        free(con_str);

        uint64_t diff = current_timestamp - received;
        diff_avg = diff_avg * (1 - alpha) + diff * alpha / 2;
      }
    }
  }
}

/**
 * Thread to ping broker
 * sends timestamps, calculates network delay
 * */
static void ping(void *pvParameters)
{
  char data[30];
  char name[30];
  unsigned short int i = 0;
  while (1)
  {
    uint64_t timestamp = get_timestamp();
    sprintf(data, "%lld", (long long)timestamp);
    sprintf(name, "delay_%d", i);
    om2m_coap_create_content_instance(ctx, dst_addr, AE_NAME, PING,
                                      name, data, &i, COAP_REQUEST_POST);
    vTaskDelay(500 / portTICK_RATE_MS);
  }
}

static void coap_context_handler(void *pvParameters)
{
  coap_context_t *ctx = (coap_context_t *)pvParameters;
  fd_set readfds;
  struct timeval tv;
  int result;
  coap_queue_t *nextpdu;
  coap_tick_t now;

  set_timeout(&max_wait, wait_seconds);
  debug("timeout is set to %d seconds\n", wait_seconds);

  while (1)
  {
    FD_ZERO(&readfds);
    FD_SET(ctx->sockfd, &readfds);

    nextpdu = coap_peek_next(ctx);

    coap_ticks(&now);
    if (nextpdu &&
        nextpdu->t < min(obs_wait ? obs_wait : max_wait, max_wait) - now)
    {
      /* set timeout if there is a pdu to send */
      tv.tv_usec = ((nextpdu->t) % COAP_TICKS_PER_SECOND) * 1000000 /
                   COAP_TICKS_PER_SECOND;
      tv.tv_sec = (nextpdu->t) / COAP_TICKS_PER_SECOND;
    }
    else
    {
      /* check if obs_wait fires before max_wait */
      if (obs_wait && obs_wait < max_wait)
      {
        tv.tv_usec = ((obs_wait - now) % COAP_TICKS_PER_SECOND) * 1000000 /
                     COAP_TICKS_PER_SECOND;
        tv.tv_sec = (obs_wait - now) / COAP_TICKS_PER_SECOND;
      }
      else
      {
        tv.tv_usec = ((max_wait - now) % COAP_TICKS_PER_SECOND) * 1000000 /
                     COAP_TICKS_PER_SECOND;
        tv.tv_sec = (max_wait - now) / COAP_TICKS_PER_SECOND;
      }
    }

    result = select(ctx->sockfd + 1, &readfds, 0, 0, &tv);

    if (result < 0)
    { /* error */
      perror("select");
    }
    else if (result > 0)
    { /* read from socket */
      if (FD_ISSET(ctx->sockfd, &readfds))
      {
        coap_read(ctx); /* read received data */
        /* coap_dispatch( ctx );  /\* and dispatch PDUs from receivequeue */
      }
    }
    else
    { /* timeout */
      coap_ticks(&now);
      if (max_wait <= now)
      {
        info("timeout\n");
        continue;
      }
      if (obs_wait && obs_wait <= now)
      {
        debug("clear observation relationship\n");
        // clear_obs(ctx, ctx->endpoint, &dst); /* FIXME: handle error case
        // COAP_TID_INVALID */

        /* make sure that the obs timer does not fire again */
        obs_wait = 0;
        obs_seconds = 0;
      }
    }
  }

  coap_free_context(ctx);
  vTaskDelete(NULL);
}
/**
 * Main function,
 * Creates entities and containers
 * publishes/subscribes
 * */
static void om2m_coap_client_task(void *pvParameters)
{
  create_entity(AE_NAME);                         // Create ESP8266
  create_container(AE_NAME, CONTAINER_NAME);      // Create ESP8266/HR
  create_container(AE_NAME, ACTUATION);           // Create ESP8266/Actuation
  create_container(AE_NAME, PING);                // Create ESP8266/DELAY
  create_entity(CNTRL_SUB);                       // Create Sensor
  create_sub(AE_NAME, ACTUATION, CNTRL_SUB, SUB); // Subscribe to ESP8266/Actuation with Sensor entity

  xTaskCreate(ping, "ping_pong", 10000, NULL, 5, NULL);

  char name[50];
  char data[20];
  unsigned short int i = 0;

  while (1)
  {
    xEventGroupWaitBits(coap_group, BUFFER_BIT, false, true, portMAX_DELAY);
    xEventGroupClearBits(coap_group, BUFFER_BIT);

    //Send Heart Beat
    sprintf(data, "%lf:%lf", avg, diff_avg);
    sprintf(name, "HB_%d", i);

#if defined(E2E)
    // Sent to Monitor for e2e estimation
    printf("Publish;rn:%s;timestamp:%lld\n", name, get_timestamp());
#endif

    om2m_coap_create_content_instance(ctx, dst_addr, AE_NAME, CONTAINER_NAME,
                                      name, data, &i, COAP_REQUEST_POST);
    //TODO: Send SpO2_%d id
    vTaskDelay(1000 / portTICK_RATE_MS);
  }
  while (1)
    vTaskDelay(1000 / portTICK_RATE_MS);

  vTaskDelete(NULL);
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
  switch (event->event_id)
  {
  case SYSTEM_EVENT_STA_START:
    esp_wifi_connect();
    break;
  case SYSTEM_EVENT_STA_GOT_IP:
    xEventGroupSetBits(coap_group, CONNECTED_BIT);
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    /* This is a workaround as ESP32 WiFi libs don't currently
             auto-reassociate. */
    esp_wifi_connect();
    xEventGroupClearBits(coap_group, CONNECTED_BIT);
    break;
  default:
    break;
  }
  return ESP_OK;
}

static void wifi_conn_init(void)
{
  tcpip_adapter_init();
  ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASS,
              .listen_interval = 100,
          },
  };
  // WIFI_PS_MAX_MODEM
  // WIFI_PS_NONE
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#if !defined(E2E)
  ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
#endif
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
}
/**
 * Create new subscription to entity/container,
 * with entity sub_entity,
 * registered with sub_name name
 * 
 * @param entity
 * @param container
 * @param sub_entity
 * @sub_name
 * */
static void create_sub(char *entity, char *container, char *sub_entity, char *sub_name)
{
  xEventGroupClearBits(coap_group, SUB_BIT);

  while (!(xEventGroupGetBits(coap_group) & SUB_BIT))
  {
#if !defined(E2E)
    ESP_LOGI(TAG, "Subscribing to: %s/%s with %s", entity, container, sub_entity);
#endif
    om2m_coap_create_subscription(ctx, dst_addr, entity, container, sub_entity, sub_name);
    vTaskDelay(RETRANSMISSION / portTICK_RATE_MS);
  }
  printf("Subscribed to %s/%s with %s\tSub name: %s\n", entity, container, sub_entity, sub_name);
}


/**
 * Create new entity
 * 
 * @param entity - Name of entity to create
 * */ 
static void create_entity(char *entity)
{
  xEventGroupClearBits(coap_group, AE_BIT);

  while (!(xEventGroupGetBits(coap_group) & AE_BIT))
  {
#if !defined(E2E)
    ESP_LOGI(TAG, "Creating AE %s", entity);
#endif
    om2m_coap_create_ae(ctx, dst_addr, entity, 8989);
    vTaskDelay(RETRANSMISSION / portTICK_RATE_MS);
  }
  xEventGroupWaitBits(coap_group, AE_BIT, false, true, portMAX_DELAY);
  printf("AE %s created\n", entity);
}

/**
 * Create new container in path entity/container
 * 
 * @param entity - entity name
 * @param container - container name
 * */
static void create_container(char *entity, char *container)
{
  xEventGroupClearBits(coap_group, CNT_BIT);

  while (!(xEventGroupGetBits(coap_group) & CNT_BIT))
  {
#if !defined(E2E)
    ESP_LOGI(TAG, "Creating publish container %s/%s", entity, container);
#endif
    om2m_coap_create_container(ctx, dst_addr, entity, container);
    vTaskDelay(RETRANSMISSION / portTICK_RATE_MS);
  }
  xEventGroupWaitBits(coap_group, CNT_BIT, false, true, portMAX_DELAY);
  printf("Container %s/%s created\n", entity, container);
}

static void init_coap(void)
{
  // wait for AP connection
  xEventGroupWaitBits(coap_group, CONNECTED_BIT, false, true,
                      portMAX_DELAY);
#if !defined(E2E)
  ESP_LOGI(TAG, "Connected to AP");
#endif

  coap_address_init(&src_addr);
  coap_address_init(&dst_addr);

  src_addr.addr.sin.sin_family = AF_INET;
  src_addr.addr.sin.sin_port = htons(CSE_PORT);
  src_addr.addr.sin.sin_addr.s_addr = INADDR_ANY;

  dst_addr.addr.sin.sin_family = AF_INET;
  dst_addr.addr.sin.sin_port = htons(CSE_PORT);
  dst_addr.addr.sin.sin_addr.s_addr = inet_addr(CSE_IP);

  // create new context
  while (!ctx)
  {
#if !defined(E2E)
    ESP_LOGI(TAG, "Creating new context");
#endif
    ctx = coap_new_context(&src_addr);

    vTaskDelay(2000 / portTICK_RATE_MS);
  }
  coap_register_response_handler(ctx, message_handler);
  coap_register_request_handler(ctx, request_hanlder);

  // create new task for context handling
  xTaskCreate(coap_context_handler, "coap_context_handler", 8192, ctx, 5, NULL);
#if !defined(E2E)
  ESP_LOGI(TAG, "CoAP context created");
#endif
}
void app_main(void)
{
  printf("Starting ESP\n");
  ESP_ERROR_CHECK(nvs_flash_init());
  coap_group = xEventGroupCreate();

#if defined(SENSOR)
  max30100_init();
  xTaskCreate(max30100_updater, "updater", 10000, NULL, 5, NULL);
  //xTaskCreate(adjust_current, "ajdust current", 10000, NULL, 3, NULL);
#endif
#ifndef DEBUG_SENSOR
  wifi_conn_init();
  init_coap();
  xTaskCreate(om2m_coap_client_task, "coap", 16384, NULL, 5, NULL);
#endif
}