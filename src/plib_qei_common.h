/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PLIB_QEI_COMMON_H
#define PLIB_QEI_COMMON_H

#include <stdint.h>
#include <soc.h>

#define QEI_STATUS_NONE                  (0U)
#define QEI_INDEX_STATUS                 ((uint32_t)QEI_QEISTAT_IDXIRQ_Msk)
#define QEI_HOME_STATUS                  ((uint32_t)QEI_QEISTAT_HOMIRQ_Msk)
#define QEI_VELOCITY_OVERFLOW_STATUS     ((uint32_t)QEI_QEISTAT_VELOVIRQ_Msk)
#define QEI_POSITION_OVERFLOW_STATUS     ((uint32_t)QEI_QEISTAT_POSOVIRQ_Msk)
#define QEI_POS_INIT_COMPLETE_STATUS     ((uint32_t)QEI_QEISTAT_PCIIRQ_Msk)
#define QEI_POS_LESS_EQ_STATUS           ((uint32_t)QEI_QEISTAT_PCLEQIRQ_Msk)
#define QEI_POS_HIGH_EQ_STATUS           ((uint32_t)QEI_QEISTAT_PCHEQIRQ_Msk)
#define QEI_STATUS_MASK                  (QEI_INDEX_STATUS | QEI_HOME_STATUS | \
                                          QEI_VELOCITY_OVERFLOW_STATUS | \
                                          QEI_POSITION_OVERFLOW_STATUS | \
                                          QEI_POS_INIT_COMPLETE_STATUS | \
                                          QEI_POS_LESS_EQ_STATUS | \
                                          QEI_POS_HIGH_EQ_STATUS)

typedef uint32_t QEI_STATUS;
typedef void (*QEI_CALLBACK)(QEI_STATUS status, uintptr_t context);

typedef struct {
    QEI_CALLBACK callback;
    uintptr_t context;
} QEI_CH_OBJECT;

#endif /* PLIB_QEI_COMMON_H */