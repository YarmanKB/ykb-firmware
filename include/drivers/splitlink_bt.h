#ifndef __DRIVERS_SPLITLINK_BT_H_
#define __DRIVERS_SPLITLINK_BT_H_

#if CONFIG_SPLITLINK_BT_CENTRAL
int splitlink_bt_start(void);
#else
#include <sys/errno.h>
static inline int splitlink_bt_start(void) { return -ENOSYS; }
#endif

#endif // __DRIVERS_SPLITLINK_BT_H_
