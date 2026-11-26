/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>

#include "app_dc_motor.h"
#include "plib_qei.h"
#include "trs_server.h"

LOG_MODULE_REGISTER(app_dc_motor);

static uint32_t qei_counts_per_output_rev = QEI_COUNTS_PER_OUTPUT_REV;
#if DT_NODE_HAS_STATUS(DT_ALIAS(dc_motor_a_high_pwm), okay) && \
    DT_NODE_HAS_STATUS(DT_ALIAS(dc_motor_b_high_pwm), okay) && \
    DT_NODE_HAS_STATUS(DT_ALIAS(dc_motor_a_low), okay) && \
    DT_NODE_HAS_STATUS(DT_ALIAS(dc_motor_b_low), okay)
static const struct pwm_dt_spec motor_a_high_pwm = PWM_DT_SPEC_GET(DT_ALIAS(dc_motor_a_high_pwm));
static const struct pwm_dt_spec motor_b_high_pwm = PWM_DT_SPEC_GET(DT_ALIAS(dc_motor_b_high_pwm));
static const struct gpio_dt_spec motor_a_low = GPIO_DT_SPEC_GET(DT_ALIAS(dc_motor_a_low), gpios);
static const struct gpio_dt_spec motor_b_low = GPIO_DT_SPEC_GET(DT_ALIAS(dc_motor_b_low), gpios);
#define DC_MOTOR_OUTPUTS_AVAILABLE 1
#endif

#define DC_MOTOR_PWM_PERIOD_NS 1000000U
#define DC_MOTOR_START_DUTY_PERCENT 3U
#define DC_MOTOR_MAX_DUTY_PERCENT 95U
#define DC_MOTOR_CONTROL_PERIOD_MS 100U
#define DC_MOTOR_DUTY_SLEW_PERCENT 1U
#define DC_MOTOR_PI_KP_DIVISOR 50
#define DC_MOTOR_PI_KI_DIVISOR 500
#define DC_MOTOR_PI_INTEGRAL_LIMIT 475000

static struct k_work_delayable qei_log_work;
static struct k_work_delayable dc_motor_control_work;
static struct k_timer qei_gpio_sample_timer;
static uint32_t qei_last_position;
static int64_t qei_last_log_ms;
static int32_t qei_last_control_position;
static uint32_t qei_last_gpio_state;
static volatile uint32_t qei_gpio_transitions;
static int32_t dc_motor_integral_error;
static uint32_t motor_status_rpm_x10;
static int32_t dc_motor_filtered_rpm_x10;
static int64_t dc_motor_last_control_ms;

static void qei_gpio_sample_timer_handler(struct k_timer *timer)
{
    uint32_t state = QEI_ENCODER_GPIO_REGS->GPIO_PORT & QEI_ENCODER_PIN_MASK;

    if (state != qei_last_gpio_state) {
        qei_gpio_transitions++;
        qei_last_gpio_state = state;
    }
}

static void log_vendor_cmd(const char *label, const uint8_t opcode, const uint8_t *payload, uint16_t len)
{
    char hex_str[256];
    int pos = 0;

    pos += snprintf(&hex_str[pos], sizeof(hex_str) - pos, "%s - Opcode: %02X, Len: %d, Data: ", label, opcode, len);

    for (uint16_t i = 0; i < len && pos < (int)sizeof(hex_str) - 4; i++) {
        pos += snprintf(&hex_str[pos], sizeof(hex_str) - pos, "%02X ", payload[i]);
    }

    LOG_INF("%s", hex_str);
}

static struct {
    char name[32];
    uint8_t name_len;
    char feedback[16];
    uint8_t feedback_len;
    uint16_t slider_value;
    uint8_t fixed_direction;
    uint8_t start_stop;
    uint32_t current_duty_ns;
    uint8_t active_direction;
    bool outputs_active;
} motor_state;

