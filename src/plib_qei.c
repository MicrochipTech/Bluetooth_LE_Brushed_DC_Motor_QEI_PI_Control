/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <soc.h>

#include "plib_qei.h"

void QEI_Initialize(void)
{
    QEI_Stop();

    CFG_REGS->CFG_PMD3 &= ~CFG_PMD3_QEIMD_Msk;

    QEI_ENCODER_GPIO_REGS->GPIO_ANSELCLR = QEI_ENCODER_PIN_MASK;
    QEI_ENCODER_GPIO_REGS->GPIO_TRISSET = QEI_ENCODER_PIN_MASK;

#if defined(CONFIG_BOARD_BZ6_MOTOR_CONTROL_BOARD)
    /* Route the confirmed MCB phase order directly; no decoder swap is needed. */
    PPS_REGS->PPS_QEIQEA0G4R = 0U;
    PPS_REGS->PPS_QEIQEB0G5R = 0U;
    PPS_REGS->PPS_QEIQEA0G3R = PPS_QEIQEA0G3R_QEIQEA0G3_RPC8;
    PPS_REGS->PPS_QEIQEB0G4R = PPS_QEIQEB0G4R_QEIQEB0G4_RPC9;
#else
    PPS_REGS->PPS_QEIQEA0G4R = PPS_QEIQEA0G4R_QEIQEA0G4_RPB6;
    PPS_REGS->PPS_QEIQEB0G4R = PPS_QEIQEB0G4R_QEIQEB0G4_RPB4;
#endif

    QEI_REGS->QEI_QEICON = QEI_QEICON_CCM_COUNT_MODE |
                           QEI_QEICON_PIMOD_0 |
                           QEI_QEICON_INTDIV_1_1 |
                           QEI_QEICON_QEISIDL_CONT;
    QEI_REGS->QEI_QEIIOC = QEI_QEIIOC_QFDIV_1_1;
    QEI_REGS->QEI_QEIICC = 200U;
    QEI_REGS->QEI_QEICMPL = 1U;
    QEI_REGS->QEI_QEISTAT = 0U;
    QEI_REGS->QEI_POSCNT = 0U;
    QEI_REGS->QEI_VELCNT = 0U;
}

void QEI_Start(void)
{
    QEI_REGS->QEI_QEICONSET = QEI_QEICON_ON_Msk;
}

void QEI_Stop(void)
{
    QEI_REGS->QEI_QEICONCLR = QEI_QEICON_ON_Msk;
}

uint32_t QEI_PulseIntervalGet(void)
{
    return QEI_REGS->QEI_INTHLD;
}

void QEI_PositionWindowSet(uint32_t high_threshold, uint32_t low_threshold)
{
    QEI_REGS->QEI_QEIICC = high_threshold;
    QEI_REGS->QEI_QEICMPL = low_threshold;
}

void QEI_PositionCountSet(uint32_t position_count)
{
    QEI_REGS->QEI_POSCNT = position_count;
}

void QEI_VelocityCountSet(uint32_t velocity_count)
{
    QEI_REGS->QEI_VELCNT = velocity_count;
}