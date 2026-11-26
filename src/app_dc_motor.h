/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_DC_MOTOR_H
#define APP_DC_MOTOR_H

#include <zephyr/types.h>
#include <zephyr/bluetooth/conn.h>

/* Protocol Constants */
#define DC_MOTOR_OPCODE                 0x8E
#define DC_MOTOR_SERVICE_DATA           0xFFE2

/* UI Update Response fixed motor parameters */
#define DC_MOTOR_VOLTAGE                24U
#define DC_MOTOR_MIN_RPM                30U
#define DC_MOTOR_MAX_RPM                1360U
#define QEI_COUNTS_PER_OUTPUT_REV       194U
#define DC_MOTOR_CLOSED_LOOP_ENABLED    1U

/* Control Request IDs (App -> Device) */
#define DC_CMD_UI_UPDATE_REQ            0xA1
#define DC_CMD_SLIDER_SET               0xA2
#define DC_CMD_FIXED_DIRECTION_SET      0xA4
#define DC_CMD_START_STOP_SET           0xA5

/* Data Response IDs (Device -> App) */
#define DC_RSP_UI_UPDATE                0xB1
#define DC_RSP_SLIDER_SET               0xB2
#define DC_RSP_FIXED_DIRECTION_SET      0xB4
#define DC_RSP_START_STOP_SET           0xB5

/* Notification IDs (Device -> App) */
#define DC_NTF_SLIDER                   0xC2
#define DC_NTF_FIXED_DIRECTION          0xC4
#define DC_NTF_START_STOP               0xC5

/* Acknowledgment Response IDs */
#define DC_ACK_UI_UPDATE                0xD1
#define DC_ACK_SLIDER_SET               0xD2
#define DC_ACK_FIXED_DIRECTION_SET      0xD4
#define DC_ACK_START_STOP_SET           0xD5

/* Direction Values */
#define DC_DIR_CW                       0x00
#define DC_DIR_CCW                      0x01

/* Start/Stop Values */
#define DC_STOP                         0x00
#define DC_START                        0x01

int app_dc_motor_ble_init(void);
int app_dc_motor_format_status(char *buffer, size_t capacity);

int app_dc_motor_ble_handle_cmd(struct bt_conn *conn, uint8_t opcode,
                                const uint8_t *payload, uint16_t payload_len);

int app_dc_motor_ble_send_ui_update_response(struct bt_conn *conn);

int app_dc_motor_ble_send_slider_response(struct bt_conn *conn, uint16_t slider_value);

int app_dc_motor_ble_send_fixed_direction_response(struct bt_conn *conn, uint8_t direction);

int app_dc_motor_ble_send_start_stop_response(struct bt_conn *conn, uint8_t start_stop);

int app_dc_motor_ble_send_slider_notify(struct bt_conn *conn, uint16_t slider_value);

int app_dc_motor_ble_send_fixed_direction_notify(struct bt_conn *conn, uint8_t direction);

int app_dc_motor_ble_send_start_stop_notify(struct bt_conn *conn, uint8_t start_stop);

int app_dc_motor_ble_send_ack(struct bt_conn *conn, uint8_t ack_id);

#endif /* APP_DC_MOTOR_H */