static void qei_log_work_handler(struct k_work *work)
{
    int32_t position = (int32_t)QEI_PositionGet();
#if defined(CONFIG_APP_DC_MOTOR_VERBOSE_QEI_LOG)
    int32_t raw_velocity = (int32_t)QEI_VelocityGet();
    uint32_t qei_ioc = QEI_REGS->QEI_QEIIOC;
    uint32_t encoder_port = QEI_ENCODER_GPIO_REGS->GPIO_PORT;
    uint32_t gpio_transitions = qei_gpio_transitions;
#endif
    int32_t delta = (int32_t)((uint32_t)position - qei_last_position);
    int64_t now_ms = k_uptime_get();
    int64_t elapsed_ms = now_ms - qei_last_log_ms;
    if (elapsed_ms <= 0) {
        elapsed_ms = 1;
    }
    int64_t rpm_x10 = ((int64_t)delta * 600000) /
                      ((int64_t)qei_counts_per_output_rev * elapsed_ms);
    uint32_t rpm_magnitude = (uint32_t)(rpm_x10 < 0 ? -rpm_x10 : rpm_x10);
    int32_t velocity = (int32_t)(((int64_t)delta * 1000) / elapsed_ms);
    motor_status_rpm_x10 = rpm_magnitude;

    qei_last_position = (uint32_t)position;
    qei_last_log_ms = now_ms;
    qei_gpio_transitions = 0U;

    LOG_INF("calculated_rpm=%s%u.%u rpm target_rpm=%u rpm duty=%u%% velocity=%d counts/s",
            rpm_x10 < 0 ? "-" : "", rpm_magnitude / 10U, rpm_magnitude % 10U,
            motor_state.slider_value,
            (motor_state.current_duty_ns * 100U) / DC_MOTOR_PWM_PERIOD_NS,
            velocity);
#if defined(CONFIG_APP_DC_MOTOR_VERBOSE_QEI_LOG)
    LOG_INF("QEI position=%d counts delta=%d counts sample=%u ms calculated_rpm=%s%u.%u rpm target_rpm=%u rpm duty=%u%% raw_velocity=%d counts QEA=%u QEB=%u "
            QEI_ENCODER_A_NAME "=%u " QEI_ENCODER_B_NAME "=%u sampled_changes=%u "
            QEI_ENCODER_PORT_NAME "=0x%04x",
            position, delta, (uint32_t)elapsed_ms,
            rpm_x10 < 0 ? "-" : "", rpm_magnitude / 10U, rpm_magnitude % 10U,
            motor_state.slider_value,
            (motor_state.current_duty_ns * 100U) / DC_MOTOR_PWM_PERIOD_NS,
            raw_velocity,
            (qei_ioc & QEI_QEIIOC_QEA_Msk) ? 1U : 0U,
            (qei_ioc & QEI_QEIIOC_QEB_Msk) ? 1U : 0U,
            (encoder_port & (1UL << QEI_ENCODER_A_PIN)) ? 1U : 0U,
            (encoder_port & (1UL << QEI_ENCODER_B_PIN)) ? 1U : 0U,
            gpio_transitions,
            encoder_port & 0xffffU);
#endif
    k_work_schedule(&qei_log_work, K_SECONDS(1));
}

int app_dc_motor_format_status(char *buffer, size_t capacity)
{
    int length = snprintf(buffer, capacity, "Calculated RPM : %u.%u | PWM :%u%%\r\n",
                          motor_status_rpm_x10 / 10U, motor_status_rpm_x10 % 10U,
                          (motor_state.current_duty_ns * 100U) / DC_MOTOR_PWM_PERIOD_NS);

    return length < 0 || (size_t)length >= capacity ? -ENOSPC : length;
}

static int dc_motor_stop_outputs(void)
{
#if !DC_MOTOR_OUTPUTS_AVAILABLE
    return -ENODEV;
#else
    int ret = pwm_set_dt(&motor_a_high_pwm, DC_MOTOR_PWM_PERIOD_NS, 0);

    if (ret != 0) {
        return ret;
    }

    ret = pwm_set_dt(&motor_b_high_pwm, DC_MOTOR_PWM_PERIOD_NS, 0);
    if (ret != 0) {
        return ret;
    }

    ret = gpio_pin_set_dt(&motor_a_low, 0);
    if (ret != 0) {
        return ret;
    }

    ret = gpio_pin_set_dt(&motor_b_low, 0);
    if (ret == 0) {
        motor_state.current_duty_ns = 0;
        motor_state.outputs_active = false;
    }

    return ret;
#endif
}

