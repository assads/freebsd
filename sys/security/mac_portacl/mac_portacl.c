/*-
 * Copyright (c) 2003 Networks Associates Technology, Inc.
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Network
 * Associates Laboratories, the Security Research Division of Network
 * Associates, Inc. under DARPA/SPAWAR contract N66001-01-C-8035 ("CBOSS"),
 * as part of the DARPA CHATS research program.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
 *
 * $FreeBSD$
 */

/*
 * Developed by the TrustedBSD Project.
 *
 * Administratively limit access to local UDP/TCP ports for binding purposes.
 * Intended to be combined with net.inet.ip.portrange.reservedhigh to allow
 * specific uids and gids to bind specific ports for specific purposes,
 * while not opening the door to any user replacing an "official" service
 * while you're restarting it.  This only affects ports explicitly bound by
 * the user process (either for listen/outgoing socket for TCP, or send/
 * receive for UDP).  This module will not limit ports bound implicitly for
 * out-going connections where the process hasn't explicitly selected a port:
 * these are automatically selected by the IP stack.
 *
 * To use this module, security.mac.enforce_socket must be enabled, and
 * you will probably want to twiddle the net.inet sysctl listed above.
 * Then use sysctl(8) to modify the rules string:
 *
 * # sysctl security.mac.portacl.rules="uid:425:tcp:80,uid:425:tcp:79"
 *
 * This ruleset, for example, permits uid 425 to bind TCP ports 80 (http)
 * and 79 (finger).  User names and group names can't be used directly
 * because the kernel only knows about uids and gids.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/domain.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/mac.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/queue.h>
#include <sys/systm.h>
#include <sys/sysproto.h>
#include <sys/sysent.h>
#include <sys/vnode.h>
#include <sys/file.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/stdint.h>
#include <sys/sysctl.h>

#include <netinet/in.h>

#include <vm/vm.h>

#include <sys/mac_policy.h>

SYSCTL_DECL(_security_mac);

SYSCTL_NODE(_security_mac, OID_AUTO, portacl, CTLFLAG_RW, 0,
    "TrustedBSD mac_portacl policy controls");

static int	mac_portacl_enabled = 1;
SYSCTL_INT(_security_mac_portacl, OID_AUTO, enabled, CTLFLAG_RW,
    &mac_portacl_enabled, 0, "Enforce portacl policy");
TUNABLE_INT("security.mac.portacl.enabled", &mac_portacl_enabled);

static int	mac_portacl_suser_exempt = 1;
SYSCTL_INT(_security_mac_portacl, OID_AUTO, suser_exempt, CTLFLAG_RW,
    &mac_portacl_suser_exempt, 0, "Privilege permits binding of any port");
TUNABLE_INT("security.mac.portacl.suser_exempt",
    &mac_portacl_suser_exempt);

static int	mac_portacl_port_high = 1023;
SYSCTL_INT(_security_mac_portacl, OID_AUTO, port_high, CTLFLAG_RW,
    &mac_portacl_port_high, 0, "Highest port to enforce for");
TUNABLE_INT("security.mac.portacl.port_high", &mac_portacl_port_high);

MALLOC_DEFINE(M_PORTACL, "portacl rule", "Rules for mac_portacl");

#define	MAC_RULE_STRING_LEN	1024

#define	RULE_GID	1
#define	RULE_UID	2
#define	RULE_PROTO_TCP	1
#define	RULE_PROTO_UDP	2
struct rule {
	id_t			r_id;
	int			r_idtype;
	u_int16_t		r_port;
	int			r_protocol;

	TAILQ_ENTRY(rule)	r_entries;
};

#define	GID_STRING	"gid"
#define	TCP_STRING	"tcp"
#define	UID_STRING	"uid"
#define	UDP_STRING	"udp"

/*
 * Text format for the rule string is that a rule consists of a
 * comma-seperated list of elements.  Each element is in the form
 * idtype:id:protocol:portnumber, and constitutes granting of permission
 * for the specified binding.
 */

static struct sx			rule_sx;
static TAILQ_HEAD(rulehead, rule)	rule_head;
static char				rule_string[MAC_RULE_STRING_LEN];

static void
toast_rules(struct rulehead *head)
{
	struct rule *rule;
	int i;

	i = 0;
	for (rule = TAILQ_FIRST(head);
	     rule != NULL;
	     rule = TAILQ_NEXT(rule, r_entries))
		i++;

	while ((rule = TAILQ_FIRST(head)) != NULL) {
		TAILQ_REMOVE(head, rule, r_entries);
		free(rule, M_PORTACL);
	}
}

/*
 * Note that there is an inherent race condition in the unload of modules
 * and access via sysctl.
 */
