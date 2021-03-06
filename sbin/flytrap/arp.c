/*-
 * Copyright (c) 2016-2018 The University of Oslo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/time.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft/arp.h>
#include <ft/assert.h>
#include <ft/endian.h>
#include <ft/ethernet.h>
#include <ft/ip4.h>
#include <ft/log.h>

#include "flytrap.h"
#include "flow.h"
#include "iface.h"
#include "packet.h"

/* magic value for "never seen" */
#define ARP_NEVER	UINT64_MAX

/* min unanswered ARP requests before we claim an address */
#define ARP_MINREQ	     3

/* how long to wait (in ms) before claiming an address */
#define ARP_TIMEOUT	  3000

/* age (in ms) of an entry before it is considered stale */
#define ARP_STALE	 30000

/* age (in ms) of an entry before it is removed from the tree */
#define ARP_EXPIRE	300000

/*
 * A node in the tree.
 */
struct arpn {
	uint32_t	 addr;		/* network address */
	uint8_t		 plen;		/* prefix length */
	union {
		/* leaf node */
		uint64_t	 first;		/* first seen (ms) */
		/* inner node */
		uint64_t	 oldest;	/* oldest child */
	};
	union {
		/* leaf node */
		uint64_t	 last;		/* last seen (ms) */
		/* inner node */
		uint64_t	 newest;	/* newest child */
	};
	union {
		/* leaf node */
		struct {
			ether_addr	 ether;		/* Ethernet address */
			unsigned int	 nreq;		/* requests seen */
			int		 claimed:1;	/* claimed by us */
			int		 reserved:1;	/* reserved address */
		};
		/* inner node */
		struct {
			struct arpn	*sub[16];	/* children */
		};
	};
};

static struct arpn arp_root = { .first = ARP_NEVER };
static unsigned int narpn, nleaves;

/*
 * Print the leaf nodes of a tree in order.
 */
static void
arp_print_tree(FILE *f, struct arpn *n)
{
	unsigned int i;

	fprintf(f, "%*s%u.%u.%u.%u",
	    (int)(n->plen / 2), "",
	    (n->addr >> 24) & 0xff,
	    (n->addr >> 16) & 0xff,
	    (n->addr >> 8) & 0xff,
	    n->addr & 0xff);
	if (n->plen < 32) {
		fprintf(f, "/%u", n->plen);
		if (n->newest > 0) {
			fprintf(f, " %lu.%03lu s",
			    U64_SEC_UL(ft_time - n->newest),
			    U64_MSEC_UL(ft_time - n->newest));
		}
		fprintf(f, "\n");
		for (i = 0; i < 16; ++i)
			if (n->sub[i] != NULL)
				arp_print_tree(f, n->sub[i]);
	} else if (n->nreq > 0) {
		fprintf(f, " unknown (%u req)\n", n->nreq);
	} else {
		fprintf(f, " = %02x:%02x:%02x:%02x:%02x:%02x %lu.%03lu s%s\n",
		    n->ether.o[0], n->ether.o[1], n->ether.o[2],
		    n->ether.o[3], n->ether.o[4], n->ether.o[5],
		    U64_SEC_UL(ft_time - n->last),
		    U64_MSEC_UL(ft_time - n->last),
		    n->claimed ? " !" : "");
	}
}

/*
 * Create a node for the subnet of the specified prefix length which
 * contains the specified address.
 */
static struct arpn *
arp_new(uint32_t addr, uint8_t plen)
{
	struct arpn *n;

	if ((n = calloc(1, sizeof *n)) == NULL)
		return (NULL);
	narpn++;
	n->addr = addr & -(1 << (32 - plen));
	n->plen = plen;
	n->first = ARP_NEVER;
	n->last = 0;
	ft_debug("created node %u.%u.%u.%u/%u",
	    (n->addr >> 24) & 0xff, (n->addr >> 16) & 0xff,
	    (n->addr >> 8) & 0xff, n->addr & 0xff, n->plen);
	return (n);
}

/*
 * Delete all children of a given node in a tree.
 */
static void
arp_delete(struct arpn *n)
{
	unsigned int i;

	if (n == NULL)
		return;
	if (n->plen == 32)
		nleaves--;
	else
		for (i = 0; i < 16; ++i)
			if (n->sub[i] != NULL)
				arp_delete(n->sub[i]);
	ft_debug("deleted node %u.%u.%u.%u/%u",
	    (n->addr >> 24) & 0xff, (n->addr >> 16) & 0xff,
	    (n->addr >> 8) & 0xff, n->addr & 0xff, n->plen);
	narpn--;
	free(n);
}