static int dc_motor_apply_from_slider(uint16_t slider_value)
{
#if !DC_MOTOR_OUTPUTS_AVAILABLE
    return -ENODEV;
#else
    int ret;

    if (slider_value == 0 || motor_state.start_stop == DC_STOP) {
        ret = dc_motor_stop_outputs();
        if (ret != 0) {
            return ret;
        }

        LOG_INF("DC motor stopped");
        return 0;
    }

    uint32_t duty_percent = DC_MOTOR_START_DUTY_PERCENT;
    if (slider_value > 1U) {
        duty_percent += ((uint32_t)(slider_value - 1U) *
                         (DC_MOTOR_MAX_DUTY_PERCENT - DC_MOTOR_START_DUTY_PERCENT)) /
                        (DC_MOTOR_MAX_RPM - 1U);
    }
    uint32_t duty_ns = (duty_percent * DC_MOTOR_PWM_PERIOD_NS) / 100U;

    if (motor_state.outputs_active && motor_state.active_direction != motor_state.fixed_direction) {
        ret = dc_motor_stop_outputs();
        if (ret != 0) {
            return ret;
        }
        k_busy_wait(100);
    }

    if (motor_state.fixed_direction == DC_DIR_CW) {
        ret = gpio_pin_set_dt(&motor_b_low, 0);
        if (ret != 0) {
            return ret;
        }
        ret = pwm_set_dt(&motor_a_high_pwm, DC_MOTOR_PWM_PERIOD_NS, 0);
        if (ret != 0) {
            return ret;
        }
        ret = gpio_pin_set_dt(&motor_a_low, 1);
        if (ret != 0) {
            return ret;
        }
        ret = pwm_set_dt(&motor_b_high_pwm, DC_MOTOR_PWM_PERIOD_NS, duty_ns);
    } else {
        ret = gpio_pin_set_dt(&motor_a_low, 0);
        if (ret != 0) {
            return ret;
        }
        ret = pwm_set_dt(&motor_b_high_pwm, DC_MOTOR_PWM_PERIOD_NS, 0);
        if (ret != 0) {
            return ret;
        }
        ret = gpio_pin_set_dt(&motor_b_low, 1);
        if (ret != 0) {
            return ret;
        }
        ret = pwm_set_dt(&motor_a_high_pwm, DC_MOTOR_PWM_PERIOD_NS, duty_ns);
    }

    if (ret == 0) {
        motor_state.current_duty_ns = duty_ns;
        motor_state.active_direction = motor_state.fixed_direction;
        motor_state.outputs_active = true;
        LOG_INF("DC motor PWM set: target=%u rpm duty=%u%% dir=%s, pulse=%u ns",
                slider_value, duty_percent,
                motor_state.fixed_direction == DC_DIR_CW ? "CW" : "CCW", duty_ns);
    }

    return ret;
#endif
}

static int dc_motor_set_active_duty(uint32_t duty_ns)
{
#if !DC_MOTOR_OUTPUTS_AVAILABLE
    return -ENODEV;
#else
    if (!motor_state.outputs_active) {
        return 0;
    }

    uint32_t max_step_ns = (DC_MOTOR_DUTY_SLEW_PERCENT *
                            DC_MOTOR_PWM_PERIOD_NS) / 100U;
    uint32_t current_duty_ns = motor_state.current_duty_ns;

    if (duty_ns > current_duty_ns + max_step_ns) {
        duty_ns = current_duty_ns + max_step_ns;
    } else if (current_duty_ns > duty_ns + max_step_ns) {
        duty_ns = current_duty_ns - max_step_ns;
    }

    int ret;
    if (motor_state.active_direction == DC_DIR_CW) {
        ret = pwm_set_dt(&motor_b_high_pwm, DC_MOTOR_PWM_PERIOD_NS, duty_ns);
    } else {
        ret = pwm_set_dt(&motor_a_high_pwm, DC_MOTOR_PWM_PERIOD_NS, duty_ns);
    }

    if (ret == 0) {
        motor_state.current_duty_ns = duty_ns;
    }

    return ret;
#endif
}

