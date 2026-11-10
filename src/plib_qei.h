/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PLIB_QEI_H
#define PLIB_QEI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <soc.h>

#include "plib_qei_common.h"

#if defined(CONFIG_BOARD_BZ6_MOTOR_CONTROL_BOARD)
#define QEI_ENCODER_GPIO_REGS GPIOC_REGS
#define QEI_ENCODER_A_PIN 8U
#define QEI_ENCODER_B_PIN 9U
#define QEI_ENCODER_A_NAME "PC8"
#define QEI_ENCODER_B_NAME "PC9"
#define QEI_ENCODER_PORT_NAME "PORTC"
#else
#define QEI_ENCODER_GPIO_REGS GPIOB_REGS
#define QEI_ENCODER_A_PIN 6U
#define QEI_ENCODER_B_PIN 4U
#define QEI_ENCODER_A_NAME "PB6"
#define QEI_ENCODER_B_NAME "PB4"
#define QEI_ENCODER_PORT_NAME "PORTB"
#endif

#define QEI_ENCODER_PIN_MASK ((1UL << QEI_ENCODER_A_PIN) | (1UL << QEI_ENCODER_B_PIN))

#ifdef __cplusplus
extern "C" {
#endif

void QEI_Initialize(void);
void QEI_Start(void);
void QEI_Stop(void);

static inline uint32_t QEI_PositionGet(void)
{
    return QEI_REGS->QEI_POSCNT;
}

static inline uint32_t QEI_VelocityGet(void)
{
    return QEI_REGS->QEI_VELCNT;
}

static inline uint32_t QEI_RevolutionsGet(void)
{
    return QEI_REGS->QEI_INDXCNT;
}

uint32_t QEI_PulseIntervalGet(void);
void QEI_PositionWindowSet(uint32_t high_threshold, uint32_t low_threshold);
void QEI_PositionCountSet(uint32_t position_count);
void QEI_VelocityCountSet(uint32_t velocity_count);

#ifdef __cplusplus
}
#endif

#endif /* PLIB_QEI_H */