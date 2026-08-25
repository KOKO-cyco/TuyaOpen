/**
 * @file ikcp_pacing.c
 * @brief KCP send pacing: pace at the measured delivery rate
 * @version 3.0
 * @date 2026-08-25
 * @copyright Copyright (c) Tuya Inc.
 *
 * @note Without pacing, ikcp_flush hands the whole congestion window to the
 *       socket in one tight loop. On this hardware that is a 94 kB burst once
 *       cwnd reaches ~67 segments, which the Wi-Fi queue cannot absorb: it tail
 *       drops a long contiguous run, every segment in it times out together
 *       (bursts of 28-31 RTOs were measured), and cwnd restarts from 1.
 *
 *       Version 2 paced at cwnd/srtt. That removed the bursts but left the flow
 *       at ~200 kbps on a link measured to carry far more, because both terms
 *       are wrong on a bloated link: srtt is the queue's depth rather than the
 *       path's latency (3 ms idle, 230 ms average and 1.2 s peak under a
 *       1.1 Mbit/s load on the board's Wi-Fi), and cwnd only moves after the
 *       queue is already full enough to lose something. A rate built from those
 *       two tracks the queue, not the link.
 *
 *       So the rate now comes from what the peer actually acknowledges: bytes
 *       delivered over the time they took, filtered by a windowed maximum. That
 *       is the bottleneck's drain rate, and pacing at it keeps the queue shallow
 *       instead of discovering its depth. The earlier objection to measuring
 *       delivery - that an estimator sitting on a rate it is itself limiting
 *       only ever measures its own output - is what the gain cycle answers: one
 *       phase in eight sends 25% faster than the estimate, so headroom is probed
 *       for continuously, and the windowed max remembers the best result for ten
 *       round trips rather than believing the latest sample.
 *
 *       Rate alone is not enough. Pacing exactly as fast as the link drains
 *       holds a queue at whatever depth it already reached, and cwnd will not
 *       bring it down either when the bottleneck buffers deeply enough that
 *       nothing is ever dropped. So what may be outstanding is also capped, at
 *       two bandwidth-delay products measured from the same delivery rate and
 *       from the smallest round trip seen recently. That cap is what drains the
 *       queue; the rate is what keeps it from refilling.
 */
#include "ikcp_pacing.h"
#include "ikcp_minmax.h"
#include "ikcp.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
/*
 * Gain cycle, in units of 1/256. Seven phases at the estimate and one at 1.25x
 * to look for headroom; the 0.75x phase after it drains whatever queue the
 * probe built before settling back. Same shape and same constants the vendor
 * stack uses, which is BBR's ProbeBW cycle.
 */
#define PACING_GAIN_UNIT    256U
#define PACING_GAIN_PHASES  8U

/*
 * How long the windowed maximum remembers a delivery rate sample, in round
 * trips. Long enough that one starved interval cannot talk the flow down,
 * short enough that a link which genuinely got slower is believed.
 */
#define PACING_BW_WIN_RTTS  10U

/*
 * Gain applied before any delivery sample exists, over the cwnd/srtt fallback.
 * Matches what the kernel paces at while slow start is still doubling.
 */
#define PACING_STARTUP_NUM  2U
#define PACING_STARTUP_DEN  1U

/* Leave a sliver of the estimate unused so pacing cannot itself build a queue. */
#define PACING_MARGIN_PCT   1U

/*
 * How long the windowed minimum remembers an RTT sample. This has to outlast
 * any queue the flow might build, or the "unloaded" latency it is supposed to
 * hold would just be the queue's own depth measured a moment ago.
 */
#define PACING_RTT_WIN_MS   10000U

/*
 * In-flight ceiling, as a multiple of the bandwidth-delay product.
 *
 * Pacing at the delivery rate holds a queue at whatever depth it already has;
 * it cannot drain one, because sending exactly as fast as the link drains keeps
 * the backlog constant. Bounding what may be outstanding is what empties it,
 * and it is also the only brake that still works when the bottleneck buffers so
 * deeply that nothing is ever dropped to make cwnd back off - which is the case
 * on the board's Wi-Fi. Two BDPs leaves room for delayed acks without leaving
 * room for a standing queue.
 */
#define PACING_INFLIGHT_BDP 2U
#define PACING_INFLIGHT_MIN_PKTS 4U

