/* Pilot Light Monitor
 *
 * Copyright 2023 Vernon Mauery <vernon@mauery.org>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <stdlib.h>
#include <string.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

extern const char* TAG;
esp_err_t _http_event_handler(esp_http_client_event_t* evt)
{
    static char* output_buffer; // Buffer to store response of http request from
                                // event handler
    switch (evt->event_id)
    {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
                     evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL)
            {
                // Response is accumulated in output_buffer. Uncomment the below
                // line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error(
                (esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0)
            {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (output_buffer != NULL)
            {
                free(output_buffer);
                output_buffer = NULL;
            }
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            esp_http_client_set_header(evt->client, "From", "user@example.com");
            esp_http_client_set_header(evt->client, "Accept", "text/html");
            esp_http_client_set_redirection(evt->client);
            break;
    }
    return ESP_OK;
}

char* basic_auth(const char* user, const char* passwd);

void https_post(const char* uri, const char* data, const char* content_type,
                const char* user, const char* passwd)
{
    esp_http_client_config_t config = {
        .url = uri,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .is_async = true,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, data, strlen(data));
    esp_http_client_set_header(client, "Content-type", content_type);
    if (user || passwd)
    {
        char* auth = basic_auth(user, passwd);
        esp_http_client_set_header(client, "Authorization", auth);
        free(auth);
    }
    while (1)
    {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN)
        {
            break;
        }
    }
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %lld",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

void https_get(const char* host, const char* path, const char* query)
{
    printf("get: https://%s%s%s%s\n", host, path, (query ? "?" : ""),
           (query ? query : ""));
    esp_http_client_config_t config = {
        .host = host,
        .path = path,
        .query = query,
        .method = HTTP_METHOD_GET,
        .common_name = host,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %lld",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

char* urlencode(const char* msg)
{
    size_t count = 0;
    const char* tmsg = msg;
    if (!msg)
    {
        return NULL;
    }
    while (*tmsg)
    {
        switch (*tmsg)
        {
            case 'a' ... 'z':
            case 'A' ... 'Z':
            case '0' ... '9':
            case '-':
            case '_':
            case '.':
            case ' ':
                count++;
                break;
            default:
                // turn x into %HH, 2 chars longer
                count += 3;
                break;
        }
        tmsg++;
    }
    char* smsg = malloc(count + 1);
    if (!smsg)
    {
        return NULL;
    }
    static const char* atoh = "0123456789abcdef";
    char* tsmsg = smsg;
    while (*msg)
    {
        switch (*msg)
        {
            case 'a' ... 'z':
            case 'A' ... 'Z':
            case '0' ... '9':
            case '-':
            case '_':
            case '.':
                *tsmsg++ = *msg;
                break;
            case ' ':
                *tsmsg++ = '+';
                break;
            default:
                // turn x into %HH, 2 chars longer
                *tsmsg++ = '%';
                *tsmsg++ = atoh[((*msg) >> 4) & 0x0f];
                *tsmsg++ = atoh[((*msg) >> 0) & 0x0f];
                break;
        }
        msg++;
    }
    *tsmsg = 0;
    // must be free'd
    return smsg;
}
