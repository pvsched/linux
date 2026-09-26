/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_EVENT_SERVICE_H
#define _VIRT_PVSCHED_EVENT_SERVICE_H

#include <linux/types.h>

int pvsched_event_service_get(void);
void pvsched_event_service_put(void);

#if IS_ENABLED(CONFIG_KUNIT)
unsigned int pvsched_event_service_holders(void);
u32 pvsched_mode_flags(u32 flags);
#endif

#endif /* _VIRT_PVSCHED_EVENT_SERVICE_H */
