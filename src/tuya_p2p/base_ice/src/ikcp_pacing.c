/**
 * @file ikcp_pacing.c
 * @brief KCP send pacing: spread a congestion window across the RTT
 * @version 2.0
 * @date 2026-08-14
 * @copyright Copyright (c) Tuya Inc.
 *
 * @note Without pacing, ikcp_flush hands the whole congestion window to the
 *       socket in one tight loop. On this hardware that is a 94 kB burst once
 *       cwnd reaches ~67 segments, which the Wi-Fi queue cannot absorb: it tail
 *       drops a long contiguous run, every segment in it times out together
 *       (bursts of 28-31 RTOs were measured), and cwnd restarts from 1. The
 *       flow then climbs back and does it again. Sending the same window spread
 *       over an RTT keeps the queue shallow and the losses isolated, which is
 *       also the only shape fast retransmit can repair.
 *
 *       The rate comes from cwnd/srtt, as Linux TCP does, rather than from an
 *       independent bandwidth estimate: KCP already maintains both, and a
 *       second estimator built on top of a rate it is itself limiting only
 *       measures its own output.
 */
#include "ikcp_pacing.h"
#include "ikcp.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
/*
 * Gain over the plain cwnd/RTT rate, matching what the Linux kernel paces at:
 * twice the window while slow start is still doubling, 1.2x afterwards so the
 * flow can still discover new headroom without bursting to find it.
 */
#define PACING_GAIN_SS_NUM  2U
#define PACING_GAIN_SS_DEN  1U
#define PACING_GAIN_CA_NUM  6U
#define PACING_GAIN_CA_DEN  5U

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
struct pacing {
    IUINT32 budget; /* wire bytes still allowed in the current flush */
};

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
int pacing_init(ikcpcb *kcp)
{
    struct pacing *p;

    if (kcp == NULL) {
        return -1;
    }
    p = (struct pacing *)ikcp_malloc(sizeof(*p));
    if (p == NULL) {
        return -1;
    }
    memset(p, 0, sizeof(*p));
    kcp->pacing = p;
    return 0;
}

void pacing_fini(ikcpcb *kcp)
{
    if (kcp == NULL || kcp->pacing == NULL) {
        return;
    }
    ikcp_free(kcp->pacing);
    kcp->pacing = NULL;
}

void pacing_flush_begin(ikcpcb *kcp)
{
    struct pacing *p;
    IUINT32 wnd;
    IUINT32 srtt;
    IUINT32 gain_num;
    IUINT32 gain_den;
    IUINT64 budget;

    if (kcp == NULL || kcp->pacing == NULL) {
        return;
    }
    p = (struct pacing *)kcp->pacing;

    /* The window flush will actually use - see the same calculation there. */
    wnd = (kcp->snd_wnd < kcp->rmt_wnd) ? kcp->snd_wnd : kcp->rmt_wnd;
    if (kcp->nocwnd == 0 && kcp->cwnd < wnd) {
        wnd = kcp->cwnd;
    }
    if (wnd < 1) {
        wnd = 1;
    }

    /*
     * Before the first RTT sample srtt is 0; rx_rto carries the initial
     * estimate. Never let the divisor fall below one flush period, or a fast
     * link would compute a budget larger than the window it is pacing.
     */
    srtt = (kcp->rx_srtt > 0) ? (IUINT32)kcp->rx_srtt : (IUINT32)kcp->rx_rto;
    if (srtt < kcp->interval) {
        srtt = kcp->interval;
    }

    if (kcp->cwnd < kcp->ssthresh) {
        gain_num = PACING_GAIN_SS_NUM;
        gain_den = PACING_GAIN_SS_DEN;
    } else {
        gain_num = PACING_GAIN_CA_NUM;
        gain_den = PACING_GAIN_CA_DEN;
    }

    /* (window bytes / srtt) is the target rate; multiply by one flush period. */
    budget = (IUINT64)wnd * kcp->mss;
    budget = (budget * kcp->interval * gain_num) / ((IUINT64)srtt * gain_den);

    /*
     * One full packet is the floor. A window of one on a link with a long RTT
     * budgets less than a packet per flush, and rounding that down to zero
     * would stall the flow completely instead of merely slowing it.
     */
    if (budget < kcp->mtu) {
        budget = kcp->mtu;
    }
    p->budget = (IUINT32)budget;
}

int pacing_try_send(ikcpcb *kcp, uint32_t pkt_len)
{
    struct pacing *p;

    if (kcp == NULL || kcp->pacing == NULL) {
        return 1;
    }
    p = (struct pacing *)kcp->pacing;

    if (p->budget == 0) {
        return 0;
    }
    /* Retransmissions are charged too: they occupy the same queue. */
    p->budget = (pkt_len >= p->budget) ? 0 : (p->budget - pkt_len);
    return 1;
}
