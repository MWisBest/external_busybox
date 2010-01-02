/*
 * NTP client/server, based on OpenNTPD 3.9p1
 *
 * Author: Adam Tkac <vonsch@gmail.com>
 *
 * Licensed under GPLv2, see file LICENSE in this tarball for details.
 *
 * Parts of OpenNTPD clock syncronization code is replaced by
 * code which is based on ntp-4.2.6, whuch carries the following
 * copyright notice:
 *
 ***********************************************************************
 *                                                                     *
 * Copyright (c) University of Delaware 1992-2009                      *
 *                                                                     *
 * Permission to use, copy, modify, and distribute this software and   *
 * its documentation for any purpose with or without fee is hereby     *
 * granted, provided that the above copyright notice appears in all    *
 * copies and that both the copyright notice and this permission       *
 * notice appear in supporting documentation, and that the name        *
 * University of Delaware not be used in advertising or publicity      *
 * pertaining to distribution of the software without specific,        *
 * written prior permission. The University of Delaware makes no       *
 * representations about the suitability this software for any         *
 * purpose. It is provided "as is" without express or implied          *
 * warranty.                                                           *
 *                                                                     *
 ***********************************************************************
 */
#include "libbb.h"
#include <math.h>
#include <netinet/ip.h> /* For IPTOS_LOWDELAY definition */
#include <sys/timex.h>
#ifndef IPTOS_LOWDELAY
# define IPTOS_LOWDELAY 0x10
#endif
#ifndef IP_PKTINFO
# error "Sorry, your kernel has to support IP_PKTINFO"
#endif


/* Verbosity control (max level of -dddd options accepted).
 * max 5 is very talkative (and bloated). 2 is non-bloated,
 * production level setting.
 */
#define MAX_VERBOSE     2


#define RETRY_INTERVAL  5       /* on error, retry in N secs */
#define QUERYTIME_MAX   15      /* wait for reply up to N secs */

#define FREQ_TOLERANCE  0.000015 /* % frequency tolerance (15 PPM) */
#define MINPOLL         4       /* % minimum poll interval (6: 64 s) */
#define MAXPOLL         12      /* % maximum poll interval (12: 1.1h, 17: 36.4h) (was 17) */
#define MINDISP         0.01    /* % minimum dispersion (s) */
#define MAXDISP         16      /* maximum dispersion (s) */
#define MAXSTRAT        16      /* maximum stratum (infinity metric) */
#define MAXDIST         1       /* % distance threshold (s) */
#define MIN_SELECTED    1       /* % minimum intersection survivors */
#define MIN_CLUSTERED   3       /* % minimum cluster survivors */

#define MAXDRIFT        0.000500 /* frequency drift we can correct (500 PPM) */

/* Clock discipline parameters and constants */
#define STEP_THRESHOLD  0.128   /* step threshold (s) */
#define WATCH_THRESHOLD 150     /* stepout threshold (s). std ntpd uses 900 (11 mins (!)) */
/* NB: set WATCH_THRESHOLD to ~60 when debugging to save time) */
#define PANIC_THRESHOLD 1000    /* panic threshold (s) */

/* Poll-adjust threshold.
 * When we see that offset is small enough compared to discipline jitter,
 * we grow a counter: += MINPOLL. When it goes over POLLADJ_LIMIT,
 * we poll_exp++. If offset isn't small, counter -= poll_exp*2,
 * and when it goes below -POLLADJ_LIMIT, we poll_exp--
 */
#define POLLADJ_LIMIT   30
/* If offset < POLLADJ_GATE * discipline_jitter, then we can increase
 * poll interval (we think we can't improve timekeeping
 * by staying at smaller poll).
 */
#define POLLADJ_GATE    4
/* Compromise Allan intercept (s). doc uses 1500, std ntpd uses 512 */
#define ALLAN           512
/* PLL loop gain */
#define PLL             65536
/* FLL loop gain [why it depends on MAXPOLL??] */
#define FLL             (MAXPOLL + 1)
/* Parameter averaging constant */
#define AVG             4


enum {
	NTP_VERSION     = 4,
	NTP_MAXSTRATUM  = 15,

	NTP_DIGESTSIZE     = 16,
	NTP_MSGSIZE_NOAUTH = 48,
	NTP_MSGSIZE        = (NTP_MSGSIZE_NOAUTH + 4 + NTP_DIGESTSIZE),

	/* Status Masks */
	MODE_MASK       = (7 << 0),
	VERSION_MASK    = (7 << 3),
	VERSION_SHIFT   = 3,
	LI_MASK         = (3 << 6),

	/* Leap Second Codes (high order two bits of m_status) */
	LI_NOWARNING    = (0 << 6),    /* no warning */
	LI_PLUSSEC      = (1 << 6),    /* add a second (61 seconds) */
	LI_MINUSSEC     = (2 << 6),    /* minus a second (59 seconds) */
	LI_ALARM        = (3 << 6),    /* alarm condition */

	/* Mode values */
	MODE_RES0       = 0,    /* reserved */
	MODE_SYM_ACT    = 1,    /* symmetric active */
	MODE_SYM_PAS    = 2,    /* symmetric passive */
	MODE_CLIENT     = 3,    /* client */
	MODE_SERVER     = 4,    /* server */
	MODE_BROADCAST  = 5,    /* broadcast */
	MODE_RES1       = 6,    /* reserved for NTP control message */
	MODE_RES2       = 7,    /* reserved for private use */
};

//TODO: better base selection
#define OFFSET_1900_1970 2208988800UL  /* 1970 - 1900 in seconds */

#define NUM_DATAPOINTS  8

typedef struct {
	uint32_t int_partl;
	uint32_t fractionl;
} l_fixedpt_t;

typedef struct {
	uint16_t int_parts;
	uint16_t fractions;
} s_fixedpt_t;

typedef struct {
	uint8_t     m_status;     /* status of local clock and leap info */
	uint8_t     m_stratum;
	uint8_t     m_ppoll;      /* poll value */
	int8_t      m_precision_exp;
	s_fixedpt_t m_rootdelay;
	s_fixedpt_t m_rootdisp;
	uint32_t    m_refid;
	l_fixedpt_t m_reftime;
	l_fixedpt_t m_orgtime;
	l_fixedpt_t m_rectime;
	l_fixedpt_t m_xmttime;
	uint32_t    m_keyid;
	uint8_t     m_digest[NTP_DIGESTSIZE];
} msg_t;

typedef struct {
	double d_recv_time;
	double d_offset;
	double d_dispersion;
} datapoint_t;

typedef struct {
	len_and_sockaddr *p_lsa;
	char             *p_dotted;
	/* when to send new query (if p_fd == -1)
	 * or when receive times out (if p_fd >= 0): */
	time_t           next_action_time;
	int              p_fd;
	int              datapoint_idx;
	uint32_t         lastpkt_refid;
	uint8_t          lastpkt_status;
	uint8_t          lastpkt_stratum;
	uint8_t          p_reachable_bits;
	double           p_xmttime;
	double           lastpkt_recv_time;
	double           lastpkt_delay;
	double           lastpkt_rootdelay;
	double           lastpkt_rootdisp;
	/* produced by filter algorithm: */
	double           filter_offset;
	double           filter_dispersion;
	double           filter_jitter;
	datapoint_t      filter_datapoint[NUM_DATAPOINTS];
	/* last sent packet: */
	msg_t            p_xmt_msg;
} peer_t;


enum {
	OPT_n = (1 << 0),
	OPT_q = (1 << 1),
	OPT_N = (1 << 2),
	OPT_x = (1 << 3),
	/* Insert new options above this line. */
	/* Non-compat options: */
	OPT_p = (1 << 4),
	OPT_l = (1 << 5) * ENABLE_FEATURE_NTPD_SERVER,
};

struct globals {
	/* total round trip delay to currently selected reference clock */
	double   rootdelay;
	/* reference timestamp: time when the system clock was last set or corrected */
	double   reftime;
	/* total dispersion to currently selected reference clock */
	double   rootdisp;
	llist_t  *ntp_peers;
#if ENABLE_FEATURE_NTPD_SERVER
	int      listen_fd;
#endif
	unsigned verbose;
	unsigned peer_cnt;
	/* refid: 32-bit code identifying the particular server or reference clock
	 *  in stratum 0 packets this is a four-character ASCII string,
	 *  called the kiss code, used for debugging and monitoring
	 *  in stratum 1 packets this is a four-character ASCII string
	 *  assigned to the reference clock by IANA. Example: "GPS "
	 *  in stratum 2+ packets, it's IPv4 address or 4 first bytes of MD5 hash of IPv6
	 */
	uint32_t refid;
	uint8_t  ntp_status;
	/* precision is defined as the larger of the resolution and time to
	 * read the clock, in log2 units.  For instance, the precision of a
	 * mains-frequency clock incrementing at 60 Hz is 16 ms, even when the
	 * system clock hardware representation is to the nanosecond.
	 *
	 * Delays, jitters of various kinds are clamper down to precision.
	 *
	 * If precision_sec is too large, discipline_jitter gets clamped to it
	 * and if offset is much smaller than discipline_jitter, poll interval
	 * grows even though we really can benefit from staying at smaller one,
	 * collecting non-lagged datapoits and correcting the offset.
	 * (Lagged datapoits exist when poll_exp is large but we still have
	 * systematic offset error - the time distance between datapoints
	 * is significat and older datapoints have smaller offsets.
	 * This makes our offset estimation a bit smaller than reality)
	 * Due to this effect, setting G_precision_sec close to
	 * STEP_THRESHOLD isn't such a good idea - offsets may grow
	 * too big and we will step. I observed it with -6.
	 *
	 * OTOH, setting precision too small would result in futile attempts
	 * to syncronize to the unachievable precision.
	 *
	 * -6 is 1/64 sec, -7 is 1/128 sec and so on.
	 */
#define G_precision_exp  -8
#define G_precision_sec  (1.0 / (1 << (- G_precision_exp)))
	uint8_t  stratum;
	/* Bool. After set to 1, never goes back to 0: */
	uint8_t  adjtimex_was_done;