static void dc_motor_control_work_handler(struct k_work *work)
{
    int64_t now_ms = k_uptime_get();
    int64_t elapsed_ms = now_ms - dc_motor_last_control_ms;
    dc_motor_last_control_ms = now_ms;
    if (elapsed_ms <= 0) {
        elapsed_ms = DC_MOTOR_CONTROL_PERIOD_MS;
    }

    if (!DC_MOTOR_CLOSED_LOOP_ENABLED || !motor_state.outputs_active || motor_state.start_stop == DC_STOP ||
        motor_state.slider_value == 0U) {
        dc_motor_integral_error = 0;
        dc_motor_filtered_rpm_x10 = 0;
        qei_last_control_position = (int32_t)QEI_PositionGet();
        k_work_schedule(&dc_motor_control_work,
                        K_MSEC(DC_MOTOR_CONTROL_PERIOD_MS));
        return;
    }

    int32_t position = (int32_t)QEI_PositionGet();
    int32_t delta = (int32_t)((uint32_t)position - (uint32_t)qei_last_control_position);
    int32_t signed_rpm_x10 = (int32_t)(((int64_t)delta * 600000) /
                                   ((int64_t)qei_counts_per_output_rev * elapsed_ms));
    int32_t measured_rpm_x10 = motor_state.fixed_direction == DC_DIR_CW ?
                               signed_rpm_x10 : -signed_rpm_x10;
    dc_motor_filtered_rpm_x10 += (measured_rpm_x10 - dc_motor_filtered_rpm_x10) / 4;
    int32_t error_x10 = ((int32_t)motor_state.slider_value * 10) - dc_motor_filtered_rpm_x10;

    qei_last_control_position = position;
    int64_t integral_candidate = dc_motor_integral_error +
        ((int64_t)error_x10 * elapsed_ms) / DC_MOTOR_CONTROL_PERIOD_MS;
    int32_t next_integral = (int32_t)CLAMP(integral_candidate,
        -DC_MOTOR_PI_INTEGRAL_LIMIT, DC_MOTOR_PI_INTEGRAL_LIMIT);

    int32_t feedforward_percent = DC_MOTOR_START_DUTY_PERCENT;
    if (motor_state.slider_value > 1U) {
        feedforward_percent += ((int32_t)(motor_state.slider_value - 1U) *
                               (DC_MOTOR_MAX_DUTY_PERCENT - DC_MOTOR_START_DUTY_PERCENT)) /
                              (DC_MOTOR_MAX_RPM - 1U);
    }
    int32_t correction_x100 = (error_x10 * 10 / DC_MOTOR_PI_KP_DIVISOR) +
                             (next_integral * 10 / DC_MOTOR_PI_KI_DIVISOR);
    int32_t duty_x100 = feedforward_percent * 100 + correction_x100;
        const int32_t minimum_duty_x100 = (int32_t)DC_MOTOR_START_DUTY_PERCENT * 100;
        const int32_t maximum_duty_x100 = (int32_t)DC_MOTOR_MAX_DUTY_PERCENT * 100;

        if (!((duty_x100 >= maximum_duty_x100 && error_x10 > 0) ||
            (duty_x100 <= minimum_duty_x100 && error_x10 < 0))) {
        dc_motor_integral_error = next_integral;
    }
        duty_x100 = CLAMP(duty_x100, minimum_duty_x100, maximum_duty_x100);

    int ret = dc_motor_set_active_duty(
        (uint32_t)(((uint64_t)duty_x100 * DC_MOTOR_PWM_PERIOD_NS) / 10000U));
    if (ret != 0) {
        LOG_ERR("PI duty update failed: %d", ret);
    }

    k_work_schedule(&dc_motor_control_work,
                    K_MSEC(DC_MOTOR_CONTROL_PERIOD_MS));
}