/*
 * Expire
 */
static void
arp_expire(struct arpn *n, uint64_t cutoff)
{
	unsigned int i, ndel, nexp;

	if (n == NULL)
		n = &arp_root;
	ndel = narpn;
	nexp = nleaves;
	ft_debug("expiring in %u.%u.%u.%u/%u "
	    "oldest %lu.%03lu s newest %lu.%03lu s",
	    (n->addr >> 24) & 0xff, (n->addr >> 16) & 0xff,
	    (n->addr >> 8) & 0xff, n->addr & 0xff, n->plen,
	    U64_SEC_UL(ft_time - n->oldest), U64_MSEC_UL(ft_time - n->oldest),
	    U64_SEC_UL(ft_time - n->newest), U64_MSEC_UL(ft_time - n->newest));
	/* reset fences */
	n->first = ARP_NEVER;
	n->last = 0;
	/* iterate over children */
	for (i = 0; i < 16; ++i) {
		if (n->sub[i] == NULL)
			continue;
		if (n->sub[i]->plen < 32) {
			/* check descendants first */
			if (n->sub[i]->oldest < cutoff)
				arp_expire(n->sub[i], cutoff);
		}
		if (n->sub[i]->newest < cutoff) {
			/* expired or empty */
			arp_delete(n->sub[i]);
			n->sub[i] = NULL;
		}
		if (n->sub[i] != NULL) {
			/* update our fences */
			if (n->sub[i]->newest < n->oldest)
				n->oldest = n->sub[i]->newest;
			if (n->sub[i]->newest > n->newest)
				n->newest = n->sub[i]->newest;
		}
	}
	ndel -= narpn;
	nexp -= nleaves;
	if (nexp > 0 || ndel > 0) {
		ft_debug("expired %u nodes under %u.%u.%u.%u/%u (%u deleted)",
		    nexp, (n->addr >> 24) & 0xff, (n->addr >> 16) & 0xff,
		    (n->addr >> 8) & 0xff, n->addr & 0xff, n->plen, ndel);
	}
}

/*
 * Periodic maintenance
 */
void
arp_periodic(struct timeval *tv)
{
	uint64_t now;

	now = tv->tv_sec * 1000;
	arp_expire(NULL, now - ARP_EXPIRE);
}

/*
 * Insert an address into a tree.
 */
static struct arpn *
arp_insert(struct arpn *n, uint32_t addr)
{
	struct arpn *sn, *rn;
	uint32_t sub;
	uint8_t splen;

	if (n == NULL)
		n = &arp_root;
	if (n->plen == 32) {
		ft_assert(n->addr == addr);
		if (ft_time < n->first)
			n->first = ft_time;
		if (ft_time > n->last)
			n->last = ft_time;
		return (n);
	}
	splen = n->plen + 4;
	sub = (addr >> (32 - splen)) % 16;
	if ((sn = n->sub[sub]) == NULL) {
		if ((sn = arp_new(addr, splen)) == NULL)
			return (NULL);
		if (sn->plen == 32) {
			ft_verbose("arp: inserted %u.%u.%u.%u",
			    (addr >> 24) & 0xff, (addr >> 16) & 0xff,
			    (addr >> 8) & 0xff, addr & 0xff);
			nleaves++;
		}
		n->sub[sub] = sn;
	}
	if ((rn = arp_insert(sn, addr)) == NULL)
		return (NULL);
	/* for non-leaf nodes, first / last means oldest / newest */
	if (sn->newest < n->oldest)
		n->oldest = sn->newest;
	if (sn->newest > n->newest)
		n->newest = sn->newest;
	return (rn);
}

/*
 * ARP registration
 */
