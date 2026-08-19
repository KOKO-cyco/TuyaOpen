/**
 * @file ikcp_pacing.h
 * @brief KCP send pacing (TuyaOS mid_p2p ikcp_pacing)
 * @version 1.0
 * @date 2026-08-04
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __IKCP_PACING_H__
#define __IKCP_PACING_H__

#include <stdint.h>

/**
 * @brief Compile-time switch for KCP send pacing.
 *
 * On means ikcp_flush spreads the congestion window across the RTT instead of
 * handing it to the socket all at once. Off restores the burst behaviour, which
 * is only useful for reproducing the loss pattern it was introduced to fix.
 *
 * This was previously off for good reason: the earlier implementation derived
 * its own bandwidth estimate and could throttle the stream far below the link,
 * and it advanced its next-send deadline off kcp->current, which does not move
 * within a flush - so it let exactly one segment through per flush regardless
 * of the rate it had computed. Both are gone; the rate now comes from cwnd and
 * srtt, which KCP already maintains.
 */
#ifndef IKCP_PACING_RATE_LIMIT
#define IKCP_PACING_RATE_LIMIT 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ikcp.h sits next to ikcp.c rather than on the include path, so it cannot be
 * pulled in from here - forward declare instead, as this header always has. */
struct IKCPCB;
typedef struct IKCPCB ikcpcb;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Allocate and attach pacing state to kcp
 * @param[in,out] kcp kcp control block
 * @return 0 on success, <0 on failure
 */
int pacing_init(ikcpcb *kcp);

/**
 * @brief Free pacing state
 * @param[in,out] kcp kcp control block
 * @return none
 */
void pacing_fini(ikcpcb *kcp);

/**
 * @brief Open a new pacing budget for the flush about to run
 * @param[in,out] kcp kcp control block
 * @return none
 * @note Call once per ikcp_flush, before any data segment is considered. The
 *       budget is one flush period's worth of the cwnd/srtt rate, so a whole
 *       window takes an RTT to reach the wire.
 */
void pacing_flush_begin(ikcpcb *kcp);

/**
 * @brief Charge a packet against this flush's budget
 * @param[in,out] kcp kcp control block
 * @param[in] pkt_len wire length of the packet, retransmissions included
 * @return 1 if it may be sent now, 0 if the budget is spent (stop the flush)
 */
int pacing_try_send(ikcpcb *kcp, uint32_t pkt_len);

#ifdef __cplusplus
}
#endif

#endif /* __IKCP_PACING_H__ */
