/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_hidd_api.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_gap_bt_api.h"
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define DEBOUNCE_TIME 50 // Debounce time in milliseconds

#define YOUR_GAMEPAD_REPORT_ID 0x01
#define REPORT_PROTOCOL_GAMEPAD_REPORT_SIZE (6)
#define REPORT_BUFFER_SIZE REPORT_PROTOCOL_GAMEPAD_REPORT_SIZE

typedef struct
{
    gpio_num_t gpio_pin;           // GPIO pin number
    bool last_state;               // Last state of the button
    bool pressed;                  // Current press state
    unsigned long last_press_time; // Timestamp for debouncing
} button_t;

button_t buttons[] = {
    {13, true, false, 0}, // Button 0 (GPIO 13)
    {33, true, false, 0}  // Button 1 (GPIO 25)
};

#define NUM_BUTTONS (sizeof(buttons) / sizeof(buttons[0])) // Number of buttons

// Function to initialize buttons
void init_buttons(void)
{
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE, // Disable interrupt (polling)
            .pin_bit_mask = (1ULL << buttons[i].gpio_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, // Use internal pull-up resistor
            .pull_down_en = GPIO_PULLDOWN_DISABLE};
        gpio_config(&io_conf);
    }
}

// // Function to handle button press and debounce
// void handle_button_press(button_t *btn)
// {
//     bool current_state = gpio_get_level(btn->gpio_pin);       // Read current button state
//     unsigned long current_time = esp_timer_get_time() / 1000; // Convert to ms

//     if (current_state == 0 && btn->last_state == 1)
//     { // Detect press (active low)
//         if (current_time - btn->last_press_time > DEBOUNCE_TIME)
//         { // Debounce check
//             btn->pressed = true;
//             btn->last_press_time = current_time; // Update last press time
//         }
//     }
//     btn->last_state = current_state; // Update last button state
// }

void handle_button_press(button_t *btn)
{
    bool current_state = gpio_get_level(btn->gpio_pin); // Read current button state
    if (current_state == 0)
    {
        btn->pressed = true;
    }
}

// Function to process button presses
void process_button_press(int button_index)
{
    button_t *btn = &buttons[button_index];
    if (btn->pressed)
    {
        ESP_LOGI("button_input_task", "Button %d Pressed!", button_index);
        btn->pressed = false; // Reset the flag
    }
}

typedef struct
{
    esp_hidd_app_param_t app_param;
    esp_hidd_qos_param_t both_qos;
    uint8_t protocol_mode;
    SemaphoreHandle_t gamepad_mutex;
    TaskHandle_t gamepad_task_hdl;
    uint8_t buffer[REPORT_BUFFER_SIZE];
} local_param_t;

static local_param_t s_local_param = {0};