/*
 * ProbeRTT.
 *
 * A windowed minimum can only remember a round trip it has actually seen, and a
 * flow that filled the bottleneck before its first ack came back never sees an
 * unloaded one: measured here, the "minimum" settled at 2471 ms on a path whose
 * real round trip was 40 ms, and the ceiling built from it was 129 kB - no
 * ceiling at all. So the queue is deliberately drained from time to time, by
 * holding in flight down to a few packets until a true sample lands.
 *
 * The first one comes early because the estimate is worthless until it happens
 * and this carries live video; after that the standard period applies, since
 * draining costs throughput and the path length rarely changes.
 */
/*
 * What may be outstanding before anything has been measured. Same value TCP
 * opens with, and for the same reason: a sender that has not yet heard from its
 * peer knows nothing about the path and should not be able to commit a large
 * backlog to it. Without this the flow filled a five second queue during the
 * first second, and no probe short enough to be useful could then drain it.
 */
#define PACING_INIT_INFLIGHT_PKTS 10U

#define PACING_PROBERTT_FIRST_MS  1000U
#define PACING_PROBERTT_PERIOD_MS 10000U
#define PACING_PROBERTT_MS        200U

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
struct pacing {
    struct minmax bw;          /* windowed max delivery rate, bytes/sec */
    struct minmax rtt;         /* windowed min rtt, ms - the unloaded latency */
    IUINT32 delivered;         /* cumulative wire bytes the peer has acked */
    IUINT32 sample_delivered;  /* delivered as of the last rate sample */
    IUINT32 sample_stamp;      /* when that sample was taken */
    IUINT32 cycle_stamp;       /* when the current gain phase started */
    IUINT32 cycle_idx;         /* position in the gain cycle */
    IUINT32 rate;              /* current pacing rate, bytes/sec */
    IUINT32 inflight;          /* wire bytes sent once and not yet acked */
    IUINT32 inflight_cap;      /* wire bytes allowed outstanding, 0 = no limit */
    IUINT32 probe_at;          /* when the next rtt probe is due */
    IUINT32 probe_until;       /* end of the probe in progress, 0 if none */
    IUINT32 tokens;            /* wire bytes that may be sent right now */
    IUINT32 token_stamp;       /* when tokens were last topped up */
    int started;               /* a timestamp has been seen, so deltas are real */
};

static const IUINT32 pacing_gain[PACING_GAIN_PHASES] = {
    320, /* 1.25x - probe for headroom */
    192, /* 0.75x - drain what the probe built */
    256, 256, 256, 256, 256, 256,
};

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/* Signed distance between two millisecond stamps, so comparisons stay correct
 * across the 32-bit wrap. ikcp.c keeps its own copy static, hence this one. */
static IINT32 pacing_tdiff(IUINT32 later, IUINT32 earlier)
{
    return (IINT32)(later - earlier);
}

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
    /*
     * One packet of credit to open with. Without it the first flush of a flow
     * whose rate rounds to less than a packet per period would send nothing at
     * all, and nothing would ever be delivered to build an estimate from.
     */
    p->tokens = kcp->mtu;
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

void pacing_on_acked(ikcpcb *kcp, uint32_t wire_bytes)
{
    struct pacing *p;

    if (kcp == NULL || kcp->pacing == NULL) {
        return;
    }
    p = (struct pacing *)kcp->pacing;
    /* Wraps every 4 GB; every read below is a difference, so wrapping is safe. */
    p->delivered += wire_bytes;
    p->inflight = (wire_bytes >= p->inflight) ? 0 : (p->inflight - wire_bytes);
}

void pacing_on_rtt(ikcpcb *kcp, uint32_t rtt)
{
    struct pacing *p;

    if (kcp == NULL || kcp->pacing == NULL || rtt == 0) {
        return;
    }
    p = (struct pacing *)kcp->pacing;
    /*
     * The minimum, not the average: the smallest round trip seen recently is
     * the path without a queue in it, which is what the BDP has to be built
     * from. kcp->rx_srtt cannot serve here - on a bloated link it is mostly
     * queue, and a BDP computed from it would licence exactly the backlog it
     * was measuring.
     */
    minmax_running_min(&p->rtt, PACING_RTT_WIN_MS, kcp->current, rtt);
}

/* Smoothed RTT, floored at one flush period so it can never be a divisor of
 * zero and can never claim the link turns around faster than we look at it. */
static IUINT32 pacing_srtt(const ikcpcb *kcp)
{
    IUINT32 srtt = (kcp->rx_srtt > 0) ? (IUINT32)kcp->rx_srtt : (IUINT32)kcp->rx_rto;

    if (srtt < kcp->interval) {
        srtt = kcp->interval;
    }
    return srtt;
}

/*
 * Take a delivery rate sample if a full round trip of acknowledgements has
 * accumulated, and feed it to the windowed maximum.
 *
 * A shorter interval than one RTT measures the ack pattern rather than the
 * link: acks arrive in clumps, and a clump divided by a millisecond looks like
 * an enormous bandwidth that would then be paced at for ten round trips.
 */