	uint8_t  discipline_state;      // doc calls it c.state
	uint8_t  poll_exp;              // s.poll
	int      polladj_count;         // c.count
	long     kernel_freq_drift;
	double   last_update_offset;    // c.last
	double   last_update_recv_time; // s.t
	double   discipline_jitter;     // c.jitter
//TODO: add s.jitter - grep for it here and see clock_combine() in doc
#define USING_KERNEL_PLL_LOOP 1
#if !USING_KERNEL_PLL_LOOP
	double   discipline_freq_drift; // c.freq
//TODO: conditionally calculate wander? it's used only for logging
	double   discipline_wander;     // c.wander
#endif
};
#define G (*ptr_to_globals)

static const int const_IPTOS_LOWDELAY = IPTOS_LOWDELAY;


#define VERB1 if (MAX_VERBOSE && G.verbose)
#define VERB2 if (MAX_VERBOSE >= 2 && G.verbose >= 2)
#define VERB3 if (MAX_VERBOSE >= 3 && G.verbose >= 3)
#define VERB4 if (MAX_VERBOSE >= 4 && G.verbose >= 4)
#define VERB5 if (MAX_VERBOSE >= 5 && G.verbose >= 5)


static double LOG2D(int a)
{
	if (a < 0)
		return 1.0 / (1UL << -a);
	return 1UL << a;
}
static ALWAYS_INLINE double SQUARE(double x)
{
	return x * x;
}
static ALWAYS_INLINE double MAXD(double a, double b)
{
	if (a > b)
		return a;
	return b;
}
static ALWAYS_INLINE double MIND(double a, double b)
{
	if (a < b)
		return a;
	return b;
}
#define SQRT(x) (sqrt(x))

static double
gettime1900d(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL); /* never fails */
	return (tv.tv_sec + 1.0e-6 * tv.tv_usec + OFFSET_1900_1970);
}

static void
d_to_tv(double d, struct timeval *tv)
{
	tv->tv_sec = (long)d;
	tv->tv_usec = (d - tv->tv_sec) * 1000000;
}

static double
lfp_to_d(l_fixedpt_t lfp)
{
	double ret;
	lfp.int_partl = ntohl(lfp.int_partl);
	lfp.fractionl = ntohl(lfp.fractionl);
	ret = (double)lfp.int_partl + ((double)lfp.fractionl / UINT_MAX);
	return ret;
}
static double
sfp_to_d(s_fixedpt_t sfp)
{
	double ret;
	sfp.int_parts = ntohs(sfp.int_parts);
	sfp.fractions = ntohs(sfp.fractions);
	ret = (double)sfp.int_parts + ((double)sfp.fractions / USHRT_MAX);
	return ret;
}
#if ENABLE_FEATURE_NTPD_SERVER
static l_fixedpt_t
d_to_lfp(double d)
{
	l_fixedpt_t lfp;
	lfp.int_partl = (uint32_t)d;
	lfp.fractionl = (uint32_t)((d - lfp.int_partl) * UINT_MAX);
	lfp.int_partl = htonl(lfp.int_partl);
	lfp.fractionl = htonl(lfp.fractionl);
	return lfp;
}
static s_fixedpt_t
d_to_sfp(double d)
{
	s_fixedpt_t sfp;
	sfp.int_parts = (uint16_t)d;
	sfp.fractions = (uint16_t)((d - sfp.int_parts) * USHRT_MAX);
	sfp.int_parts = htons(sfp.int_parts);
	sfp.fractions = htons(sfp.fractions);
	return sfp;
}
#endif

static double
dispersion(const datapoint_t *dp, double t)
{
	return dp->d_dispersion + FREQ_TOLERANCE * (t - dp->d_recv_time);
}

static double
root_distance(peer_t *p, double t)
{
	/* The root synchronization distance is the maximum error due to
	 * all causes of the local clock relative to the primary server.
	 * It is defined as half the total delay plus total dispersion
	 * plus peer jitter.
	 */
	return MAXD(MINDISP, p->lastpkt_rootdelay + p->lastpkt_delay) / 2
		+ p->lastpkt_rootdisp
		+ p->filter_dispersion
		+ FREQ_TOLERANCE * (t - p->lastpkt_recv_time)
		+ p->filter_jitter;
}

static void
set_next(peer_t *p, unsigned t)
{
	p->next_action_time = time(NULL) + t;
}

/*
 * Peer clock filter and its helpers
 */
static void
filter_datapoints(peer_t *p, double t)
{
	int i, idx;
	int got_newest;
	double minoff, maxoff, wavg, sum, w;
	double x = x; /* for compiler */
	double oldest_off = oldest_off;
	double oldest_age = oldest_age;
	double newest_off = newest_off;
	double newest_age = newest_age;

	minoff = maxoff = p->filter_datapoint[0].d_offset;
	for (i = 1; i < NUM_DATAPOINTS; i++) {
		if (minoff > p->filter_datapoint[i].d_offset)
			minoff = p->filter_datapoint[i].d_offset;
		if (maxoff < p->filter_datapoint[i].d_offset)
			maxoff = p->filter_datapoint[i].d_offset;
	}

	idx = p->datapoint_idx; /* most recent datapoint */
	/* Average offset:
	 * Drop two outliers and take weighted average of the rest:
	 * most_recent/2 + older1/4 + older2/8 ... + older5/32 + older6/32
	 * we use older6/32, not older6/64 since sum of weights should be 1:
	 * 1/2 + 1/4 + 1/8 + 1/16 + 1/32 + 1/32 = 1
	 */
	wavg = 0;
	w = 0.5;
	/*                     n-1
	 *                     ---    dispersion(i)
	 * filter_dispersion =  \     -------------
	 *                      /       (i+1)
	 *                     ---     2
	 *                     i=0
	 */
	got_newest = 0;
	sum = 0;
	for (i = 0; i < NUM_DATAPOINTS; i++) {
		VERB4 {
			bb_error_msg("datapoint[%d]: off:%f disp:%f(%f) age:%f%s",
				i,
				p->filter_datapoint[idx].d_offset,
				p->filter_datapoint[idx].d_dispersion, dispersion(&p->filter_datapoint[idx], t),
				t - p->filter_datapoint[idx].d_recv_time,
				(minoff == p->filter_datapoint[idx].d_offset || maxoff == p->filter_datapoint[idx].d_offset)
					? " (outlier by offset)" : ""
			);
		}

		sum += dispersion(&p->filter_datapoint[idx], t) / (2 << i);

		if (minoff == p->filter_datapoint[idx].d_offset) {
			minoff -= 1; /* so that we don't match it ever again */
		} else
		if (maxoff == p->filter_datapoint[idx].d_offset) {
			maxoff += 1;
		} else {
			oldest_off = p->filter_datapoint[idx].d_offset;
			oldest_age = t - p->filter_datapoint[idx].d_recv_time;
			if (!got_newest) {
				got_newest = 1;
				newest_off = oldest_off;
				newest_age = oldest_age;
			}
			x = oldest_off * w;
			wavg += x;
			w /= 2;
		}

		idx = (idx - 1) & (NUM_DATAPOINTS - 1);
	}
	p->filter_dispersion = sum;
	wavg += x; /* add another older6/64 to form older6/32 */
	/* Fix systematic underestimation with large poll intervals.
	 * Imagine that we still have a bit of uncorrected drift,
	 * and poll interval is big (say, 100 sec). Offsets form a progression:
	 * 0.0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 - 0.7 is most recent.
	 * The algorithm above drops 0.0 and 0.7 as outliers,
	 * and then we have this estimation, ~25% off from 0.7:
	 * 0.1/32 + 0.2/32 + 0.3/16 + 0.4/8 + 0.5/4 + 0.6/2 = 0.503125
	 */
	x = newest_age / (oldest_age - newest_age); /* in above example, 100 / (600 - 100) */
	if (x < 1) {
		x = (newest_off - oldest_off) * x; /* 0.5 * 100/500 = 0.1 */
		wavg += x;
	}
	p->filter_offset = wavg;

	/*                  +-----                 -----+ ^ 1/2
	 *                  |       n-1                 |
	 *                  |       ---                 |
	 *                  |  1    \                2  |
	 * filter_jitter =  | --- * /  (avg-offset_j)   |
	 *                  |  n    ---                 |
	 *                  |       j=0                 |
	 *                  +-----                 -----+
	 * where n is the number of valid datapoints in the filter (n > 1);
	 * if filter_jitter < precision then filter_jitter = precision
	 */
	sum = 0;
	for (i = 0; i < NUM_DATAPOINTS; i++) {
		sum += SQUARE(wavg - p->filter_datapoint[i].d_offset);
	}
	sum = SQRT(sum / NUM_DATAPOINTS);
	p->filter_jitter = sum > G_precision_sec ? sum : G_precision_sec;

	VERB3 bb_error_msg("filter offset:%f(corr:%e) disp:%f jitter:%f",
			p->filter_offset, x,
			p->filter_dispersion,
			p->filter_jitter);

}

static void
reset_peer_stats(peer_t *p, double t, double offset)
{
	int i;
	for (i = 0; i < NUM_DATAPOINTS; i++) {
		if (offset < 16 * STEP_THRESHOLD) {
			p->filter_datapoint[i].d_recv_time -= offset;
			if (p->filter_datapoint[i].d_offset != 0) {
				p->filter_datapoint[i].d_offset -= offset;
			}
		} else {
			p->filter_datapoint[i].d_recv_time  = t;
			p->filter_datapoint[i].d_offset     = 0;
			p->filter_datapoint[i].d_dispersion = MAXDISP;
		}
	}
	if (offset < 16 * STEP_THRESHOLD) {
		p->lastpkt_recv_time -= offset;
	} else {
		p->p_reachable_bits = 0;
		p->lastpkt_recv_time = t;
	}
	filter_datapoints(p, t); /* recalc p->filter_xxx */
	p->next_action_time -= (time_t)offset;
	VERB5 bb_error_msg("%s->lastpkt_recv_time=%f", p->p_dotted, p->lastpkt_recv_time);
}