// HID Descriptor for a Gamepad with 15 Buttons + 2 Joysticks
const uint8_t hid_gamepad_descriptor[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x05, // Usage (Gamepad)
    0xA1, 0x01, // Collection (Application)

    // Report ID
    0x85, 0x01, // Report ID 1

    // Joysticks (X, Y, Rx, Ry) - 8-bit each (-127 to 127)
    0x09, 0x30, 0x09, 0x31, // Usage (X, Y)
    0x09, 0x33, 0x09, 0x34, // Usage (Rx, Ry)
    0x15, 0x81,             // Logical Minimum (-127)
    0x25, 0x7F,             // Logical Maximum (127)
    0x75, 0x08,             // Report Size (8 bits)
    0x95, 0x04,             // Report Count (4)
    0x81, 0x02,             // Input (Data, Variable, Absolute)

    // Buttons (15 buttons)
    0x05, 0x09, // Usage Page (Button)
    0x19, 0x01, // Usage Minimum (Button 1)
    0x29, 0x0F, // Usage Maximum (Button 15)
    0x15, 0x00, // Logical Minimum (0)
    0x25, 0x01, // Logical Maximum (1)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x0F, // Report Count (15 buttons)
    0x81, 0x02, // Input (Data, Variable, Absolute)

    // Padding to make a full byte (16 bits)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x03, // Input (Constant, Variable, Absolute)

    // End Collection
    0xC0};

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18)
    {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

const int hid_gamepad_descriptor_len = sizeof(hid_gamepad_descriptor);

bool check_report_id_type(uint8_t report_id, uint8_t report_type)
{
    bool ret = false;
    xSemaphoreTake(s_local_param.gamepad_mutex, portMAX_DELAY);

    do
    {
        // Ensure the report type is INPUT (gamepad sends input data)
        if (report_type != ESP_HIDD_REPORT_TYPE_INPUT)
        {
            break;
        }

        // If Boot Mode is active (not relevant for a gamepad, but included for completeness)
        if (s_local_param.protocol_mode == ESP_HIDD_BOOT_MODE)
        {
            // Gamepads do not use Boot Mode, so reject the request
            break;
        }
        else
        {
            // Your custom gamepad report ID check
            if (report_id == YOUR_GAMEPAD_REPORT_ID) // Define this in your descriptor
            {
                ret = true;
                break;
            }
        }
    } while (0);

    // Send an error if the report ID is invalid
    if (!ret)
    {
        esp_bt_hid_device_report_error(ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_REP_ID);
    }

    xSemaphoreGive(s_local_param.gamepad_mutex);
    return ret;
}

// Send the buttons and joystick data
void send_gamepad_report(char joystick1_x, char joystick1_y, char joystick2_x, char joystick2_y, uint16_t buttons)
{
    uint8_t report_id;
    uint16_t report_size;
    xSemaphoreTake(s_local_param.gamepad_mutex, portMAX_DELAY); // Protect critical section

    if (s_local_param.protocol_mode == ESP_HIDD_REPORT_MODE)
    {
        // In Report Mode (more flexible)
        report_id = 0x01; // Default report ID
        report_size = REPORT_PROTOCOL_GAMEPAD_REPORT_SIZE;

        s_local_param.buffer[0] = joystick1_x;           // Joystick 1 X axis
        s_local_param.buffer[1] = joystick1_y;           // Joystick 1 Y axis
        s_local_param.buffer[2] = joystick2_x;           // Joystick 2 X axis
        s_local_param.buffer[3] = joystick2_y;           // Joystick 2 Y axis
        s_local_param.buffer[4] = buttons & 0xFF;        // Low byte of button states
        s_local_param.buffer[5] = (buttons >> 8) & 0x7F; // High byte (only 7 bits used)
    }
    // Send the report
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, report_id, report_size, s_local_param.buffer);
    xSemaphoreGive(s_local_param.gamepad_mutex); // Release mutex
    // ESP_LOGI("Gamepad", "Sending report: buttons=%x, joystick1_x=%d, joystick1_y=%d, joystick2_x=%d, joystick2_y=%d", buttons, joystick1_x, joystick1_y, joystick2_x, joystick2_y);
}

