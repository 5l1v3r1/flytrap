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

#include <stdint.h>
#include <stdio.h>

#include <cryb/test.h>

#include <ft/assert.h>

#include "t_ip4.h"

static struct t_ip4s_case {
	const char		*desc;
	const char		*insert;
	const char		*remove;
	unsigned long		 count;
	const char		*present;
	const char		*absent;
} t_ip4s_cases[] = {
	{
		.desc		 = "empty",
		.count		 = 0,
	},
	{
		.desc		 = "full",
		.insert		 = "0.0.0.0/0",
		.count		 = (1UL << 32),
		.present	 = "0.0.0.0,127.255.255.255,128.0.0.0,255.255.255.255",
	},
	{
		.desc		 = "half full",
		.insert		 = "0.0.0.0/1",
		.count		 = (1UL << 31),
		.present	 = "0.0.0.0,127.255.255.255",
		.absent		 = "128.0.0.0,255.255.255.255",
	},
	{
		.desc		 = "half empty",
		.insert		 = "0.0.0.0/0",
		.remove		 = "128.0.0.0/1",
		.count		 = (1UL << 31),
		.present	 = "0.0.0.0,127.255.255.255",
		.absent		 = "128.0.0.0,255.255.255.255",
	},
	{
		.desc		 = "single insertion",
		.insert		 = "172.16.23.42",
		.count		 = 1,
		.present	 = "172.16.23.42",
		.absent		 = "0.0.0.0,172.16.23.41,172.16.23.43,255.255.255.255",
	},
	{
		.desc		 = "single removal",
		.insert		 = "0.0.0.0/0",
		.remove		 = "172.16.23.42",
		.count		 = (1UL << 32) - 1,
		.present	 = "0.0.0.0,172.16.23.41,172.16.23.43,255.255.255.255",
		.absent		 = "172.16.23.42",
	},
	{
		.desc		 = "complete removal",
		.insert		 = "172.16.0.0/24",
		.remove		 = "172.16.0.0/25,172.16.0.128/25",
		.count		 = 0,
	},
	{
		.desc		 = "left removal",
		.insert		 = "172.16.23.0/24",
		.remove		 = "172.16.22.255-172.16.23.1",
		.count		 = 254,
		.present	 = "172.16.23.2-172.16.23.255",
		.absent		 = "172.16.23.0,172.16.23.1",
	},
	{
		.desc		 = "right removal",
		.insert		 = "172.16.23.0/24",
		.remove		 = "172.16.23.254-172.16.24.1",
		.count		 = 254,
		.present	 = "172.16.23.0-172.16.23.253",
		.absent		 = "172.16.23.254,172.16.23.255",
	},
	{
		.desc		 = "partial removal from leaf",
		.insert		 = "172.16.16.0/20",
		.remove		 = "172.16.23.0/24",
		.count		 = (1UL << 12) - (1UL << 8),
		.present	 = "172.16.16.0-172.16.22.255,"
					"172.16.24.0-172.16.31.255",
		.absent		 = "172.16.23.0-172.16.23.255",
	},
	{
		.desc		 = "unaligned insertion",
		.insert		 = "172.16.0.0/15",
		.count		 = (1UL << 17),
		.present	 = "172.16.0.0,172.17.255.255",
		.absent		 = "0.0.0.0,172.15.255.255,172.18.0.0,255.255.255.255",
	},
	{
		.desc		 = "unaligned removal",
		.insert		 = "0.0.0.0/0",
		.remove		 = "172.16.0.0/15",
		.count		 = (1UL << 32) - (1UL << 17),
		.present	 = "0.0.0.0,172.15.255.255,172.18.0.0,255.255.255.255",
		.absent		 = "172.16.0.0,172.17.255.255",
	},
	{
		.desc		 = "insert into full",
		.insert		 = "0.0.0.0/0,172.16.0.1/32",
		.count		 = (1UL << 32),
		.present	 = "172.16.0.1",
	},
	{
		.desc		 = "insert duplicate",
		.insert		 = "172.16.0.0/24,172.16.0.1/32",
		.count		 = (1UL << 8),
		.present	 = "172.16.0.0,172.16.0.255",
	},
	{
		.desc		 = "aggregate",
		.insert		 = "172.16.0.0/25,172.16.0.128/25",
		.count		 = (1UL << 8),
		.present	 = "172.16.0.0,172.16.0.255",
	},
};

static int
t_ip4s(char **desc CRYB_UNUSED, void *arg)
{
	struct t_ip4s_case *t = arg;
	ip4_addr first, last;
	const char *p, *q;
	ip4s_node *n;
	int ret;

	ret = 1;
	n = ip4s_new();
	if (!t_is_not_null(n))
		return (0);
	for (p = q = t->insert; q != NULL && *q != '\0'; p = q + 1) {
		q = ip4_parse_range(p, &first, &last);
		ft_assert(q != NULL && (*q == '\0' || *q == ','));
		if (ip4s_insert(n, be32toh(first.q), be32toh(last.q)) != 0)
			return (-1);
	}
	for (p = q = t->remove; q != NULL && *q != '\0'; p = q + 1) {
		q = ip4_parse_range(p, &first, &last);
		ft_assert(q != NULL && (*q == '\0' || *q == ','));
		if (ip4s_remove(n, be32toh(first.q), be32toh(last.q)) != 0)
			return (-1);
	}
	ret &= t_compare_ul(t->count, ip4s_count(n));
	for (p = q = t->present; q != NULL && *q != '\0'; p = q + 1) {
		q = ip4_parse_range(p, &first, &last);
		ft_assert(q != NULL && (*q == '\0' || *q == ','));
		ret &= t_ip4s_present(n, &first);
		ret &= t_ip4s_present(n, &last);
	}
	for (p = q = t->absent; q != NULL && *q != '\0'; p = q + 1) {
		q = ip4_parse_range(p, &first, &last);
		ft_assert(q != NULL && (*q == '\0' || *q == ','));
		ret &= t_ip4s_absent(n, &first);
		ret &= t_ip4s_absent(n, &last);
	}
	if (!ret && t_verbose)
		ip4s_fprint(stderr, n);
	ip4s_destroy(n);
	return (ret);
}

static int
t_prepare(int argc CRYB_UNUSED, char *argv[] CRYB_UNUSED)
{
	unsigned int i;

	for (i = 0; i < sizeof t_ip4s_cases / sizeof t_ip4s_cases[0]; ++i)
		t_add_test(t_ip4s, &t_ip4s_cases[i],
		    "%s", t_ip4s_cases[i].desc);
	return (0);
}

int
main(int argc, char *argv[])
{

	t_main(t_prepare, NULL, argc, argv);
}