int app_dc_motor_ble_init(void)
{
    memset(&motor_state, 0, sizeof(motor_state));

    const char *default_name = "BRUSHED DC";
    motor_state.name_len = strlen(default_name);
    memcpy(motor_state.name, default_name, motor_state.name_len);

    const char *feedback = "QEI";
    motor_state.feedback_len = strlen(feedback);
    memcpy(motor_state.feedback, feedback, motor_state.feedback_len);

    motor_state.slider_value = DC_MOTOR_MAX_RPM;
    motor_state.fixed_direction = DC_DIR_CCW;
    motor_state.active_direction = DC_DIR_CCW;
    motor_state.start_stop = DC_STOP;

    QEI_Initialize();
    QEI_PositionCountSet(0U);
    QEI_VelocityCountSet(0U);
    QEI_Start();
    qei_last_position = QEI_PositionGet();
    qei_last_log_ms = k_uptime_get();
    qei_last_gpio_state = QEI_ENCODER_GPIO_REGS->GPIO_PORT & QEI_ENCODER_PIN_MASK;
    qei_gpio_transitions = 0U;
    qei_last_control_position = (int32_t)qei_last_position;
    dc_motor_integral_error = 0;
    dc_motor_filtered_rpm_x10 = 0;
    dc_motor_last_control_ms = k_uptime_get();
    k_timer_init(&qei_gpio_sample_timer, qei_gpio_sample_timer_handler, NULL);
    k_timer_start(&qei_gpio_sample_timer, K_MSEC(1), K_MSEC(1));
    k_work_init_delayable(&qei_log_work, qei_log_work_handler);
    k_work_schedule(&qei_log_work, K_SECONDS(1));
    k_work_init_delayable(&dc_motor_control_work, dc_motor_control_work_handler);
    k_work_schedule(&dc_motor_control_work,
                    K_MSEC(DC_MOTOR_CONTROL_PERIOD_MS));

    LOG_INF("QEI configured for %u counts/output revolution", qei_counts_per_output_rev);
        LOG_INF("QEI hardware x4: runs in CPU idle, index reset disabled, filter bypassed; CON=0x%08x IOC=0x%08x",
            (unsigned int)QEI_REGS->QEI_QEICON,
            (unsigned int)QEI_REGS->QEI_QEIIOC);
        LOG_INF("CPR=194 supported by hand-turn test; verify multi-turn count for precision");
#if defined(CONFIG_BOARD_BZ6_MOTOR_CONTROL_BOARD)
        LOG_INF("Motor 24 V / %u RPM; PI enabled=%u; PC8=QEA PC9=QEB; normal QEI decoding (CW positive, CCW negative)",
#else
        LOG_INF("Motor 24 V / %u RPM; PI enabled=%u; PB6=QEA PB4=QEB",
#endif
            DC_MOTOR_MAX_RPM, DC_MOTOR_CLOSED_LOOP_ENABLED);

    LOG_INF("DC motor BLE module initialized for MPPB A/B half-bridge control");

#if !DC_MOTOR_OUTPUTS_AVAILABLE
    LOG_WRN("DC motor MPPB devicetree aliases are not defined, motor control disabled");
    return -ENODEV;
#else
    if (!pwm_is_ready_dt(&motor_a_high_pwm) || !pwm_is_ready_dt(&motor_b_high_pwm)) {
        LOG_ERR("DC motor PWM device not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&motor_a_low) || !gpio_is_ready_dt(&motor_b_low)) {
        LOG_ERR("DC motor GPIO device not ready");
        return -ENODEV;
    }

    int ret = gpio_pin_configure_dt(&motor_a_low, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) {
        LOG_ERR("Failed to configure MPPB Drive A Low GPIO: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&motor_b_low, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) {
        LOG_ERR("Failed to configure MPPB Drive B Low GPIO: %d", ret);
        return ret;
    }

    ret = dc_motor_stop_outputs();
    LOG_INF("DC motor set to default state: stopped, all A/B bridge drives off");

    if (ret != 0) {
        LOG_ERR("Failed to stop DC motor outputs: %d", ret);
        return ret;
    }

    LOG_INF("Control algorithm: %s; QEI feedback, feed-forward PWM, 100 ms updates, 95%% duty limit",
            DC_MOTOR_CLOSED_LOOP_ENABLED ? "closed-loop PI (proportional-integral)" : "open-loop PWM");
    return 0;
#endif
}

static int handle_ui_update_request(struct bt_conn *conn)
{
    LOG_INF("=== Handle UI Update Request (0xA1) ===");

    int ret = app_dc_motor_ble_send_ack(conn, DC_ACK_UI_UPDATE);
    if (ret != 0) {
        LOG_ERR("Failed to send UI update ACK: %d", ret);
        return ret;
    }

    return app_dc_motor_ble_send_ui_update_response(conn);
}

static int handle_slider_set(struct bt_conn *conn, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 2) {
        LOG_ERR("Invalid slider set payload length: %d", payload_len);
        return -EINVAL;
    }

    LOG_INF("=== Handle Slider Set (0xA2) ===");

    uint16_t slider_value = (payload[0] << 8) | payload[1];

    if (slider_value > DC_MOTOR_MAX_RPM) {
        LOG_WRN("Clamping DC motor RPM value %d to max %d", slider_value, DC_MOTOR_MAX_RPM);
        slider_value = DC_MOTOR_MAX_RPM;
    }

    motor_state.slider_value = slider_value;
    dc_motor_integral_error = 0;
    qei_last_control_position = (int32_t)QEI_PositionGet();
    dc_motor_last_control_ms = k_uptime_get();

    LOG_INF("DC motor speed slider set to: %d rpm", slider_value);

    int ret = app_dc_motor_ble_send_ack(conn, DC_ACK_SLIDER_SET);
    if (ret != 0) {
        LOG_ERR("Failed to send slider set ACK: %d", ret);
        return ret;
    }

    ret = dc_motor_apply_from_slider(slider_value);
    if (ret != 0) {
        LOG_ERR("Failed to update DC motor outputs: %d", ret);
    }

    return app_dc_motor_ble_send_slider_response(conn, slider_value);
}

static int handle_fixed_direction_set(struct bt_conn *conn, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 1) {
        LOG_ERR("Invalid fixed direction set payload length: %d", payload_len);
        return -EINVAL;
    }

    LOG_INF("=== Handle Fixed Direction Set (0xA4) ===");

    uint8_t direction = payload[0];

    if (direction != DC_DIR_CW && direction != DC_DIR_CCW) {
        LOG_ERR("Invalid fixed direction value: 0x%02X", direction);
        return -EINVAL;
    }

    bool stop_for_direction_change =
        motor_state.start_stop == DC_START &&
        motor_state.outputs_active &&
        motor_state.active_direction != direction;

    motor_state.fixed_direction = direction;
    dc_motor_integral_error = 0;
    qei_last_control_position = (int32_t)QEI_PositionGet();
    dc_motor_last_control_ms = k_uptime_get();

    LOG_INF("Fixed direction set to: %s", direction == DC_DIR_CW ? "CW" : "CCW");

    int ret = app_dc_motor_ble_send_ack(conn, DC_ACK_FIXED_DIRECTION_SET);
    if (ret != 0) {
        LOG_ERR("Failed to send fixed direction set ACK: %d", ret);
        return ret;
    }

    if (stop_for_direction_change) {
        ret = dc_motor_stop_outputs();
        if (ret != 0) {
            LOG_ERR("Failed to stop motor before direction change: %d", ret);
        } else {
            motor_state.start_stop = DC_STOP;
            LOG_INF("Motor stopped due to direction change");

            ret = app_dc_motor_ble_send_start_stop_notify(conn, DC_STOP);
            if (ret != 0) {
                LOG_ERR("Failed to send start/stop notify: %d", ret);
            }
        }
    } else {
        ret = dc_motor_apply_from_slider(motor_state.slider_value);
        if (ret != 0) {
            LOG_ERR("Failed to update DC motor outputs: %d", ret);
        }
    }

    return app_dc_motor_ble_send_fixed_direction_response(conn, direction);
}

static int handle_start_stop_set(struct bt_conn *conn, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 1) {
        LOG_ERR("Invalid start/stop set payload length: %d", payload_len);
        return -EINVAL;
    }

    LOG_INF("=== Handle START/STOP Set (0xA5) ===");

    uint8_t start_stop = payload[0];

    if (start_stop != DC_STOP && start_stop != DC_START) {
        LOG_ERR("Invalid start/stop value: 0x%02X", start_stop);
        return -EINVAL;
    }

    motor_state.start_stop = start_stop;
    dc_motor_integral_error = 0;
    qei_last_control_position = (int32_t)QEI_PositionGet();
    dc_motor_last_control_ms = k_uptime_get();

    LOG_INF("Start/stop set to: %s", start_stop == DC_START ? "START" : "STOP");

    int ret = app_dc_motor_ble_send_ack(conn, DC_ACK_START_STOP_SET);
    if (ret != 0) {
        LOG_ERR("Failed to send start/stop set ACK: %d", ret);
        return ret;
    }

    ret = dc_motor_apply_from_slider(motor_state.slider_value);
    if (ret != 0) {
        LOG_ERR("Failed to update DC motor outputs: %d", ret);
    }

    return app_dc_motor_ble_send_start_stop_response(conn, start_stop);
}