// Test the gamepad by pressing the first button repeatedly
void gamepad_test_task(void *pvParameters)
{
    const char *TAG = "gamepad_test_task";

    ESP_LOGI(TAG, "starting");

    // Variables for button states and joystick positions
    uint16_t buttons_state = 0;
    init_buttons(); // Initialize buttons

    for (;;)
    {
        for (int i = 0; i < NUM_BUTTONS; i++)
        {
            handle_button_press(&buttons[i]); // Check button press and debounce
            if (buttons[i].pressed)
            {
                buttons_state |= (1 << i); // Use bitwise OR to set the bit
            }
            else
            {
                // Clear the corresponding bit in the buttons_state (button is not pressed)
                buttons_state &= ~(1 << i); // Use bitwise AND with negation to clear the bit
            }
            process_button_press(i); // Process the button press
        }
        send_gamepad_report(0, 0, 0, 0, buttons_state);

        vTaskDelay(pdMS_TO_TICKS(20)); // Poll every 10 ms

        // buttons_state = 0x7FFF;                             // First button pressed
        // send_gamepad_report(127, 0, 127, 0, buttons_state); // Send report with only first button pressed
        // vTaskDelay(pdMS_TO_TICKS(20));        // Delay for 500ms

        // // Release the first button
        // buttons_state = 0x00;                               // No button pressed
        // send_gamepad_report(0, 127, 0, 127, buttons_state); // Send report with no buttons pressed
        // vTaskDelay(pdMS_TO_TICKS(20));        // Delay for 500ms
    }
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    const char *TAG = "esp_bt_gap_cb";
    switch (event)
    {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
    {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(TAG, "authentication success: %s", param->auth_cmpl.device_name);
            esp_log_buffer_hex(TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        }
        else
        {
            ESP_LOGE(TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT:
    {
        ESP_LOGI(TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit)
        {
            ESP_LOGI(TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        }
        else
        {
            ESP_LOGI(TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %" PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        break;
    default:
        ESP_LOGI(TAG, "event: %d", event);
        break;
    }
    return;
}

void bt_app_task_start_up(void)
{
    s_local_param.gamepad_mutex = xSemaphoreCreateMutex();
    memset(s_local_param.buffer, 0, REPORT_BUFFER_SIZE);
    xTaskCreate(gamepad_test_task, "gamepad_test_task", 2 * 1024, NULL, configMAX_PRIORITIES - 3, &s_local_param.gamepad_task_hdl);
    return;
}

void bt_app_task_shut_down(void)
{
    if (s_local_param.gamepad_task_hdl)
    {
        vTaskDelete(s_local_param.gamepad_task_hdl);
        s_local_param.gamepad_task_hdl = NULL;
    }

    if (s_local_param.gamepad_mutex)
    {
        vSemaphoreDelete(s_local_param.gamepad_mutex);
        s_local_param.gamepad_mutex = NULL;
    }
    return;
}

void esp_bt_hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    static const char *TAG = "esp_bt_hidd_cb";
    switch (event)
    {
    case ESP_HIDD_INIT_EVT:
        if (param->init.status == ESP_HIDD_SUCCESS)
        {
            ESP_LOGI(TAG, "setting hid parameters");
            esp_bt_hid_device_register_app(&s_local_param.app_param, &s_local_param.both_qos, &s_local_param.both_qos);
        }
        else
        {
            ESP_LOGE(TAG, "init hidd failed!");
        }
        break;
    case ESP_HIDD_DEINIT_EVT:
        break;
    case ESP_HIDD_REGISTER_APP_EVT:
        if (param->register_app.status == ESP_HIDD_SUCCESS)
        {
            ESP_LOGI(TAG, "setting hid parameters success!");
            ESP_LOGI(TAG, "setting to connectable, discoverable");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            if (param->register_app.in_use)
            {
                ESP_LOGI(TAG, "start virtual cable plug!");
                esp_bt_hid_device_connect(param->register_app.bd_addr);
            }
        }
        else
        {
            ESP_LOGE(TAG, "setting hid parameters failed!");
        }
        break;
    case ESP_HIDD_UNREGISTER_APP_EVT:
        if (param->unregister_app.status == ESP_HIDD_SUCCESS)
        {
            ESP_LOGI(TAG, "unregister app success!");
        }
        else
        {
            ESP_LOGE(TAG, "unregister app failed!");
        }
        break;
    case ESP_HIDD_OPEN_EVT:
        if (param->open.status == ESP_HIDD_SUCCESS)
        {
            if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTING)
            {
                ESP_LOGI(TAG, "connecting...");
            }
            else if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED)
            {
                ESP_LOGI(TAG, "connected to %02x:%02x:%02x:%02x:%02x:%02x", param->open.bd_addr[0],
                         param->open.bd_addr[1], param->open.bd_addr[2], param->open.bd_addr[3], param->open.bd_addr[4],
                         param->open.bd_addr[5]);
                bt_app_task_start_up();
                ESP_LOGI(TAG, "making self non-discoverable and non-connectable.");
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            }
            else
            {
                ESP_LOGE(TAG, "unknown connection status");
            }
        }
        else
        {
            ESP_LOGE(TAG, "open failed!");
        }
        break;
    case ESP_HIDD_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_HIDD_CLOSE_EVT");
        if (param->close.status == ESP_HIDD_SUCCESS)
        {
            if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTING)
            {
                ESP_LOGI(TAG, "disconnecting...");
            }
            else if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED)
            {
                ESP_LOGI(TAG, "disconnected!");
                bt_app_task_shut_down();
                ESP_LOGI(TAG, "making self discoverable and connectable again.");
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            }
            else
            {
                ESP_LOGE(TAG, "unknown connection status");
            }
        }
        else
        {
            ESP_LOGE(TAG, "close failed!");
        }
        break;
    case ESP_HIDD_SEND_REPORT_EVT:
        if (param->send_report.status == ESP_HIDD_SUCCESS)
        {
            ESP_LOGI(TAG, "ESP_HIDD_SEND_REPORT_EVT id:0x%02x, type:%d", param->send_report.report_id,
                     param->send_report.report_type);
        }
        else
        {
            ESP_LOGE(TAG, "ESP_HIDD_SEND_REPORT_EVT id:0x%02x, type:%d, status:%d, reason:%d",
                     param->send_report.report_id, param->send_report.report_type, param->send_report.status,
                     param->send_report.reason);
        }
        break;
    case ESP_HIDD_REPORT_ERR_EVT:
        ESP_LOGI(TAG, "ESP_HIDD_REPORT_ERR_EVT");
        break;
    case ESP_HIDD_GET_REPORT_EVT:
        ESP_LOGI(TAG, "ESP_HIDD_GET_REPORT_EVT id:0x%02x, type:%d, size:%d", param->get_report.report_id,
                 param->get_report.report_type, param->get_report.buffer_size);
        if (check_report_id_type(param->get_report.report_id, param->get_report.report_type))
        {
            uint8_t report_id;
            uint16_t report_len;
            if (s_local_param.protocol_mode == ESP_HIDD_REPORT_MODE)
            {
                report_id = 1;
                report_len = REPORT_PROTOCOL_GAMEPAD_REPORT_SIZE;
            }
            xSemaphoreTake(s_local_param.gamepad_mutex, portMAX_DELAY);
            esp_bt_hid_device_send_report(param->get_report.report_type, report_id, report_len, s_local_param.buffer);
            xSemaphoreGive(s_local_param.gamepad_mutex);
        }
        else
        {
            ESP_LOGE(TAG, "check_report_id failed!");
        }
        break;
    case ESP_HIDD_SET_REPORT_EVT:
        ESP_LOGI(TAG, "ESP_HIDD_SET_REPORT_EVT");
        break;
    case ESP_HIDD_SET_PROTOCOL_EVT:
        ESP_LOGI(TAG, "ESP_HIDD_SET_PROTOCOL_EVT");
        if (param->set_protocol.protocol_mode == ESP_HIDD_REPORT_MODE)
        {
            ESP_LOGI(TAG, "  - report protocol");
        }
        xSemaphoreTake(s_local_param.gamepad_mutex, portMAX_DELAY);
        s_local_param.protocol_mode = param->set_protocol.protocol_mode;
        xSemaphoreGive(s_local_param.gamepad_mutex);
        break;
    case ESP_HIDD_INTR_DATA_EVT:
        ESP_LOGI(TAG, "ESP_HIDD_INTR_DATA_EVT");
        break;
    case ESP_HIDD_VC_UNPLUG_EVT:
        ESP_LOGI(TAG, "ESP_HIDD_VC_UNPLUG_EVT");
        if (param->vc_unplug.status == ESP_HIDD_SUCCESS)
        {
            if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED)
            {
                ESP_LOGI(TAG, "disconnected!");
                bt_app_task_shut_down();
                ESP_LOGI(TAG, "making self discoverable and connectable again.");
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            }
            else
            {
                ESP_LOGE(TAG, "unknown connection status");
            }
        }
        else
        {
            ESP_LOGE(TAG, "close failed!");
        }
        break;
    default:
        break;
    }
}

void app_main(void)
{
    const char *TAG = "app_main";
    esp_err_t ret;
    char bda_str[18] = {0};

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK)
    {
        ESP_LOGE(TAG, "initialize controller failed: %s", esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK)
    {
        ESP_LOGE(TAG, "enable controller failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_EXAMPLE_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK)
    {
        ESP_LOGE(TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK)
    {
        ESP_LOGE(TAG, "enable bluedroid failed: %s", esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_gap_register_callback(esp_bt_gap_cb)) != ESP_OK)
    {
        ESP_LOGE(TAG, "gap register failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "setting device name");
    esp_bt_gap_set_device_name("HID Gamepad Example");

    ESP_LOGI(TAG, "setting cod major, peripheral");
    esp_bt_cod_t cod;
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);

    vTaskDelay(2000 / portTICK_PERIOD_MS);

    // Initialize HID SDP information and L2CAP parameters.
    // to be used in the call of `esp_bt_hid_device_register_app` after profile initialization finishes
    do
    {
        s_local_param.app_param.name = "Gamepad";
        s_local_param.app_param.description = "Gamepad Example";
        s_local_param.app_param.provider = "ESP32";
        s_local_param.app_param.subclass = ESP_HID_CLASS_GPD;
        s_local_param.app_param.desc_list = hid_gamepad_descriptor;
        s_local_param.app_param.desc_list_len = hid_gamepad_descriptor_len;

        memset(&s_local_param.both_qos, 0, sizeof(esp_hidd_qos_param_t)); // don't set the qos parameters
    } while (0);

    // Report Protocol Mode is the default mode, according to Bluetooth HID specification
    s_local_param.protocol_mode = ESP_HIDD_REPORT_MODE;

    ESP_LOGI(TAG, "register hid device callback");
    esp_bt_hid_device_register_callback(esp_bt_hidd_cb);

    ESP_LOGI(TAG, "starting hid device");
    esp_bt_hid_device_init();

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    ESP_LOGI(TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    ESP_LOGI(TAG, "exiting");
}