static void
add_peers(char *s)
{
	peer_t *p;

	p = xzalloc(sizeof(*p));
	p->p_lsa = xhost2sockaddr(s, 123);
	p->p_dotted = xmalloc_sockaddr2dotted_noport(&p->p_lsa->u.sa);
	p->p_fd = -1;
	p->p_xmt_msg.m_status = MODE_CLIENT | (NTP_VERSION << 3);
	p->next_action_time = time(NULL); /* = set_next(p, 0); */
	reset_peer_stats(p, gettime1900d(), 16 * STEP_THRESHOLD);
	/* Speed up initial sync: with small offsets from peers,
	 * 3 samples will sync
	 */
	p->filter_datapoint[6].d_dispersion = 0;
	p->filter_datapoint[7].d_dispersion = 0;

	llist_add_to(&G.ntp_peers, p);
	G.peer_cnt++;
}

static int
do_sendto(int fd,
		const struct sockaddr *from, const struct sockaddr *to, socklen_t addrlen,
		msg_t *msg, ssize_t len)
{
	ssize_t ret;

	errno = 0;
	if (!from) {
		ret = sendto(fd, msg, len, MSG_DONTWAIT, to, addrlen);
	} else {
		ret = send_to_from(fd, msg, len, MSG_DONTWAIT, to, from, addrlen);
	}
	if (ret != len) {
		bb_perror_msg("send failed");
		return -1;
	}
	return 0;
}

static int
send_query_to_peer(peer_t *p)
{
	/* Why do we need to bind()?
	 * See what happens when we don't bind:
	 *
	 * socket(PF_INET, SOCK_DGRAM, IPPROTO_IP) = 3
	 * setsockopt(3, SOL_IP, IP_TOS, [16], 4) = 0
	 * gettimeofday({1259071266, 327885}, NULL) = 0
	 * sendto(3, "xxx", 48, MSG_DONTWAIT, {sa_family=AF_INET, sin_port=htons(123), sin_addr=inet_addr("10.34.32.125")}, 16) = 48
	 * ^^^ we sent it from some source port picked by kernel.
	 * time(NULL)              = 1259071266
	 * write(2, "ntpd: entering poll 15 secs\n", 28) = 28
	 * poll([{fd=3, events=POLLIN}], 1, 15000) = 1 ([{fd=3, revents=POLLIN}])
	 * recv(3, "yyy", 68, MSG_DONTWAIT) = 48
	 * ^^^ this recv will receive packets to any local port!
	 *
	 * Uncomment this and use strace to see it in action:
	 */
#define PROBE_LOCAL_ADDR /* { len_and_sockaddr lsa; lsa.len = LSA_SIZEOF_SA; getsockname(p->query.fd, &lsa.u.sa, &lsa.len); } */

	if (p->p_fd == -1) {
		int fd, family;
		len_and_sockaddr *local_lsa;

		family = p->p_lsa->u.sa.sa_family;
		p->p_fd = fd = xsocket_type(&local_lsa, family, SOCK_DGRAM);
		/* local_lsa has "null" address and port 0 now.
		 * bind() ensures we have a *particular port* selected by kernel
		 * and remembered in p->p_fd, thus later recv(p->p_fd)
		 * receives only packets sent to this port.
		 */
		PROBE_LOCAL_ADDR
		xbind(fd, &local_lsa->u.sa, local_lsa->len);
		PROBE_LOCAL_ADDR
#if ENABLE_FEATURE_IPV6
		if (family == AF_INET)
#endif
			setsockopt(fd, IPPROTO_IP, IP_TOS, &const_IPTOS_LOWDELAY, sizeof(const_IPTOS_LOWDELAY));
		free(local_lsa);
	}

	/*
	 * Send out a random 64-bit number as our transmit time.  The NTP
	 * server will copy said number into the originate field on the
	 * response that it sends us.  This is totally legal per the SNTP spec.
	 *
	 * The impact of this is two fold: we no longer send out the current
	 * system time for the world to see (which may aid an attacker), and
	 * it gives us a (not very secure) way of knowing that we're not
	 * getting spoofed by an attacker that can't capture our traffic
	 * but can spoof packets from the NTP server we're communicating with.
	 *
	 * Save the real transmit timestamp locally.
	 */
	p->p_xmt_msg.m_xmttime.int_partl = random();
	p->p_xmt_msg.m_xmttime.fractionl = random();
	p->p_xmttime = gettime1900d();

	if (do_sendto(p->p_fd, /*from:*/ NULL, /*to:*/ &p->p_lsa->u.sa, /*addrlen:*/ p->p_lsa->len,
			&p->p_xmt_msg, NTP_MSGSIZE_NOAUTH) == -1
	) {
		close(p->p_fd);
		p->p_fd = -1;
		set_next(p, RETRY_INTERVAL);
		return -1;
	}

	p->p_reachable_bits <<= 1;
	VERB1 bb_error_msg("sent query to %s", p->p_dotted);
	set_next(p, QUERYTIME_MAX);

	return 0;
}


static void
step_time(double offset)
{
	double dtime;
	struct timeval tv;
	char buf[80];
	time_t tval;

	gettimeofday(&tv, NULL); /* never fails */
	dtime = offset + tv.tv_sec;
	dtime += 1.0e-6 * tv.tv_usec;
	d_to_tv(dtime, &tv);

	if (settimeofday(&tv, NULL) == -1)
		bb_perror_msg_and_die("settimeofday");

	tval = tv.tv_sec;
	strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", localtime(&tval));

	bb_error_msg("setting clock to %s (offset %fs)", buf, offset);
}


/*
 * Selection and clustering, and their helpers
 */