int app_dc_motor_ble_handle_cmd(struct bt_conn *conn, uint8_t opcode,
                                const uint8_t *payload, uint16_t payload_len)
{
    if (opcode != DC_MOTOR_OPCODE) {
        LOG_ERR("Invalid opcode: 0x%02X, expected 0x%02X", opcode, DC_MOTOR_OPCODE);
        return -EINVAL;
    }

    if (!payload || payload_len < 2) {
        LOG_ERR("Invalid payload length: %d (expected at least 2)", payload_len);
        return -EINVAL;
    }

    uint8_t cmd_length = payload[0];
    uint8_t cmd_id = payload[1];
    const uint8_t *cmd_payload = payload + 2;
    uint16_t cmd_payload_len = payload_len - 2;

    LOG_INF("=== Vendor Command Received ===");
    LOG_INF("Decoded vendor command length=%u id=0x%02X payload_len=%u", cmd_length, cmd_id, cmd_payload_len);
    log_vendor_cmd("RX", opcode, payload, payload_len);

    if (cmd_length < 1 || cmd_length != (uint8_t)(cmd_payload_len + 1)) {
        LOG_WRN("Vendor command length mismatch: length_field=%u expected=%u", cmd_length, cmd_payload_len + 1);
        if (cmd_length > (uint8_t)(payload_len - 1)) {
            return -EINVAL;
        }
    }

    switch (cmd_id) {
    case DC_CMD_UI_UPDATE_REQ:
        return handle_ui_update_request(conn);

    case DC_CMD_SLIDER_SET:
        return handle_slider_set(conn, cmd_payload, cmd_payload_len);

    case DC_CMD_FIXED_DIRECTION_SET:
        return handle_fixed_direction_set(conn, cmd_payload, cmd_payload_len);

    case DC_CMD_START_STOP_SET:
        return handle_start_stop_set(conn, cmd_payload, cmd_payload_len);

    default:
        LOG_ERR("Unknown command ID: 0x%02X", cmd_id);
        return -EINVAL;
    }
}