static void
destroy(struct mac_policy_conf *mpc)
{

	sx_destroy(&rule_sx);
	toast_rules(&rule_head);
}

static void
init(struct mac_policy_conf *mpc)
{

	sx_init(&rule_sx, "rule_sx");
	TAILQ_INIT(&rule_head);
}

/*
 * Note: parsing routines are destructive on the passed string.
 */
static int
parse_rule_element(char *element, struct rule **rule)
{
	char *idtype, *id, *protocol, *portnumber, *p;
	struct rule *new;
	int error;

	error = 0;
	new = malloc(sizeof(*new), M_PORTACL, M_ZERO | M_WAITOK);

	idtype = strsep(&element, ":");
	if (idtype == NULL) {
		error = EINVAL;
		goto out;
	}
	id = strsep(&element, ":");
	if (id == NULL) {
		error = EINVAL;
		goto out;
	}
	new->r_id = strtol(id, &p, 10);
	if (*p != '\0') {
		error = EINVAL;
		goto out;
	}
	if (strcmp(idtype, UID_STRING) == 0)
		new->r_idtype = RULE_UID;
	else if (strcmp(idtype, GID_STRING) == 0)
		new->r_idtype = RULE_GID;
	else {
		error = EINVAL;
		goto out;
	}
	protocol = strsep(&element, ":");
	if (protocol == NULL) {
		error = EINVAL;
		goto out;
	}
	if (strcmp(protocol, TCP_STRING) == 0)
		new->r_protocol = RULE_PROTO_TCP;
	else if (strcmp(protocol, UDP_STRING) == 0)
		new->r_protocol = RULE_PROTO_UDP;
	else {
		error = EINVAL;
		goto out;
	}
	portnumber = element;
	if (portnumber == NULL) {
		error = EINVAL;
		goto out;
	}
	new->r_port = strtol(portnumber, &p, 10);
	if (*p != '\0') {
		error = EINVAL;
		goto out;
	}

out:
	if (error != 0) {
		free(new, M_PORTACL);
		*rule = NULL;
	} else
		*rule = new;
	return (error);
}

static int
parse_rules(char *string, struct rulehead *head)
{
	struct rule *new;
	char *element;
	int error;

	error = 0;
	while ((element = strsep(&string, ",")) != NULL) {
		if (strlen(element) == 0)
			continue;
		error = parse_rule_element(element, &new);
		if (error)
			goto out;
		TAILQ_INSERT_TAIL(head, new, r_entries);
	}
out:
	if (error != 0)
		toast_rules(head);
	return (error);
}

#if 0
static void
rule_printf(struct sbuf *sb, struct rule *rule)
{
	const char *idtype, *protocol;

	switch(rule->r_idtype) {
	case RULE_GID:
		idtype = GID_STRING;
		break;
	case RULE_UID:
		idtype = UID_STRING;
		break;
	default:
		panic("rule_printf: unknown idtype (%d)\n", rule->r_idtype);
	}

	switch (rule->r_protocol) {
	case RULE_PROTO_TCP:
		protocol = TCP_STRING;
		break;
	case RULE_PROTO_UDP:
		protocol = UDP_STRING;
		break;
	default:
		panic("rule_printf: unknown protocol (%d)\n",
		    rule->r_protocol);
	}
	sbuf_printf(sb, "%s:%jd:%s:%d", idtype, (intmax_t)rule->r_id,
	    protocol, rule->r_port);
}

static char *
rules_to_string(void)
{
	struct rule *rule;
	struct sbuf *sb;
	int needcomma;
	char *temp;

	sb = sbuf_new(NULL, NULL, 0, SBUF_AUTOEXTEND);
	needcomma = 0;
	sx_slock(&rule_sx);
	for (rule = TAILQ_FIRST(&rule_head); rule != NULL;
	    rule = TAILQ_NEXT(rule, r_entries)) {
		if (!needcomma)
			needcomma = 1;
		else
			sbuf_printf(sb, ",");
		rule_printf(sb, rule);
	}
	sx_sunlock(&rule_sx);
	sbuf_finish(sb);
	temp = strdup(sbuf_data(sb), M_PORTACL);
	sbuf_delete(sb);
	return (temp);
}
#endif

/*
 * Note: due to races, there is not a single serializable order
 * between parallel calls to the sysctl.
 */