typedef struct {
	peer_t *p;
	int    type;
	double edge;
} point_t;
static int
compare_point_edge(const void *aa, const void *bb)
{
	const point_t *a = aa;
	const point_t *b = bb;
	if (a->edge < b->edge) {
		return -1;
	}
	return (a->edge > b->edge);
}
typedef struct {
	peer_t *p;
	double metric;
} survivor_t;
static int
compare_survivor_metric(const void *aa, const void *bb)
{
	const survivor_t *a = aa;
	const survivor_t *b = bb;
	if (a->metric < b->metric)
		return -1;
	return (a->metric > b->metric);
}
static int
fit(peer_t *p, double rd)
{
	if (p->p_reachable_bits == 0) {
		VERB3 bb_error_msg("peer %s unfit for selection: unreachable", p->p_dotted);
		return 0;
	}
#if 0	/* we filter out such packets earlier */
	if ((p->lastpkt_status & LI_ALARM) == LI_ALARM
	 || p->lastpkt_stratum >= MAXSTRAT
	) {
		VERB3 bb_error_msg("peer %s unfit for selection: bad status/stratum", p->p_dotted);
		return 0;
	}
#endif
	/* rd is root_distance(p, t) */
	if (rd > MAXDIST + FREQ_TOLERANCE * (1 << G.poll_exp)) {
		VERB3 bb_error_msg("peer %s unfit for selection: root distance too high", p->p_dotted);
		return 0;
	}
//TODO
//	/* Do we have a loop? */
//	if (p->refid == p->dstaddr || p->refid == s.refid)
//		return 0;
        return 1;
}
static peer_t*
select_and_cluster(double t)
{
	llist_t    *item;
	int        i, j;
	int        size = 3 * G.peer_cnt;
	/* for selection algorithm */
	point_t    point[size];
	unsigned   num_points, num_candidates;
	double     low, high;
	unsigned   num_falsetickers;
	/* for cluster algorithm */
	survivor_t survivor[size];
	unsigned   num_survivors;

	/* Selection */

	num_points = 0;
	item = G.ntp_peers;
	while (item != NULL) {
		peer_t *p = (peer_t *) item->data;
		double rd = root_distance(p, t);
		double offset = p->filter_offset;

		if (!fit(p, rd)) {
			item = item->link;
			continue;
		}

		VERB4 bb_error_msg("interval: [%f %f %f] %s",
				offset - rd,
				offset,
				offset + rd,
				p->p_dotted
		);
		point[num_points].p = p;
		point[num_points].type = -1;
		point[num_points].edge = offset - rd;
		num_points++;
		point[num_points].p = p;
		point[num_points].type = 0;
		point[num_points].edge = offset;
		num_points++;
		point[num_points].p = p;
		point[num_points].type = 1;
		point[num_points].edge = offset + rd;
		num_points++;
		item = item->link;
	}
	num_candidates = num_points / 3;
	if (num_candidates == 0) {
		VERB3 bb_error_msg("no valid datapoints, no peer selected");
		return NULL; /* never happers? */
	}
//TODO: sorting does not seem to be done in reference code
	qsort(point, num_points, sizeof(point[0]), compare_point_edge);

	/* Start with the assumption that there are no falsetickers.
	 * Attempt to find a nonempty intersection interval containing
	 * the midpoints of all truechimers.
	 * If a nonempty interval cannot be found, increase the number
	 * of assumed falsetickers by one and try again.
	 * If a nonempty interval is found and the number of falsetickers
	 * is less than the number of truechimers, a majority has been found
	 * and the midpoint of each truechimer represents
	 * the candidates available to the cluster algorithm.
	 */
	num_falsetickers = 0;
	while (1) {
		int c;
		unsigned num_midpoints = 0;

		low = 1 << 9;
		high = - (1 << 9);
		c = 0;
		for (i = 0; i < num_points; i++) {
			/* We want to do:
			 * if (point[i].type == -1) c++;
			 * if (point[i].type == 1) c--;
			 * and it's simpler to do it this way:
			 */
			c -= point[i].type;
			if (c >= num_candidates - num_falsetickers) {
				/* If it was c++ and it got big enough... */
				low = point[i].edge;
				break;
			}
			if (point[i].type == 0)
				num_midpoints++;
		}
		c = 0;
		for (i = num_points-1; i >= 0; i--) {
			c += point[i].type;
			if (c >= num_candidates - num_falsetickers) {
				high = point[i].edge;
				break;
			}
			if (point[i].type == 0)
				num_midpoints++;
		}
		/* If the number of midpoints is greater than the number
		 * of allowed falsetickers, the intersection contains at
		 * least one truechimer with no midpoint - bad.
		 * Also, interval should be nonempty.
		 */
		if (num_midpoints <= num_falsetickers && low < high)
			break;
		num_falsetickers++;
		if (num_falsetickers * 2 >= num_candidates) {
			VERB3 bb_error_msg("too many falsetickers:%d (candidates:%d), no peer selected",
					num_falsetickers, num_candidates);
			return NULL;
		}
	}
	VERB3 bb_error_msg("selected interval: [%f, %f]; candidates:%d falsetickers:%d",
			low, high, num_candidates, num_falsetickers);

	/* Clustering */

	/* Construct a list of survivors (p, metric)
	 * from the chime list, where metric is dominated
	 * first by stratum and then by root distance.
	 * All other things being equal, this is the order of preference.
	 */
	num_survivors = 0;
	for (i = 0; i < num_points; i++) {
		peer_t *p;

		if (point[i].edge < low || point[i].edge > high)
			continue;
		p = point[i].p;
		survivor[num_survivors].p = p;
//TODO: save root_distance in point_t and reuse here?
		survivor[num_survivors].metric = MAXDIST * p->lastpkt_stratum + root_distance(p, t);
		VERB4 bb_error_msg("survivor[%d] metric:%f peer:%s",
			num_survivors, survivor[num_survivors].metric, p->p_dotted);
		num_survivors++;
	}
	/* There must be at least MIN_SELECTED survivors to satisfy the
	 * correctness assertions. Ordinarily, the Byzantine criteria
	 * require four survivors, but for the demonstration here, one
	 * is acceptable.
	 */
	if (num_survivors < MIN_SELECTED) {
		VERB3 bb_error_msg("num_survivors %d < %d, no peer selected",
				num_survivors, MIN_SELECTED);
		return NULL;
	}

//looks like this is ONLY used by the fact that later we pick survivor[0].
//we can avoid sorting then, just find the minimum once!
	qsort(survivor, num_survivors, sizeof(survivor[0]), compare_survivor_metric);

	/* For each association p in turn, calculate the selection
	 * jitter p->sjitter as the square root of the sum of squares
	 * (p->offset - q->offset) over all q associations. The idea is
	 * to repeatedly discard the survivor with maximum selection
	 * jitter until a termination condition is met.
	 */
	while (1) {
		unsigned max_idx = max_idx;
		double max_selection_jitter = max_selection_jitter;
		double min_jitter = min_jitter;

		if (num_survivors <= MIN_CLUSTERED) {
			bb_error_msg("num_survivors %d <= %d, not discarding more",
					num_survivors, MIN_CLUSTERED);
			break;
		}

		/* To make sure a few survivors are left
		 * for the clustering algorithm to chew on,
		 * we stop if the number of survivors
		 * is less than or equal to MIN_CLUSTERED (3).
		 */
		for (i = 0; i < num_survivors; i++) {
			double selection_jitter_sq;
			peer_t *p = survivor[i].p;

			if (i == 0 || p->filter_jitter < min_jitter)
				min_jitter = p->filter_jitter;

			selection_jitter_sq = 0;
			for (j = 0; j < num_survivors; j++) {
				peer_t *q = survivor[j].p;
				selection_jitter_sq += SQUARE(p->filter_offset - q->filter_offset);
			}
			if (i == 0 || selection_jitter_sq > max_selection_jitter) {
				max_selection_jitter = selection_jitter_sq;
				max_idx = i;
			}
			VERB5 bb_error_msg("survivor %d selection_jitter^2:%f",
					i, selection_jitter_sq);
		}
		max_selection_jitter = SQRT(max_selection_jitter / num_survivors);
		VERB4 bb_error_msg("max_selection_jitter (at %d):%f min_jitter:%f",
				max_idx, max_selection_jitter, min_jitter);

		/* If the maximum selection jitter is less than the
		 * minimum peer jitter, then tossing out more survivors
		 * will not lower the minimum peer jitter, so we might
		 * as well stop.
		 */
		if (max_selection_jitter < min_jitter) {
			VERB3 bb_error_msg("max_selection_jitter:%f < min_jitter:%f, num_survivors:%d, not discarding more",
					max_selection_jitter, min_jitter, num_survivors);
			break;
		}

		/* Delete survivor[max_idx] from the list
		 * and go around again.
		 */
		VERB5 bb_error_msg("dropping survivor %d", max_idx);
		num_survivors--;
		while (max_idx < num_survivors) {
			survivor[max_idx] = survivor[max_idx + 1];
			max_idx++;
		}
	}

	/* Pick the best clock. If the old system peer is on the list
	 * and at the same stratum as the first survivor on the list,
	 * then don't do a clock hop. Otherwise, select the first
	 * survivor on the list as the new system peer.
	 */
//TODO - see clock_combine()
	VERB3 bb_error_msg("selected peer %s filter_offset:%f age:%f",
			survivor[0].p->p_dotted,
			survivor[0].p->filter_offset,
			t - survivor[0].p->lastpkt_recv_time
	);
	return survivor[0].p;
}


/*
 * Local clock discipline and its helpers
 */