int
arp_register(const ip4_addr *ip4, const ether_addr *ether)
{
	struct arpn *an;

	if ((an = arp_insert(NULL, be32toh(ip4->q))) == NULL)
		return (-1);
	if (memcmp(&an->ether, ether, sizeof an->ether) != 0) {
		/* warn if the ip4_addr moved from one ether_addr to another */
		if (an->ether.o[0] || an->ether.o[1] || an->ether.o[2] ||
		    an->ether.o[3] || an->ether.o[4] || an->ether.o[5]) {
			ft_verbose("%u.%u.%u.%u moved"
			    " from %02x:%02x:%02x:%02x:%02x:%02x"
			    " to %02x:%02x:%02x:%02x:%02x:%02x",
			    ip4->o[0], ip4->o[1], ip4->o[2], ip4->o[3],
			    an->ether.o[0], an->ether.o[1], an->ether.o[2],
			    an->ether.o[3], an->ether.o[4], an->ether.o[5],
			    ether->o[0], ether->o[1], ether->o[2],
			    ether->o[3], ether->o[4], ether->o[5]);
		} else {
			ft_verbose("%u.%u.%u.%u registered"
			    " at %02x:%02x:%02x:%02x:%02x:%02x",
			    ip4->o[0], ip4->o[1], ip4->o[2], ip4->o[3],
			    ether->o[0], ether->o[1], ether->o[2],
			    ether->o[3], ether->o[4], ether->o[5]);
		}
		memcpy(&an->ether, ether, sizeof an->ether);
	}
	an->nreq = 0;
	return (0);
}

/*
 * ARP lookup
 */
int
arp_lookup(const ip4_addr *ip4, ether_addr *ether)
{
	struct arpn *an;

	ft_debug("ARP lookup %u.%u.%u.%u",
	    ip4->o[0], ip4->o[1], ip4->o[2], ip4->o[3]);
	an = &arp_root;
	if ((an = an->sub[ip4->o[0] / 16]) == NULL ||
	    (an = an->sub[ip4->o[0] % 16]) == NULL ||
	    (an = an->sub[ip4->o[1] / 16]) == NULL ||
	    (an = an->sub[ip4->o[1] % 16]) == NULL ||
	    (an = an->sub[ip4->o[2] / 16]) == NULL ||
	    (an = an->sub[ip4->o[2] % 16]) == NULL ||
	    (an = an->sub[ip4->o[3] / 16]) == NULL ||
	    (an = an->sub[ip4->o[3] % 16]) == NULL)
		return (-1);
	memcpy(ether, &an->ether, sizeof(ether_addr));
	ft_debug("%u.%u.%u.%u is"
	    " at %02x:%02x:%02x:%02x:%02x:%02x",
	    ip4->o[0], ip4->o[1], ip4->o[2], ip4->o[3],
	    ether->o[0], ether->o[1], ether->o[2],
	    ether->o[3], ether->o[4], ether->o[5]);
	return (0);
}

/*
 * Claim an IP address
 */
static int
arp_reply(const ether_flow *fl, const arp_pkt *iap, struct arpn *an)
{
	arp_pkt ap;

	(void)an;

	ap.htype = htobe16(arp_type_ether);
	ap.ptype = htobe16(arp_type_ip4);
	ap.hlen = 6;
	ap.plen = 4;
	ap.oper = htobe16(arp_oper_is_at);
	memcpy(&ap.sha, &fl->p->i->ether, sizeof(ether_addr));
	memcpy(&ap.spa, &iap->tpa, sizeof(ip4_addr));
	memcpy(&ap.tha, &iap->sha, sizeof(ether_addr));
	memcpy(&ap.tpa, &iap->spa, sizeof(ip4_addr));
	if (ethernet_reply(fl, &ap, sizeof ap) != 0)
		return (-1);
	return (0);
}

/*
 * Register a reserved address
 */
int
arp_reserve(const ip4_addr *addr)
{
	struct arpn *an;

	ft_debug("arp: reserving %u.%u.%u.%u",
	    addr->o[0], addr->o[1], addr->o[2], addr->o[3]);
	if ((an = arp_insert(NULL, be32toh(addr->q))) == NULL)
		return (-1);
	an->first = an->last = 0;
	an->reserved = 1;
	return (0);
}

/*
 * Analyze a captured ARP packet
 */
