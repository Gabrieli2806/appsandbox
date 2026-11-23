/*
 * asb_types.h -- VM event types shared by the orchestrator (asb_core) and
 * the UI layers.
 */

#ifndef ASB_TYPES_H
#define ASB_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CORE_VM_NAME_MAX       128

/* ---- Asynchronous events from the backend ---- */

#define CORE_VM_EVENT_STATE_CHANGED   1   /* int_value: 1=running, 0=stopped */
#define CORE_VM_EVENT_PROGRESS        2   /* int_value: 0-100 */
#define CORE_VM_EVENT_LOG             3   /* str_value: log line */
#define CORE_VM_EVENT_ALERT           4   /* str_value: user-visible error */
#define CORE_VM_EVENT_INSTALL_STATUS  5   /* str_value: phase string */
#define CORE_VM_EVENT_LIST_CHANGED    6   /* entire VM list changed */
#define CORE_VM_EVENT_AGENT_STATUS    7   /* int_value: 1=online, 0=offline */
#define CORE_VM_EVENT_DIAG            8

typedef struct CoreVmEvent {
    uint64_t    vm_id;
    int         type;
    char        vm_name[CORE_VM_NAME_MAX];
    int         int_value;
    const char *str_value;  /* may be NULL */
    int         staging;
} CoreVmEvent;

typedef void (*CoreVmEventCallback)(const CoreVmEvent *event, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* ASB_TYPES_H */