static void
set_new_values(int disc_state, double offset, double recv_time)
{
	/* Enter new state and set state variables. Note we use the time
	 * of the last clock filter sample, which must be earlier than
	 * the current time.
	 */
	VERB3 bb_error_msg("disc_state=%d last update offset=%f recv_time=%f",
			disc_state, offset, recv_time);
	G.discipline_state = disc_state;
	G.last_update_offset = offset;
	G.last_update_recv_time = recv_time;
}
/* Clock state definitions */
#define STATE_NSET      0       /* initial state, "nothing is set" */
#define STATE_FSET      1       /* frequency set from file */
#define STATE_SPIK      2       /* spike detected */
#define STATE_FREQ      3       /* initial frequency */
#define STATE_SYNC      4       /* clock synchronized (normal operation) */
/* Return: -1: decrease poll interval, 0: leave as is, 1: increase */
static int
update_local_clock(peer_t *p, double t)
{
	int rc;
	long old_tmx_offset;
	struct timex tmx;
	double offset = p->filter_offset;
	double recv_time = p->lastpkt_recv_time;
	double abs_offset;
#if !USING_KERNEL_PLL_LOOP
	double freq_drift;
#endif
	double since_last_update;
	double etemp, dtemp;

	abs_offset = fabs(offset);

	/* If the offset is too large, give up and go home */
	if (abs_offset > PANIC_THRESHOLD) {
		bb_error_msg_and_die("offset %f far too big, exiting", offset);
	}

	/* If this is an old update, for instance as the result
	 * of a system peer change, avoid it. We never use
	 * an old sample or the same sample twice.
	 */
	if (recv_time <= G.last_update_recv_time) {
		VERB3 bb_error_msg("same or older datapoint: %f >= %f, not using it",
				G.last_update_recv_time, recv_time);
		return 0; /* "leave poll interval as is" */
	}

	/* Clock state machine transition function. This is where the
	 * action is and defines how the system reacts to large time
	 * and frequency errors.
	 */
	since_last_update = recv_time - G.reftime;
#if !USING_KERNEL_PLL_LOOP
	freq_drift = 0;
#endif
	if (G.discipline_state == STATE_FREQ) {
		/* Ignore updates until the stepout threshold */
		if (since_last_update < WATCH_THRESHOLD) {
			VERB3 bb_error_msg("measuring drift, datapoint ignored, %f sec remains",
					WATCH_THRESHOLD - since_last_update);
			return 0; /* "leave poll interval as is" */
		}
#if !USING_KERNEL_PLL_LOOP
		freq_drift = (offset - G.last_update_offset) / since_last_update;
#endif
	}

	/* There are two main regimes: when the
	 * offset exceeds the step threshold and when it does not.
	 */
	if (abs_offset > STEP_THRESHOLD) {
		llist_t *item;

		switch (G.discipline_state) {
		case STATE_SYNC:
			/* The first outlyer: ignore it, switch to SPIK state */
			VERB3 bb_error_msg("offset:%f - spike detected", offset);
			G.discipline_state = STATE_SPIK;
			return -1; /* "decrease poll interval" */

		case STATE_SPIK:
			/* Ignore succeeding outlyers until either an inlyer
			 * is found or the stepout threshold is exceeded.
			 */
			if (since_last_update < WATCH_THRESHOLD) {
				VERB3 bb_error_msg("spike detected, datapoint ignored, %f sec remains",
						WATCH_THRESHOLD - since_last_update);
				return -1; /* "decrease poll interval" */
			}
			/* fall through: we need to step */
		} /* switch */

		/* Step the time and clamp down the poll interval.
		 *
		 * In NSET state an initial frequency correction is
		 * not available, usually because the frequency file has
		 * not yet been written. Since the time is outside the
		 * capture range, the clock is stepped. The frequency
		 * will be set directly following the stepout interval.
		 *
		 * In FSET state the initial frequency has been set
		 * from the frequency file. Since the time is outside
		 * the capture range, the clock is stepped immediately,
		 * rather than after the stepout interval. Guys get
		 * nervous if it takes 17 minutes to set the clock for
		 * the first time.
		 *
		 * In SPIK state the stepout threshold has expired and
		 * the phase is still above the step threshold. Note
		 * that a single spike greater than the step threshold
		 * is always suppressed, even at the longer poll
		 * intervals.
		 */
		VERB3 bb_error_msg("stepping time by %f; poll_exp=MINPOLL", offset);
		step_time(offset);
		if (option_mask32 & OPT_q) {
			/* We were only asked to set time once. Done. */
			exit(0);
		}

		G.polladj_count = 0;
		G.poll_exp = MINPOLL;
		G.stratum = MAXSTRAT;
		for (item = G.ntp_peers; item != NULL; item = item->link) {
			peer_t *pp = (peer_t *) item->data;
			reset_peer_stats(pp, t, offset);
		}
		if (G.discipline_state == STATE_NSET) {
			set_new_values(STATE_FREQ, /*offset:*/ 0, recv_time);
			return 1; /* "ok to increase poll interval" */
		}
		set_new_values(STATE_SYNC, /*offset:*/ 0, recv_time);

	} else { /* abs_offset <= STEP_THRESHOLD */

		if (G.poll_exp < MINPOLL) {
			VERB3 bb_error_msg("saw small offset %f, disabling burst mode", offset);
			G.poll_exp = MINPOLL;
		}

		/* Compute the clock jitter as the RMS of exponentially
		 * weighted offset differences. Used by the poll adjust code.
		 */
		etemp = SQUARE(G.discipline_jitter);
		dtemp = SQUARE(MAXD(fabs(offset - G.last_update_offset), G_precision_sec));
		G.discipline_jitter = SQRT(etemp + (dtemp - etemp) / AVG);
		VERB3 bb_error_msg("discipline jitter=%f", G.discipline_jitter);

		switch (G.discipline_state) {
		case STATE_NSET:
			if (option_mask32 & OPT_q) {
				/* We were only asked to set time once.
				 * The clock is precise enough, no need to step.
				 */
				exit(0);
			}
			/* This is the first update received and the frequency
			 * has not been initialized. The first thing to do
			 * is directly measure the oscillator frequency.
			 */
			set_new_values(STATE_FREQ, offset, recv_time);
			VERB3 bb_error_msg("transitioning to FREQ, datapoint ignored");
			return -1; /* "decrease poll interval" */

#if 0 /* this is dead code for now */
		case STATE_FSET:
			/* This is the first update and the frequency
			 * has been initialized. Adjust the phase, but
			 * don't adjust the frequency until the next update.
			 */
			set_new_values(STATE_SYNC, offset, recv_time);
			/* freq_drift remains 0 */
			break;
#endif

		case STATE_FREQ:
			/* since_last_update >= WATCH_THRESHOLD, we waited enough.
			 * Correct the phase and frequency and switch to SYNC state.
			 * freq_drift was already estimated (see code above)
			 */
			set_new_values(STATE_SYNC, offset, recv_time);
			break;

		default:
#if !USING_KERNEL_PLL_LOOP
			/* Compute freq_drift due to PLL and FLL contributions.
			 *
			 * The FLL and PLL frequency gain constants
			 * depend on the poll interval and Allan
			 * intercept. The FLL is not used below one-half
			 * the Allan intercept. Above that the loop gain
			 * increases in steps to 1 / AVG.
			 */
			if ((1 << G.poll_exp) > ALLAN / 2) {
				etemp = FLL - G.poll_exp;
				if (etemp < AVG)
					etemp = AVG;
				freq_drift += (offset - G.last_update_offset) / (MAXD(since_last_update, ALLAN) * etemp);
			}
			/* For the PLL the integration interval
			 * (numerator) is the minimum of the update
			 * interval and poll interval. This allows
			 * oversampling, but not undersampling.
			 */
			etemp = MIND(since_last_update, (1 << G.poll_exp));
			dtemp = (4 * PLL) << G.poll_exp;
			freq_drift += offset * etemp / SQUARE(dtemp);
#endif
			set_new_values(STATE_SYNC, offset, recv_time);
			break;
		}
		G.stratum = p->lastpkt_stratum + 1;
	}

	G.reftime = t;
	G.ntp_status = p->lastpkt_status;
	G.refid = p->lastpkt_refid;
	G.rootdelay = p->lastpkt_rootdelay + p->lastpkt_delay;
	dtemp = p->filter_jitter; // SQRT(SQUARE(p->filter_jitter) + SQUARE(s.jitter));
	dtemp += MAXD(p->filter_dispersion + FREQ_TOLERANCE * (t - p->lastpkt_recv_time) + abs_offset, MINDISP);
	G.rootdisp = p->lastpkt_rootdisp + dtemp;
	VERB3 bb_error_msg("updating leap/refid/reftime/rootdisp from peer %s", p->p_dotted);

	/* We are in STATE_SYNC now, but did not do adjtimex yet.
	 * (Any other state does not reach this, they all return earlier)
	 * By this time, freq_drift and G.last_update_offset are set
	 * to values suitable for adjtimex.
	 */
#if !USING_KERNEL_PLL_LOOP
	/* Calculate the new frequency drift and frequency stability (wander).
	 * Compute the clock wander as the RMS of exponentially weighted
	 * frequency differences. This is not used directly, but can,
	 * along with the jitter, be a highly useful monitoring and
	 * debugging tool.
	 */
	dtemp = G.discipline_freq_drift + freq_drift;
	G.discipline_freq_drift = MAXD(MIND(MAXDRIFT, dtemp), -MAXDRIFT);
	etemp = SQUARE(G.discipline_wander);
	dtemp = SQUARE(dtemp);
	G.discipline_wander = SQRT(etemp + (dtemp - etemp) / AVG);

	VERB3 bb_error_msg("discipline freq_drift=%.9f(int:%ld corr:%e) wander=%f",
			G.discipline_freq_drift,
			(long)(G.discipline_freq_drift * 65536e6),
			freq_drift,
			G.discipline_wander);
#endif
	VERB3 {
		memset(&tmx, 0, sizeof(tmx));
		if (adjtimex(&tmx) < 0)
			bb_perror_msg_and_die("adjtimex");
		VERB3 bb_error_msg("p adjtimex freq:%ld offset:%ld constant:%ld status:0x%x",
				tmx.freq, tmx.offset, tmx.constant, tmx.status);
	}

	old_tmx_offset = 0;
	if (!G.adjtimex_was_done) {
		G.adjtimex_was_done = 1;
		/* When we use adjtimex for the very first time,
		 * we need to ADD to pre-existing tmx.offset - it may be !0
		 */
		memset(&tmx, 0, sizeof(tmx));
		if (adjtimex(&tmx) < 0)
			bb_perror_msg_and_die("adjtimex");
		old_tmx_offset = tmx.offset;
	}
	memset(&tmx, 0, sizeof(tmx));
#if 0
//doesn't work, offset remains 0 (!) in kernel:
//ntpd:  set adjtimex freq:1786097 tmx.offset:77487
//ntpd: prev adjtimex freq:1786097 tmx.offset:0
//ntpd:  cur adjtimex freq:1786097 tmx.offset:0
	tmx.modes = ADJ_FREQUENCY | ADJ_OFFSET;
	/* 65536 is one ppm */
	tmx.freq = G.discipline_freq_drift * 65536e6;
	tmx.offset = G.last_update_offset * 1000000; /* usec */
#endif
	tmx.modes = ADJ_OFFSET | ADJ_STATUS | ADJ_TIMECONST;// | ADJ_MAXERROR | ADJ_ESTERROR;
	tmx.offset = (G.last_update_offset * 1000000) /* usec */
			/* + (G.last_update_offset < 0 ? -0.5 : 0.5) - too small to bother */
			+ old_tmx_offset; /* almost always 0 */
	tmx.status = STA_PLL;
	if (G.ntp_status & LI_PLUSSEC)
		tmx.status |= STA_INS;
	if (G.ntp_status & LI_MINUSSEC)
		tmx.status |= STA_DEL;
	tmx.constant = G.poll_exp - 4;
	//tmx.esterror = (u_int32)(clock_jitter * 1e6);
	//tmx.maxerror = (u_int32)((sys_rootdelay / 2 + sys_rootdisp) * 1e6);
	rc = adjtimex(&tmx);
	if (rc < 0)
		bb_perror_msg_and_die("adjtimex");
	/* NB: here kernel returns constant == G.poll_exp, not == G.poll_exp - 4.
	 * Not sure why. Perhaps it is normal.
	 */
	VERB3 bb_error_msg("adjtimex:%d freq:%ld offset:%ld constant:%ld status:0x%x",
				rc, tmx.freq, tmx.offset, tmx.constant, tmx.status);
#if 0
	VERB3 {
		/* always gives the same output as above msg */
		memset(&tmx, 0, sizeof(tmx));
		if (adjtimex(&tmx) < 0)
			bb_perror_msg_and_die("adjtimex");
		VERB3 bb_error_msg("c adjtimex freq:%ld offset:%ld constant:%ld status:0x%x",
				tmx.freq, tmx.offset, tmx.constant, tmx.status);
	}
#endif
	if (G.kernel_freq_drift != tmx.freq / 65536) {
		G.kernel_freq_drift = tmx.freq / 65536;
		VERB2 bb_error_msg("kernel clock drift: %ld ppm", G.kernel_freq_drift);
	}
// #define STA_MODE 0x4000  /* mode (0 = PLL, 1 = FLL) (ro) */ - ?
// it appeared after a while:
//ntpd: p adjtimex freq:-14545653 offset:-5396 constant:10 status:0x41
//ntpd: c adjtimex freq:-14547835 offset:-8307 constant:10 status:0x1
//ntpd: p adjtimex freq:-14547835 offset:-6398 constant:10 status:0x41
//ntpd: c adjtimex freq:-14550486 offset:-10158 constant:10 status:0x1
//ntpd: p adjtimex freq:-14550486 offset:-6132 constant:10 status:0x41
//ntpd: c adjtimex freq:-14636129 offset:-10158 constant:10 status:0x4001
//ntpd: p adjtimex freq:-14636129 offset:-10002 constant:10 status:0x4041
//ntpd: c adjtimex freq:-14636245 offset:-7497 constant:10 status:0x1
//ntpd: p adjtimex freq:-14636245 offset:-4573 constant:10 status:0x41
//ntpd: c adjtimex freq:-14642034 offset:-11715 constant:10 status:0x1
//ntpd: p adjtimex freq:-14642034 offset:-4098 constant:10 status:0x41
//ntpd: c adjtimex freq:-14699112 offset:-11746 constant:10 status:0x4001
//ntpd: p adjtimex freq:-14699112 offset:-4239 constant:10 status:0x4041
//ntpd: c adjtimex freq:-14762330 offset:-12786 constant:10 status:0x4001
//ntpd: p adjtimex freq:-14762330 offset:-4434 constant:10 status:0x4041
//ntpd: b adjtimex freq:0 offset:-9669 constant:8 status:0x1
//ntpd: adjtimex:0 freq:-14809095 offset:-9669 constant:10 status:0x4001
//ntpd: c adjtimex freq:-14809095 offset:-9669 constant:10 status:0x4001

	return 1; /* "ok to increase poll interval" */
}