static void pacing_sample(struct pacing *p, const ikcpcb *kcp, IUINT32 now)
{
    IUINT32 srtt = pacing_srtt(kcp);
    IUINT32 delta, acked, bw;

    if (!p->started) {
        p->started = 1;
        p->sample_stamp = now;
        p->sample_delivered = p->delivered;
        p->cycle_stamp = now;
        p->token_stamp = now;
        p->probe_at = now + PACING_PROBERTT_FIRST_MS;
        return;
    }

    if (pacing_tdiff(now, p->sample_stamp) < (IINT32)srtt) {
        return;
    }
    delta = (IUINT32)pacing_tdiff(now, p->sample_stamp);
    acked = p->delivered - p->sample_delivered;

    /*
     * Only a window that actually delivered something says anything about the
     * link. An idle stretch - the encoder between key frames, a paused viewer -
     * would otherwise enter the filter as a low rate and hold the flow down
     * long after there is data to send again.
     */
    if (acked > 0) {
        bw = (IUINT32)(((IUINT64)acked * 1000u) / delta);
        minmax_running_max(&p->bw, srtt * PACING_BW_WIN_RTTS, now, bw);
    }
    p->sample_stamp = now;
    p->sample_delivered = p->delivered;
}

/* Advance the gain cycle one phase per round trip. */
static void pacing_advance_cycle(struct pacing *p, const ikcpcb *kcp, IUINT32 now)
{
    if (pacing_tdiff(now, p->cycle_stamp) >= (IINT32)pacing_srtt(kcp)) {
        p->cycle_stamp = now;
        p->cycle_idx = (p->cycle_idx + 1u) % PACING_GAIN_PHASES;
    }
}

/*
 * Rate to pace at, in bytes per second.
 *
 * Until the first delivery sample lands there is nothing measured to pace at,
 * so fall back to the window over the RTT - the same rate version 2 used, which
 * is a reasonable opening guess and keeps a flow moving before any ack returns.
 */
static IUINT32 pacing_target_rate(const struct pacing *p, const ikcpcb *kcp)
{
    IUINT32 bw = minmax_get(&p->bw);
    IUINT32 wnd, srtt;
    IUINT64 rate;

    if (bw > 0) {
        rate = ((IUINT64)bw * pacing_gain[p->cycle_idx]) / PACING_GAIN_UNIT;
        rate = (rate * (100u - PACING_MARGIN_PCT)) / 100u;
    } else {
        wnd = (kcp->snd_wnd < kcp->rmt_wnd) ? kcp->snd_wnd : kcp->rmt_wnd;
        if (kcp->nocwnd == 0 && kcp->cwnd < wnd) {
            wnd = kcp->cwnd;
        }
        if (wnd < 1) {
            wnd = 1;
        }
        srtt = pacing_srtt(kcp);
        rate = ((IUINT64)wnd * kcp->mtu * 1000u * PACING_STARTUP_NUM) /
               ((IUINT64)srtt * PACING_STARTUP_DEN);
    }

    if (rate > 0xFFFFFFFFull) {
        rate = 0xFFFFFFFFull;
    }
    return (IUINT32)rate;
}

/*
 * Bytes allowed outstanding, or 0 while the flow has nothing measured to base
 * that on.
 */
static IUINT32 pacing_inflight_cap(struct pacing *p, const ikcpcb *kcp, IUINT32 now)
{
    IUINT32 bw, min_rtt;
    IUINT64 cap;

    /*
     * Due for a probe: squeeze in flight down to a few packets so the queue
     * ahead of us empties and the next round trip measures the path instead of
     * the backlog. Held for a fixed spell rather than until a sample arrives,
     * because on a badly bloated link the sample that ends it is itself stuck
     * behind the queue being drained.
     */
    if (pacing_tdiff(now, p->probe_at) >= 0) {
        if (p->probe_until == 0) {
            /*
             * Long enough to drain what is actually queued, not a fixed spell.
             * srtt is a measure of that backlog on a bloated link, so it is the
             * right scale; a constant 200 ms was measured ending the probe with
             * five seconds of queue still ahead of it.
             */
            IUINT32 hold = pacing_srtt(kcp);
            if (hold < PACING_PROBERTT_MS) {
                hold = PACING_PROBERTT_MS;
            }
            p->probe_until = now + hold;
        }
        if (pacing_tdiff(now, p->probe_until) < 0) {
            return PACING_INFLIGHT_MIN_PKTS * kcp->mtu;
        }
        p->probe_at = now + PACING_PROBERTT_PERIOD_MS;
        p->probe_until = 0;
    }

    bw = minmax_get(&p->bw);
    min_rtt = minmax_get(&p->rtt);
    if (bw == 0 || min_rtt == 0) {
        return PACING_INIT_INFLIGHT_PKTS * kcp->mtu;
    }
    cap = (((IUINT64)bw * min_rtt) / 1000u) * PACING_INFLIGHT_BDP;
    if (cap < (IUINT64)PACING_INFLIGHT_MIN_PKTS * kcp->mtu) {
        cap = (IUINT64)PACING_INFLIGHT_MIN_PKTS * kcp->mtu;
    }
    if (cap > 0xFFFFFFFFull) {
        cap = 0xFFFFFFFFull;
    }
    return (IUINT32)cap;
}