static int
sysctl_rules(SYSCTL_HANDLER_ARGS)
{
	char *string, *copy_string, *new_string;
	struct rulehead head, save_head;
	struct rule *rule;
	int error;

	new_string = NULL;
	if (req->newptr == NULL) {
		new_string = malloc(MAC_RULE_STRING_LEN, M_PORTACL,
		    M_WAITOK | M_ZERO);
		strcpy(new_string, rule_string);
		string = new_string;
	} else
		string = rule_string;

	error = sysctl_handle_string(oidp, string, MAC_RULE_STRING_LEN, req);
	if (error)
		goto out;

	if (req->newptr != NULL) {
		copy_string = strdup(string, M_PORTACL);
		TAILQ_INIT(&head);
		error = parse_rules(copy_string, &head);
		free(copy_string, M_PORTACL);
		if (error)
			goto out;

		TAILQ_INIT(&save_head);
		sx_xlock(&rule_sx);
		/*
		 * XXX: Unfortunately, TAILQ doesn't yet have a supported
		 * assignment operator to copy one queue to another, due
	 	 * to a self-referential pointer in the tailq header.
		 * For now, do it the old-fashioned way.
		 */
		while ((rule = TAILQ_FIRST(&rule_head)) != NULL) {
			TAILQ_REMOVE(&rule_head, rule, r_entries);
			TAILQ_INSERT_HEAD(&save_head, rule, r_entries);
		}
		while ((rule = TAILQ_FIRST(&head)) != NULL) {
			TAILQ_REMOVE(&head, rule, r_entries);
			TAILQ_INSERT_HEAD(&rule_head, rule, r_entries);
		}
		strcpy(rule_string, string);
		sx_xunlock(&rule_sx);
		toast_rules(&save_head);
	}
out:
	if (new_string != NULL)
		free(new_string, M_PORTACL);
	return (error);
}

SYSCTL_PROC(_security_mac_portacl, OID_AUTO, rules,
       CTLTYPE_STRING|CTLFLAG_RW, 0, 0, sysctl_rules, "A", "Rules");

static int
rules_check(struct ucred *cred, int family, int type, u_int16_t port)
{
	struct rule *rule;
	int error;

#if 0
	printf("Check requested for euid %d, family %d, type %d, port %d\n",
	    cred->cr_uid, family, type, port);
#endif

	if (port > mac_portacl_port_high)
		return (0);

	error = EPERM;
	sx_slock(&rule_sx);
	for (rule = TAILQ_FIRST(&rule_head);
	    rule != NULL;
	    rule = TAILQ_NEXT(rule, r_entries)) {
		if (type == SOCK_DGRAM && rule->r_protocol != RULE_PROTO_UDP)
			continue;
		if (type == SOCK_STREAM && rule->r_protocol != RULE_PROTO_TCP)
			continue;
		if (port != rule->r_port)
			continue;
		if (rule->r_idtype == RULE_UID) {
			if (cred->cr_uid == rule->r_id) {
				error = 0;
				break;
			}
		} else if (rule->r_idtype == RULE_GID) {
			if (cred->cr_gid == rule->r_id) {
				error = 0;
				break;
			} else if (groupmember(rule->r_id, cred)) {
				error = 0;
				break;
			}
		} else
			panic("rules_check: unknown rule type %d",
			    rule->r_idtype);
	}
	sx_sunlock(&rule_sx);

	if (error != 0 && mac_portacl_suser_exempt != 0)
		error = suser_cred(cred, 0);

	return (error);
}

/*
 * Note, this only limits the ability to explicitly bind a port, it
 * doesn't limit implicitly bound ports for outgoing connections where
 * the source port is left up to the IP stack to determine automatically.
 */
static int
check_socket_bind(struct ucred *cred, struct socket *so,
    struct label *socketlabel, struct sockaddr *sockaddr)
{
	struct sockaddr_in *sin;
	int family, type;
	u_int16_t port;

	/* Only interested in IPv4 and IPv6 sockets. */
	if (so->so_proto->pr_domain->dom_family != PF_INET &&
	    so->so_proto->pr_domain->dom_family != PF_INET6)
		return (0);

	/* Currently, we don't attempt to deal with SOCK_RAW, etc. */
	if (so->so_type != SOCK_DGRAM &&
	    so->so_type != SOCK_STREAM)
		return (0);

	/* Reject addresses we don't understand; fail closed. */
	if (sockaddr->sa_family != AF_INET &&
	    sockaddr->sa_family != AF_INET6)
		return (EINVAL);

	family = so->so_proto->pr_domain->dom_family;
	type = so->so_type;
	sin = (struct sockaddr_in *) sockaddr;
	port = ntohs(sin->sin_port);

	return (rules_check(cred, family, type, port));
}

static struct mac_policy_ops mac_portacl_ops =
{
	.mpo_destroy = destroy,
	.mpo_init = init,
	.mpo_check_socket_bind = check_socket_bind,
};

MAC_POLICY_SET(&mac_portacl_ops, trustedbsd_mac_portacl,
    "TrustedBSD MAC/portacl", MPC_LOADTIME_FLAG_UNLOADOK, NULL);