/*
 * We've got a new reply packet from a peer, process it
 * (helpers first)
 */
static unsigned
retry_interval(void)
{
	/* Local problem, want to retry soon */
	unsigned interval, r;
	interval = RETRY_INTERVAL;
	r = random();
	interval += r % (unsigned)(RETRY_INTERVAL / 4);
	VERB3 bb_error_msg("chose retry interval:%u", interval);
	return interval;
}
static unsigned
poll_interval(int exponent) /* exp is always -1 or 0 */
{
	/* Want to send next packet at (1 << G.poll_exp) + small random value */
	unsigned interval, r;
	exponent += G.poll_exp; /* G.poll_exp is always > 0 */
	/* never true: if (exp < 0) exp = 0; */
	interval = 1 << exponent;
	r = random();
	interval += ((r & (interval-1)) >> 4) + ((r >> 8) & 1); /* + 1/16 of interval, max */
	VERB3 bb_error_msg("chose poll interval:%u (poll_exp:%d exp:%d)", interval, G.poll_exp, exponent);
	return interval;
}
static void
recv_and_process_peer_pkt(peer_t *p)
{
	int         rc;
	ssize_t     size;
	msg_t       msg;
	double      T1, T2, T3, T4;
	unsigned    interval;
	datapoint_t *datapoint;
	peer_t      *q;

	/* We can recvfrom here and check from.IP, but some multihomed
	 * ntp servers reply from their *other IP*.
	 * TODO: maybe we should check at least what we can: from.port == 123?
	 */
	size = recv(p->p_fd, &msg, sizeof(msg), MSG_DONTWAIT);
	if (size == -1) {
		bb_perror_msg("recv(%s) error", p->p_dotted);
		if (errno == EHOSTUNREACH || errno == EHOSTDOWN
		 || errno == ENETUNREACH || errno == ENETDOWN
		 || errno == ECONNREFUSED || errno == EADDRNOTAVAIL
		 || errno == EAGAIN
		) {
//TODO: always do this?
			set_next(p, retry_interval());
			goto close_sock;
		}
		xfunc_die();
	}

	if (size != NTP_MSGSIZE_NOAUTH && size != NTP_MSGSIZE) {
		bb_error_msg("malformed packet received from %s", p->p_dotted);
		goto bail;
	}

	if (msg.m_orgtime.int_partl != p->p_xmt_msg.m_xmttime.int_partl
	 || msg.m_orgtime.fractionl != p->p_xmt_msg.m_xmttime.fractionl
	) {
		goto bail;
	}

	if ((msg.m_status & LI_ALARM) == LI_ALARM
	 || msg.m_stratum == 0
	 || msg.m_stratum > NTP_MAXSTRATUM
	) {
// TODO: stratum 0 responses may have commands in 32-bit m_refid field:
// "DENY", "RSTR" - peer does not like us at all
// "RATE" - peer is overloaded, reduce polling freq
		interval = poll_interval(0);
		bb_error_msg("reply from %s: not synced, next query in %us", p->p_dotted, interval);
		goto close_sock;
	}

//	/* Verify valid root distance */
//	if (msg.m_rootdelay / 2 + msg.m_rootdisp >= MAXDISP || p->lastpkt_reftime > msg.m_xmt)
//		return;                 /* invalid header values */

	p->lastpkt_status = msg.m_status;
	p->lastpkt_stratum = msg.m_stratum;
	p->lastpkt_rootdelay = sfp_to_d(msg.m_rootdelay);
	p->lastpkt_rootdisp = sfp_to_d(msg.m_rootdisp);
	p->lastpkt_refid = msg.m_refid;

	/*
	 * From RFC 2030 (with a correction to the delay math):
	 *
	 * Timestamp Name          ID   When Generated
	 * ------------------------------------------------------------
	 * Originate Timestamp     T1   time request sent by client
	 * Receive Timestamp       T2   time request received by server
	 * Transmit Timestamp      T3   time reply sent by server
	 * Destination Timestamp   T4   time reply received by client
	 *
	 * The roundtrip delay and local clock offset are defined as
	 *
	 * delay = (T4 - T1) - (T3 - T2); offset = ((T2 - T1) + (T3 - T4)) / 2
	 */
	T1 = p->p_xmttime;
	T2 = lfp_to_d(msg.m_rectime);
	T3 = lfp_to_d(msg.m_xmttime);
	T4 = gettime1900d();

	p->lastpkt_recv_time = T4;

	VERB5 bb_error_msg("%s->lastpkt_recv_time=%f", p->p_dotted, p->lastpkt_recv_time);
	p->datapoint_idx = p->p_reachable_bits ? (p->datapoint_idx + 1) % NUM_DATAPOINTS : 0;
	datapoint = &p->filter_datapoint[p->datapoint_idx];
	datapoint->d_recv_time = T4;
	datapoint->d_offset    = ((T2 - T1) + (T3 - T4)) / 2;
	/* The delay calculation is a special case. In cases where the
	 * server and client clocks are running at different rates and
	 * with very fast networks, the delay can appear negative. In
	 * order to avoid violating the Principle of Least Astonishment,
	 * the delay is clamped not less than the system precision.
	 */
	p->lastpkt_delay = (T4 - T1) - (T3 - T2);
	if (p->lastpkt_delay < G_precision_sec)
		p->lastpkt_delay = G_precision_sec;
	datapoint->d_dispersion = LOG2D(msg.m_precision_exp) + G_precision_sec;
	if (!p->p_reachable_bits) {
		/* 1st datapoint ever - replicate offset in every element */
		int i;
		for (i = 1; i < NUM_DATAPOINTS; i++) {
			p->filter_datapoint[i].d_offset = datapoint->d_offset;
		}
	}

	p->p_reachable_bits |= 1;
	VERB1 {
		bb_error_msg("reply from %s: reach 0x%02x offset %f delay %f",
			p->p_dotted,
			p->p_reachable_bits,
			datapoint->d_offset, p->lastpkt_delay);
	}

	/* Muck with statictics and update the clock */
	filter_datapoints(p, T4);
	q = select_and_cluster(T4);
	rc = -1;
	if (q)
		rc = update_local_clock(q, T4);

	if (rc != 0) {
		/* Adjust the poll interval by comparing the current offset
		 * with the clock jitter. If the offset is less than
		 * the clock jitter times a constant, then the averaging interval
		 * is increased, otherwise it is decreased. A bit of hysteresis
		 * helps calm the dance. Works best using burst mode.
		 */
		VERB4 if (rc > 0) {
			bb_error_msg("offset:%f POLLADJ_GATE*discipline_jitter:%f poll:%s",
				q->filter_offset, POLLADJ_GATE * G.discipline_jitter,
				fabs(q->filter_offset) < POLLADJ_GATE * G.discipline_jitter
					? "grows" : "falls"
			);
		}
		if (rc > 0 && fabs(q->filter_offset) < POLLADJ_GATE * G.discipline_jitter) {
			/* was += G.poll_exp but it is a bit
			 * too optimistic for my taste at high poll_exp's */
			G.polladj_count += MINPOLL;
			if (G.polladj_count > POLLADJ_LIMIT) {
				G.polladj_count = 0;
				if (G.poll_exp < MAXPOLL) {
					G.poll_exp++;
					VERB3 bb_error_msg("polladj: discipline_jitter:%f ++poll_exp=%d",
							G.discipline_jitter, G.poll_exp);
				}
			} else {
				VERB3 bb_error_msg("polladj: incr:%d", G.polladj_count);
			}
		} else {
			G.polladj_count -= G.poll_exp * 2;
			if (G.polladj_count < -POLLADJ_LIMIT) {
				G.polladj_count = 0;
				if (G.poll_exp > MINPOLL) {
					llist_t *item;

					G.poll_exp--;
					/* Correct p->next_action_time in each peer
					 * which waits for sending, so that they send earlier.
					 * Old pp->next_action_time are on the order
					 * of t + (1 << old_poll_exp) + small_random,
					 * we simply need to subtract ~half of that.
					 */
					for (item = G.ntp_peers; item != NULL; item = item->link) {
						peer_t *pp = (peer_t *) item->data;
						if (pp->p_fd < 0)
							pp->next_action_time -= (1 << G.poll_exp);
					}
					VERB3 bb_error_msg("polladj: discipline_jitter:%f --poll_exp=%d",
							G.discipline_jitter, G.poll_exp);
				}
			} else {
				VERB3 bb_error_msg("polladj: decr:%d", G.polladj_count);
			}
		}
	}

	/* Decide when to send new query for this peer */
	interval = poll_interval(0);
	set_next(p, interval);

 close_sock:
	/* We do not expect any more packets from this peer for now.
	 * Closing the socket informs kernel about it.
	 * We open a new socket when we send a new query.
	 */
	close(p->p_fd);
	p->p_fd = -1;
 bail:
	return;
}