int
packet_analyze_arp(const ether_flow *fl, const void *data, size_t len)
{
	const arp_pkt *ap;
	struct arpn *an;

	if (len < sizeof(arp_pkt)) {
		ft_verbose("%lu.%03lu short ARP packet (%zd < %zd)",
		    FT_TIME_SEC_UL, FT_TIME_MSEC_UL, len, sizeof(arp_pkt));
		return (-1);
	}
	ap = (const arp_pkt *)data;
	ft_debug("\tARP htype 0x%04hx ptype 0x%04hx hlen %hd plen %hd",
	    be16toh(ap->htype), be16toh(ap->ptype), ap->hlen, ap->plen);
	if (be16toh(ap->htype) != arp_type_ether || ap->hlen != 6 ||
	    be16toh(ap->ptype) != arp_type_ip4 || ap->plen != 4) {
		ft_debug("\tARP packet ignored");
		return (0);
	}
	switch (be16toh(ap->oper)) {
	case arp_oper_who_has:
		ft_debug("\twho-has %u.%u.%u.%u tell %u.%u.%u.%u",
		    ap->tpa.o[0], ap->tpa.o[1], ap->tpa.o[2], ap->tpa.o[3],
		    ap->spa.o[0], ap->spa.o[1], ap->spa.o[2], ap->spa.o[3]);
		break;
	case arp_oper_is_at:
		ft_debug("\t%u.%u.%u.%u is-at %02x:%02x:%02x:%02x:%02x:%02x",
		    ap->spa.o[0], ap->spa.o[1], ap->spa.o[2], ap->spa.o[3],
		    ap->sha.o[0], ap->sha.o[1], ap->sha.o[2], ap->sha.o[3],
		    ap->sha.o[4], ap->sha.o[5]);
		break;
	default:
		ft_verbose("\tunknown operation 0x%04x", be16toh(ap->oper));
		return (0);
	}
	switch (be16toh(ap->oper)) {
	case arp_oper_who_has:
		/* ARP request */
		if (dst_set && !ip4s_lookup(dst_set, be32toh(ap->tpa.q))) {
			ft_debug("\ttarget address is out of bounds");
			break;
		}
		/* register sender */
		arp_register(&ap->spa, &ap->sha);
		/*
		 * Note that arp_insert() sets an->last = ft_time so we
		 * don't have to, but leaves an->first untouched.  For new
		 * nodes, this is the magic value ARP_NEVER.
		 */
		if ((an = arp_insert(NULL, be32toh(ap->tpa.q))) == NULL)
			return (-1);
		if (an->first == ARP_NEVER) {
			/* new entry */
			an->first = ft_time;
		} else {
			ft_verbose("%u.%u.%u.%u: last seen %lu.%03lu",
			    ap->tpa.o[0], ap->tpa.o[1], ap->tpa.o[2],
			    ap->tpa.o[3], U64_SEC_UL(an->last),
			    U64_MSEC_UL(an->last));
		}
		if (an->reserved) {
			/* ignore */
			ft_debug("\ttarget address is reserved");
			an->nreq = 0;
		} else if (an->claimed) {
			/* already ours, refresh */
			ft_debug("refreshing %u.%u.%u.%u", ap->tpa.o[0],
			    ap->tpa.o[1], ap->tpa.o[2], ap->tpa.o[3]);
			an->nreq = 0;
			if (arp_reply(fl, ap, an) != 0)
				return (-1);
		} else if (an->nreq == 0 || ft_time - an->last >= ARP_STALE) {
			/* new or stale, start over */
			an->nreq = 1;
			an->first = ft_time;
		} else if (an->nreq >= ARP_MINREQ &&
		    ft_time - an->first >= ARP_TIMEOUT) {
			/* claim new address */
			ft_verbose("claiming %u.%u.%u.%u nreq = %u in %lu ms",
			    ap->tpa.o[0], ap->tpa.o[1], ap->tpa.o[2],
			    ap->tpa.o[3], an->nreq,
			    (unsigned long)(ft_time - an->first));
			an->ether = fl->p->i->ether;
			an->claimed = 1;
			an->nreq = 0;
			if (arp_reply(fl, ap, an) != 0)
				return (-1);
		} else {
			an->nreq++;
			an->last = ft_time;
		}
		break;
	case arp_oper_is_at:
		/* ARP reply */
		arp_register(&ap->spa, &ap->sha);
		arp_register(&ap->tpa, &ap->tha);
		break;
	}
	/* run expiry */
	if (arp_root.oldest < ft_time - ARP_EXPIRE) {
		arp_expire(&arp_root, ft_time - ARP_EXPIRE);
		ft_debug("%u nodes / %u leaves in tree", narpn, nleaves);
	} else if (arp_root.oldest != ARP_NEVER) {
		ft_debug("%lu.%03lu s until expiry",
		    (arp_root.oldest + ARP_EXPIRE - ft_time) / 1000,
		    (arp_root.oldest + ARP_EXPIRE - ft_time) % 1000);
	}
	if (FT_LOG_LEVEL_DEBUG >= ft_log_level)
		arp_print_tree(stderr, &arp_root);
	return (0);
}