void pacing_flush_begin(ikcpcb *kcp)
{
    struct pacing *p;
    IUINT32 now, cap;
    IINT32 elapsed;
    IUINT64 topup;

    if (kcp == NULL || kcp->pacing == NULL) {
        return;
    }
    p = (struct pacing *)kcp->pacing;
    now = kcp->current;

    pacing_sample(p, kcp, now);
    pacing_advance_cycle(p, kcp, now);
    p->rate = pacing_target_rate(p, kcp);

    /*
     * Credit accrues with time rather than being reset each flush. A rate below
     * one packet per flush period is common on a poor link - version 2 rounded
     * that up to a whole packet every period, which put a floor of about
     * 1.1 Mbit/s under the pacer and made it unable to slow down at all on the
     * link that needed it most. Carrying the remainder lets the flow send one
     * packet every few periods instead.
     */
    elapsed = pacing_tdiff(now, p->token_stamp);
    if (elapsed > 0) {
        topup = ((IUINT64)p->rate * (IUINT32)elapsed) / 1000u;
        if (topup > 0xFFFFFFFFull) {
            topup = 0xFFFFFFFFull;
        }
        if (p->tokens > 0xFFFFFFFFu - (IUINT32)topup) {
            p->tokens = 0xFFFFFFFFu;
        } else {
            p->tokens += (IUINT32)topup;
        }
        p->token_stamp = now;
    }

    /*
     * Cap the credit at one flush period's worth plus a packet. Any more and an
     * idle flow would bank enough to burst on its first frame back, which is
     * the behaviour pacing exists to prevent; any less and a fast link could
     * not spend a period's allowance within the period.
     */
    cap = (IUINT32)(((IUINT64)p->rate * kcp->interval) / 1000u);
    if (cap > 0xFFFFFFFFu - kcp->mtu) {
        cap = 0xFFFFFFFFu - kcp->mtu;
    }
    cap += kcp->mtu;
    if (cap < 2u * kcp->mtu) {
        cap = 2u * kcp->mtu;
    }
    if (p->tokens > cap) {
        p->tokens = cap;
    }

    /*
     * Ceiling on what may be outstanding, from the measured BDP. Left at zero
     * until both terms have been measured, so a flow that has not yet learned
     * anything is governed by cwnd alone rather than by a guess.
     */
    p->inflight_cap = pacing_inflight_cap(p, kcp, now);
}

uint32_t pacing_min_rtt(const ikcpcb *kcp)
{
    if (kcp == NULL || kcp->pacing == NULL) {
        return 0;
    }
    return minmax_get(&((const struct pacing *)kcp->pacing)->rtt);
}

uint32_t pacing_bw(const ikcpcb *kcp)
{
    if (kcp == NULL || kcp->pacing == NULL) {
        return 0;
    }
    return minmax_get(&((const struct pacing *)kcp->pacing)->bw);
}

int pacing_try_send(ikcpcb *kcp, uint32_t pkt_len, int is_new)
{
    struct pacing *p;

    if (kcp == NULL || kcp->pacing == NULL) {
        return 1;
    }
    p = (struct pacing *)kcp->pacing;

    /*
     * The ceiling applies to new data only. A retransmission replaces something
     * already counted as outstanding, so charging it again would let a lossy
     * spell starve the very repairs that end it - and kcp->nsnd_buf cannot serve
     * as the measure here either, since it counts segments still waiting for
     * their first transmission alongside those actually in flight.
     */
    if (is_new && p->inflight_cap > 0 && p->inflight >= p->inflight_cap) {
        return 0;
    }

    /* Retransmissions are charged against the rate: they occupy the same queue. */
    if (p->tokens < pkt_len) {
        return 0;
    }
    p->tokens -= pkt_len;
    if (is_new) {
        p->inflight += pkt_len;
    }
    return 1;
}