#if ENABLE_FEATURE_NTPD_SERVER
static void
recv_and_process_client_pkt(void /*int fd*/)
{
	ssize_t          size;
	uint8_t          version;
	double           rectime;
	len_and_sockaddr *to;
	struct sockaddr  *from;
	msg_t            msg;
	uint8_t          query_status;
	l_fixedpt_t      query_xmttime;

	to = get_sock_lsa(G.listen_fd);
	from = xzalloc(to->len);

	size = recv_from_to(G.listen_fd, &msg, sizeof(msg), MSG_DONTWAIT, from, &to->u.sa, to->len);
	if (size != NTP_MSGSIZE_NOAUTH && size != NTP_MSGSIZE) {
		char *addr;
		if (size < 0) {
			if (errno == EAGAIN)
				goto bail;
			bb_perror_msg_and_die("recv");
		}
		addr = xmalloc_sockaddr2dotted_noport(from);
		bb_error_msg("malformed packet received from %s: size %u", addr, (int)size);
		free(addr);
		goto bail;
	}

	query_status = msg.m_status;
	query_xmttime = msg.m_xmttime;

	/* Build a reply packet */
	memset(&msg, 0, sizeof(msg));
	msg.m_status = G.stratum < MAXSTRAT ? G.ntp_status : LI_ALARM;
	msg.m_status |= (query_status & VERSION_MASK);
	msg.m_status |= ((query_status & MODE_MASK) == MODE_CLIENT) ?
			 MODE_SERVER : MODE_SYM_PAS;
	msg.m_stratum = G.stratum;
	msg.m_ppoll = G.poll_exp;
	msg.m_precision_exp = G_precision_exp;
	rectime = gettime1900d();
	msg.m_xmttime = msg.m_rectime = d_to_lfp(rectime);
	msg.m_reftime = d_to_lfp(G.reftime);
	msg.m_orgtime = query_xmttime;
	msg.m_rootdelay = d_to_sfp(G.rootdelay);
//simple code does not do this, fix simple code!
	msg.m_rootdisp = d_to_sfp(G.rootdisp);
	version = (query_status & VERSION_MASK); /* ... >> VERSION_SHIFT - done below instead */
	msg.m_refid = G.refid; // (version > (3 << VERSION_SHIFT)) ? G.refid : G.refid3;

	/* We reply from the local address packet was sent to,
	 * this makes to/from look swapped here: */
	do_sendto(G.listen_fd,
		/*from:*/ &to->u.sa, /*to:*/ from, /*addrlen:*/ to->len,
		&msg, size);

 bail:
	free(to);
	free(from);
}
#endif

/* Upstream ntpd's options:
 *
 * -4   Force DNS resolution of host names to the IPv4 namespace.
 * -6   Force DNS resolution of host names to the IPv6 namespace.
 * -a   Require cryptographic authentication for broadcast client,
 *      multicast client and symmetric passive associations.
 *      This is the default.
 * -A   Do not require cryptographic authentication for broadcast client,
 *      multicast client and symmetric passive associations.
 *      This is almost never a good idea.
 * -b   Enable the client to synchronize to broadcast servers.
 * -c conffile
 *      Specify the name and path of the configuration file,
 *      default /etc/ntp.conf
 * -d   Specify debugging mode. This option may occur more than once,
 *      with each occurrence indicating greater detail of display.
 * -D level
 *      Specify debugging level directly.
 * -f driftfile
 *      Specify the name and path of the frequency file.
 *      This is the same operation as the "driftfile FILE"
 *      configuration command.
 * -g   Normally, ntpd exits with a message to the system log
 *      if the offset exceeds the panic threshold, which is 1000 s
 *      by default. This option allows the time to be set to any value
 *      without restriction; however, this can happen only once.
 *      If the threshold is exceeded after that, ntpd will exit
 *      with a message to the system log. This option can be used
 *      with the -q and -x options. See the tinker command for other options.
 * -i jaildir
 *      Chroot the server to the directory jaildir. This option also implies
 *      that the server attempts to drop root privileges at startup
 *      (otherwise, chroot gives very little additional security).
 *      You may need to also specify a -u option.
 * -k keyfile
 *      Specify the name and path of the symmetric key file,
 *      default /etc/ntp/keys. This is the same operation
 *      as the "keys FILE" configuration command.
 * -l logfile
 *      Specify the name and path of the log file. The default
 *      is the system log file. This is the same operation as
 *      the "logfile FILE" configuration command.
 * -L   Do not listen to virtual IPs. The default is to listen.
 * -n   Don't fork.
 * -N   To the extent permitted by the operating system,
 *      run the ntpd at the highest priority.
 * -p pidfile
 *      Specify the name and path of the file used to record the ntpd
 *      process ID. This is the same operation as the "pidfile FILE"
 *      configuration command.
 * -P priority
 *      To the extent permitted by the operating system,
 *      run the ntpd at the specified priority.
 * -q   Exit the ntpd just after the first time the clock is set.
 *      This behavior mimics that of the ntpdate program, which is
 *      to be retired. The -g and -x options can be used with this option.
 *      Note: The kernel time discipline is disabled with this option.
 * -r broadcastdelay
 *      Specify the default propagation delay from the broadcast/multicast
 *      server to this client. This is necessary only if the delay
 *      cannot be computed automatically by the protocol.
 * -s statsdir
 *      Specify the directory path for files created by the statistics
 *      facility. This is the same operation as the "statsdir DIR"
 *      configuration command.
 * -t key
 *      Add a key number to the trusted key list. This option can occur
 *      more than once.
 * -u user[:group]
 *      Specify a user, and optionally a group, to switch to.
 * -v variable
 * -V variable
 *      Add a system variable listed by default.
 * -x   Normally, the time is slewed if the offset is less than the step
 *      threshold, which is 128 ms by default, and stepped if above
 *      the threshold. This option sets the threshold to 600 s, which is
 *      well within the accuracy window to set the clock manually.
 *      Note: since the slew rate of typical Unix kernels is limited
 *      to 0.5 ms/s, each second of adjustment requires an amortization
 *      interval of 2000 s. Thus, an adjustment as much as 600 s
 *      will take almost 14 days to complete. This option can be used
 *      with the -g and -q options. See the tinker command for other options.
 *      Note: The kernel time discipline is disabled with this option.
 */

/* By doing init in a separate function we decrease stack usage
 * in main loop.
 */
static NOINLINE void ntp_init(char **argv)
{
	unsigned opts;
	llist_t *peers;

	srandom(getpid());

	if (getuid())
		bb_error_msg_and_die(bb_msg_you_must_be_root);

	/* Set some globals */
#if 0
	/* With constant b = 100, G.precision_exp is also constant -6.
	 * Uncomment this to verify.
	 */
	{
		int prec = 0;
		int b;
# if 0
		struct timespec	tp;
		/* We can use sys_clock_getres but assuming 10ms tick should be fine */
		clock_getres(CLOCK_REALTIME, &tp);
		tp.tv_sec = 0;
		tp.tv_nsec = 10000000;
		b = 1000000000 / tp.tv_nsec;  /* convert to Hz */
# else
		b = 100; /* b = 1000000000/10000000 = 100 */
# endif
		while (b > 1)
			prec--, b >>= 1;
		/*G.precision_exp = prec;*/
		/*G.precision_sec = (1.0 / (1 << (- prec)));*/
		bb_error_msg("G.precision_exp:%d sec:%f", prec, G_precision_sec); /* -6 */
	}
#endif
	G.stratum = MAXSTRAT;
	G.poll_exp = 1; /* should use MINPOLL, but 1 speeds up initial sync */
	G.reftime = G.last_update_recv_time = gettime1900d();

	/* Parse options */
	peers = NULL;
	opt_complementary = "dd:p::"; /* d: counter, p: list */
	opts = getopt32(argv,
			"nqNx" /* compat */
			"p:"IF_FEATURE_NTPD_SERVER("l") /* NOT compat */
			"d" /* compat */
			"46aAbgL", /* compat, ignored */
			&peers, &G.verbose);
	if (!(opts & (OPT_p|OPT_l)))
		bb_show_usage();
//	if (opts & OPT_x) /* disable stepping, only slew is allowed */
//		G.time_was_stepped = 1;
	while (peers)
		add_peers(llist_pop(&peers));
	if (!(opts & OPT_n)) {
		bb_daemonize_or_rexec(DAEMON_DEVNULL_STDIO, argv);
		logmode = LOGMODE_NONE;
	}
#if ENABLE_FEATURE_NTPD_SERVER
	G.listen_fd = -1;
	if (opts & OPT_l) {
		G.listen_fd = create_and_bind_dgram_or_die(NULL, 123);
		socket_want_pktinfo(G.listen_fd);
		setsockopt(G.listen_fd, IPPROTO_IP, IP_TOS, &const_IPTOS_LOWDELAY, sizeof(const_IPTOS_LOWDELAY));
	}
#endif
	/* I hesitate to set -20 prio. -15 should be high enough for timekeeping */
	if (opts & OPT_N)
		setpriority(PRIO_PROCESS, 0, -15);

	bb_signals((1 << SIGTERM) | (1 << SIGINT), record_signo);
	bb_signals((1 << SIGPIPE) | (1 << SIGHUP), SIG_IGN);
}