int app_dc_motor_ble_send_ui_update_response(struct bt_conn *conn)
{
    uint8_t payload[64];
    uint16_t payload_len = 1;

    payload[0] = 0;
    payload[payload_len++] = DC_RSP_UI_UPDATE;
    payload[payload_len++] = motor_state.name_len;

    memcpy(&payload[payload_len], motor_state.name, motor_state.name_len);
    payload_len += motor_state.name_len;

    payload[payload_len++] = motor_state.feedback_len;
    memcpy(&payload[payload_len], motor_state.feedback, motor_state.feedback_len);
    payload_len += motor_state.feedback_len;

    payload[payload_len++] = (DC_MOTOR_VOLTAGE >> 8) & 0xFF;
    payload[payload_len++] = DC_MOTOR_VOLTAGE & 0xFF;

    payload[payload_len++] = (DC_MOTOR_MIN_RPM >> 8) & 0xFF;
    payload[payload_len++] = DC_MOTOR_MIN_RPM & 0xFF;

    payload[payload_len++] = (DC_MOTOR_MAX_RPM >> 8) & 0xFF;
    payload[payload_len++] = DC_MOTOR_MAX_RPM & 0xFF;

    payload[payload_len++] = (motor_state.slider_value >> 8) & 0xFF;
    payload[payload_len++] = motor_state.slider_value & 0xFF;

    payload[payload_len++] = motor_state.fixed_direction;
    payload[payload_len++] = motor_state.start_stop;

    payload[0] = payload_len - 1;

    LOG_INF("=== Sending UI Update Response (0xB1) ===");
    log_vendor_cmd("TX", DC_MOTOR_OPCODE, payload, payload_len);

    return trs_server_send_vendor_command(conn, DC_MOTOR_OPCODE, payload_len, payload);
}

