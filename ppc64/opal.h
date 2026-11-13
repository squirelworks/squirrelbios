/* SquirrelBIOS ppc64le-powernv: the OPAL-style runtime interface.
 *
 * Token numbers, return codes and argument layouts follow the public
 * OPAL API documentation (doc/opal-api in the OpenPOWER firmware docs),
 * so an OS written for OPAL finds the calls where it expects them. The
 * implementation is SquirrelBIOS's own.
 */
#ifndef SQ_PPC64_OPAL_H
#define SQ_PPC64_OPAL_H

#define OPAL_TEST                       0
#define OPAL_CONSOLE_WRITE              1
#define OPAL_CONSOLE_READ               2
#define OPAL_RTC_READ                   3
#define OPAL_RTC_WRITE                  4
#define OPAL_CEC_POWER_DOWN             5
#define OPAL_CEC_REBOOT                 6
#define OPAL_POLL_EVENTS                10
#define OPAL_CONSOLE_WRITE_BUFFER_SPACE 25
#define OPAL_START_CPU                  41
#define OPAL_QUERY_CPU_STATUS           42
#define OPAL_RETURN_CPU                 69
#define OPAL_REINIT_CPUS                70
#define OPAL_CHECK_TOKEN                80
#define OPAL_CONSOLE_FLUSH              117

#define OPAL_SUCCESS            0
#define OPAL_PARAMETER          -1
#define OPAL_BUSY               -2
#define OPAL_PARTIAL            -3
#define OPAL_CLOSED             -5
#define OPAL_HARDWARE           -6
#define OPAL_UNSUPPORTED        -7
#define OPAL_INTERNAL_ERROR     -11
#define OPAL_WRONG_STATE        -14

#define OPAL_TOKEN_ABSENT       0
#define OPAL_TOKEN_PRESENT      1

/* OPAL_QUERY_CPU_STATUS */
#define OPAL_THREAD_INACTIVE    0
#define OPAL_THREAD_STARTED     1
#define OPAL_THREAD_UNAVAILABLE 2

#endif