int ntpd_main(int argc UNUSED_PARAM, char **argv) MAIN_EXTERNALLY_VISIBLE;
int ntpd_main(int argc UNUSED_PARAM, char **argv)
{
	struct globals g;
	struct pollfd *pfd;
	peer_t **idx2peer;

	memset(&g, 0, sizeof(g));
	SET_PTR_TO_GLOBALS(&g);

	ntp_init(argv);

	{
		/* if ENABLE_FEATURE_NTPD_SERVER, + 1 for listen_fd: */
		unsigned cnt = g.peer_cnt + ENABLE_FEATURE_NTPD_SERVER;
		idx2peer = xzalloc(sizeof(idx2peer[0]) * cnt);
		pfd = xzalloc(sizeof(pfd[0]) * cnt);
	}

	while (!bb_got_signal) {
		llist_t *item;
		unsigned i, j;
		unsigned sent_cnt, trial_cnt;
		int nfds, timeout;
		time_t cur_time, nextaction;

		/* Nothing between here and poll() blocks for any significant time */

		cur_time = time(NULL);
		nextaction = cur_time + 3600;

		i = 0;
#if ENABLE_FEATURE_NTPD_SERVER
		if (g.listen_fd != -1) {
			pfd[0].fd = g.listen_fd;
			pfd[0].events = POLLIN;
			i++;
		}
#endif
		/* Pass over peer list, send requests, time out on receives */
		sent_cnt = trial_cnt = 0;
		for (item = g.ntp_peers; item != NULL; item = item->link) {
			peer_t *p = (peer_t *) item->data;

			/* Overflow-safe "if (p->next_action_time <= cur_time) ..." */
			if ((int)(cur_time - p->next_action_time) >= 0) {
				if (p->p_fd == -1) {
					/* Time to send new req */
					trial_cnt++;
					if (send_query_to_peer(p) == 0)
						sent_cnt++;
				} else {
					/* Timed out waiting for reply */
					close(p->p_fd);
					p->p_fd = -1;
					timeout = poll_interval(-1); /* try a bit faster */
					bb_error_msg("timed out waiting for %s, reach 0x%02x, next query in %us",
							p->p_dotted, p->p_reachable_bits, timeout);
					set_next(p, timeout);
				}
			}

			if (p->next_action_time < nextaction)
				nextaction = p->next_action_time;

			if (p->p_fd >= 0) {
				/* Wait for reply from this peer */
				pfd[i].fd = p->p_fd;
				pfd[i].events = POLLIN;
				idx2peer[i] = p;
				i++;
			}
		}

		timeout = nextaction - cur_time;
		if (timeout < 1)
			timeout = 1;

		/* Here we may block */
		VERB2 bb_error_msg("poll %us, sockets:%u", timeout, i);
		nfds = poll(pfd, i, timeout * 1000);
		if (nfds <= 0)
			continue;

		/* Process any received packets */
		j = 0;
#if ENABLE_FEATURE_NTPD_SERVER
		if (g.listen_fd != -1) {
			if (pfd[0].revents /* & (POLLIN|POLLERR)*/) {
				nfds--;
				recv_and_process_client_pkt(/*g.listen_fd*/);
			}
			j = 1;
		}
#endif
		for (; nfds != 0 && j < i; j++) {
			if (pfd[j].revents /* & (POLLIN|POLLERR)*/) {
				nfds--;
				recv_and_process_peer_pkt(idx2peer[j]);
			}
		}
	} /* while (!bb_got_signal) */

	kill_myself_with_sig(bb_got_signal);
}






/*** openntpd-4.6 uses only adjtime, not adjtimex ***/

/*** ntp-4.2.6/ntpd/ntp_loopfilter.c - adjtimex usage ***/

#if 0
static double
direct_freq(double fp_offset)
{

#ifdef KERNEL_PLL
	/*
	 * If the kernel is enabled, we need the residual offset to
	 * calculate the frequency correction.
	 */
	if (pll_control && kern_enable) {
		memset(&ntv, 0, sizeof(ntv));
		ntp_adjtime(&ntv);
#ifdef STA_NANO
		clock_offset = ntv.offset / 1e9;
#else /* STA_NANO */
		clock_offset = ntv.offset / 1e6;
#endif /* STA_NANO */
		drift_comp = FREQTOD(ntv.freq);
	}
#endif /* KERNEL_PLL */
	set_freq((fp_offset - clock_offset) / (current_time - clock_epoch) + drift_comp);
	wander_resid = 0;
	return drift_comp;
}

static void
set_freq(double	freq) /* frequency update */
{
	char tbuf[80];

	drift_comp = freq;

#ifdef KERNEL_PLL
	/*
	 * If the kernel is enabled, update the kernel frequency.
	 */
	if (pll_control && kern_enable) {
		memset(&ntv, 0, sizeof(ntv));
		ntv.modes = MOD_FREQUENCY;
		ntv.freq = DTOFREQ(drift_comp);
		ntp_adjtime(&ntv);
		snprintf(tbuf, sizeof(tbuf), "kernel %.3f PPM", drift_comp * 1e6);
		report_event(EVNT_FSET, NULL, tbuf);
	} else {
		snprintf(tbuf, sizeof(tbuf), "ntpd %.3f PPM", drift_comp * 1e6);
		report_event(EVNT_FSET, NULL, tbuf);
	}
#else /* KERNEL_PLL */
	snprintf(tbuf, sizeof(tbuf), "ntpd %.3f PPM", drift_comp * 1e6);
	report_event(EVNT_FSET, NULL, tbuf);
#endif /* KERNEL_PLL */
}

...
...
...

#ifdef KERNEL_PLL
	/*
	 * This code segment works when clock adjustments are made using
	 * precision time kernel support and the ntp_adjtime() system
	 * call. This support is available in Solaris 2.6 and later,
	 * Digital Unix 4.0 and later, FreeBSD, Linux and specially
	 * modified kernels for HP-UX 9 and Ultrix 4. In the case of the
	 * DECstation 5000/240 and Alpha AXP, additional kernel
	 * modifications provide a true microsecond clock and nanosecond
	 * clock, respectively.
	 *
	 * Important note: The kernel discipline is used only if the
	 * step threshold is less than 0.5 s, as anything higher can
	 * lead to overflow problems. This might occur if some misguided
	 * lad set the step threshold to something ridiculous.
	 */
	if (pll_control && kern_enable) {

#define MOD_BITS (MOD_OFFSET | MOD_MAXERROR | MOD_ESTERROR | MOD_STATUS | MOD_TIMECONST)

		/*
		 * We initialize the structure for the ntp_adjtime()
		 * system call. We have to convert everything to
		 * microseconds or nanoseconds first. Do not update the
		 * system variables if the ext_enable flag is set. In
		 * this case, the external clock driver will update the
		 * variables, which will be read later by the local
		 * clock driver. Afterwards, remember the time and
		 * frequency offsets for jitter and stability values and
		 * to update the frequency file.
		 */
		memset(&ntv,  0, sizeof(ntv));
		if (ext_enable) {
			ntv.modes = MOD_STATUS;
		} else {
#ifdef STA_NANO
			ntv.modes = MOD_BITS | MOD_NANO;
#else /* STA_NANO */
			ntv.modes = MOD_BITS;
#endif /* STA_NANO */
			if (clock_offset < 0)
				dtemp = -.5;
			else
				dtemp = .5;
#ifdef STA_NANO
			ntv.offset = (int32)(clock_offset * 1e9 + dtemp);
			ntv.constant = sys_poll;
#else /* STA_NANO */
			ntv.offset = (int32)(clock_offset * 1e6 + dtemp);
			ntv.constant = sys_poll - 4;
#endif /* STA_NANO */
			ntv.esterror = (u_int32)(clock_jitter * 1e6);
			ntv.maxerror = (u_int32)((sys_rootdelay / 2 + sys_rootdisp) * 1e6);
			ntv.status = STA_PLL;

			/*
			 * Enable/disable the PPS if requested.
			 */
			if (pps_enable) {
				if (!(pll_status & STA_PPSTIME))
					report_event(EVNT_KERN,
					    NULL, "PPS enabled");
				ntv.status |= STA_PPSTIME | STA_PPSFREQ;
			} else {
				if (pll_status & STA_PPSTIME)
					report_event(EVNT_KERN,
					    NULL, "PPS disabled");
				ntv.status &= ~(STA_PPSTIME |
				    STA_PPSFREQ);
			}
			if (sys_leap == LEAP_ADDSECOND)
				ntv.status |= STA_INS;
			else if (sys_leap == LEAP_DELSECOND)
				ntv.status |= STA_DEL;
		}

		/*
		 * Pass the stuff to the kernel. If it squeals, turn off
		 * the pps. In any case, fetch the kernel offset,
		 * frequency and jitter.
		 */
		if (ntp_adjtime(&ntv) == TIME_ERROR) {
			if (!(ntv.status & STA_PPSSIGNAL))
				report_event(EVNT_KERN, NULL,
				    "PPS no signal");
		}
		pll_status = ntv.status;
#ifdef STA_NANO
		clock_offset = ntv.offset / 1e9;
#else /* STA_NANO */
		clock_offset = ntv.offset / 1e6;
#endif /* STA_NANO */
		clock_frequency = FREQTOD(ntv.freq);

		/*
		 * If the kernel PPS is lit, monitor its performance.
		 */
		if (ntv.status & STA_PPSTIME) {
#ifdef STA_NANO
			clock_jitter = ntv.jitter / 1e9;
#else /* STA_NANO */
			clock_jitter = ntv.jitter / 1e6;
#endif /* STA_NANO */
		}

#if defined(STA_NANO) && NTP_API == 4
		/*
		 * If the TAI changes, update the kernel TAI.
		 */
		if (loop_tai != sys_tai) {
			loop_tai = sys_tai;
			ntv.modes = MOD_TAI;
			ntv.constant = sys_tai;
			ntp_adjtime(&ntv);
		}
#endif /* STA_NANO */
	}
#endif /* KERNEL_PLL */
#endif