int app_dc_motor_ble_send_slider_response(struct bt_conn *conn, uint16_t slider_value)
{
    uint8_t payload[5];

    payload[0] = 3;
    payload[1] = DC_RSP_SLIDER_SET;
    payload[2] = (slider_value >> 8) & 0xFF;
    payload[3] = slider_value & 0xFF;

    LOG_INF("=== Sending Slider Response (0xB2) - Value: %d ===", slider_value);
    log_vendor_cmd("TX", DC_MOTOR_OPCODE, payload, 4);

    return trs_server_send_vendor_command(conn, DC_MOTOR_OPCODE, 4, payload);
}

int app_dc_motor_ble_send_fixed_direction_response(struct bt_conn *conn, uint8_t direction)
{
    uint8_t payload[3];

    payload[0] = 2;
    payload[1] = DC_RSP_FIXED_DIRECTION_SET;
    payload[2] = direction;

    LOG_INF("=== Sending Fixed Direction Response (0xB4) - Direction: %s ===",
            direction == DC_DIR_CW ? "CW" : "CCW");
    log_vendor_cmd("TX", DC_MOTOR_OPCODE, payload, 3);

    return trs_server_send_vendor_command(conn, DC_MOTOR_OPCODE, 3, payload);
}

int app_dc_motor_ble_send_start_stop_response(struct bt_conn *conn, uint8_t start_stop)
{
    uint8_t payload[3];

    payload[0] = 2;
    payload[1] = DC_RSP_START_STOP_SET;
    payload[2] = start_stop;

    LOG_INF("=== Sending START/STOP Response (0xB5) - State: %s ===",
            start_stop == DC_START ? "START" : "STOP");
    log_vendor_cmd("TX", DC_MOTOR_OPCODE, payload, 3);

    return trs_server_send_vendor_command(conn, DC_MOTOR_OPCODE, 3, payload);
}

int app_dc_motor_ble_send_slider_notify(struct bt_conn *conn, uint16_t slider_value)
{
    uint8_t payload[4];

    payload[0] = 3;
    payload[1] = DC_NTF_SLIDER;
    payload[2] = (slider_value >> 8) & 0xFF;
    payload[3] = slider_value & 0xFF;

    LOG_INF("=== Sending Slider Notification (0xC2) - Value: %d ===", slider_value);
    log_vendor_cmd("TX", DC_MOTOR_OPCODE, payload, 4);

    return trs_server_send_vendor_command(conn, DC_MOTOR_OPCODE, 4, payload);
}

int app_dc_motor_ble_send_fixed_direction_notify(struct bt_conn *conn, uint8_t direction)
{
    uint8_t payload[3];

    payload[0] = 2;
    payload[1] = DC_NTF_FIXED_DIRECTION;
    payload[2] = direction;

    LOG_INF("=== Sending Fixed Direction Notification (0xC4) - Direction: %s ===",
            direction == DC_DIR_CW ? "CW" : "CCW");
    log_vendor_cmd("TX", DC_MOTOR_OPCODE, payload, 3);

    return trs_server_send_vendor_command(conn, DC_MOTOR_OPCODE, 3, payload);
}

int app_dc_motor_ble_send_start_stop_notify(struct bt_conn *conn, uint8_t start_stop)
{
    uint8_t payload[3];

    payload[0] = 2;
    payload[1] = DC_NTF_START_STOP;
    payload[2] = start_stop;

    LOG_INF("=== Sending START/STOP Notification (0xC5) - State: %s ===",
            start_stop == DC_START ? "START" : "STOP");
    log_vendor_cmd("TX", DC_MOTOR_OPCODE, payload, 3);

    return trs_server_send_vendor_command(conn, DC_MOTOR_OPCODE, 3, payload);
}

int app_dc_motor_ble_send_ack(struct bt_conn *conn, uint8_t ack_id)
{
    if (ack_id != DC_ACK_UI_UPDATE &&
        ack_id != DC_ACK_SLIDER_SET &&
        ack_id != DC_ACK_FIXED_DIRECTION_SET &&
        ack_id != DC_ACK_START_STOP_SET) {
        LOG_ERR("Invalid ACK ID: 0x%02X", ack_id);
        return -EINVAL;
    }

    uint8_t payload[2];
    payload[0] = 1;
    payload[1] = ack_id;

    LOG_INF("=== Sending ACK (0x%02X) ===", ack_id);
    log_vendor_cmd("TX", DC_MOTOR_OPCODE, payload, 2);

    return trs_server_send_vendor_command(conn, DC_MOTOR_OPCODE, 2, payload);
}
