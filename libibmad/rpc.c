/*
 * Copyright (c) 2004-2009 Voltaire Inc.  All rights reserved.
 * Copyright (c) 2009 HNR Consulting.  All rights reserved.
 * Copyright (c) 2011 Mellanox Technologies LTD.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <infiniband/umad.h>
#include <infiniband/mad.h>

#include "mad_internal.h"

int ibdebug;

static struct ibmad_port mad_port;
struct ibmad_port *ibmp = &mad_port;

static int iberrs;

int madrpc_retries = MAD_DEF_RETRIES;
int madrpc_timeout = MAD_DEF_TIMEOUT_MS;

static void *save_mad;
static int save_mad_len = 256;

#undef DEBUG
#define DEBUG	if (ibdebug)	IBWARN
#define ERRS(fmt, ...) do {	\
	if (iberrs || ibdebug)	\
		IBWARN(fmt, ## __VA_ARGS__); \
} while (0)

#define MAD_TID(mad)	(*((uint64_t *)((char *)(mad) + 8)))

void madrpc_show_errors(int set)
{
	iberrs = set;
}

void madrpc_save_mad(void *madbuf, int len)
{
	save_mad = madbuf;
	save_mad_len = len;
}

int madrpc_set_retries(int retries)
{
	if (retries > 0)
		madrpc_retries = retries;
	return madrpc_retries;
}

int madrpc_set_timeout(int timeout)
{
	madrpc_timeout = timeout;
	return 0;
}

void mad_rpc_set_retries(struct ibmad_port *port, int retries)
{
	port->retries = retries;
}

void mad_rpc_set_timeout(struct ibmad_port *port, int timeout)
{
	port->timeout = timeout;
}

int madrpc_portid(void)
{
	return ibmp->port_id;
}

int mad_rpc_portid(struct ibmad_port *srcport)
{
	return srcport->port_id;
}

int mad_rpc_class_agent(struct ibmad_port *port, int class)
{
	if (class < 1 || class >= MAX_CLASS)
		return -1;
	return port->class_agents[class];
}

static int
_do_madrpc(int port_id, void *sndbuf, void *rcvbuf, int agentid, int len,
	   int timeout, int max_retries, int *p_error)
{
	uint32_t trid;		/* only low 32 bits - see mad_trid() */
	int retries;
	int length, status;

	if (ibdebug > 1) {
		IBWARN(">>> sending: len %d pktsz %zu", len, umad_size() + len);
		xdump(stderr, "send buf\n", sndbuf, umad_size() + len);
	}

	if (save_mad) {
		memcpy(save_mad, umad_get_mad(sndbuf),
		       save_mad_len < len ? save_mad_len : len);
		save_mad = NULL;
	}

	if (max_retries <= 0) {
		errno = EINVAL;
		*p_error = EINVAL;
		ERRS("max_retries %d <= 0", max_retries);
		return -1;
	}

	trid =
	    (uint32_t) mad_get_field64(umad_get_mad(sndbuf), 0, IB_MAD_TRID_F);

	for (retries = 0; retries < max_retries; retries++) {
		if (retries)
			ERRS("retry %d (timeout %d ms)", retries, timeout);

		length = len;
		if (umad_send(port_id, agentid, sndbuf, length, timeout, 0) < 0) {
			IBWARN("send failed; %s", strerror(errno));
			return -1;
		}

		/* Use same timeout on receive side just in case */
		/* send packet is lost somewhere. */
		do {
			length = len;
			if (umad_recv(port_id, rcvbuf, &length, timeout) < 0) {
				IBWARN("recv failed: %s", strerror(errno));
				return -1;
			}

			if (ibdebug > 2)
				umad_addr_dump(umad_get_mad_addr(rcvbuf));
			if (ibdebug > 1) {
				IBWARN("rcv buf:");
				xdump(stderr, "rcv buf\n", umad_get_mad(rcvbuf),
				      IB_MAD_SIZE);
			}
		} while ((uint32_t)
			 mad_get_field64(umad_get_mad(rcvbuf), 0,
					 IB_MAD_TRID_F) != trid);

		status = umad_status(rcvbuf);
		if (!status)
			return length;	/* done */
		if (status == ENOMEM)
			return length;
	}

	errno = status;
	*p_error = ETIMEDOUT;
	ERRS("timeout after %d retries, %d ms", retries, timeout * retries);
	return -1;
}

static int redirect_port(ib_portid_t * port, uint8_t * mad)
{
	port->lid = mad_get_field(mad, 64, IB_CPI_REDIRECT_LID_F);
	if (!port->lid) {
		IBWARN("GID-based redirection is not supported");
		return -1;
	}

	port->qp = mad_get_field(mad, 64, IB_CPI_REDIRECT_QP_F);
	port->qkey = mad_get_field(mad, 64, IB_CPI_REDIRECT_QKEY_F);
	port->sl = (uint8_t) mad_get_field(mad, 64, IB_CPI_REDIRECT_SL_F);

	/* TODO: Reverse map redirection P_Key to P_Key index */

	if (ibdebug)
		IBWARN("redirected to lid %d, qp 0x%x, qkey 0x%x, sl 0x%x",
		       port->lid, port->qp, port->qkey, port->sl);

	return 0;
}

void *mad_rpc(const struct ibmad_port *port, ib_rpc_t * rpc,
	      ib_portid_t * dport, void *payload, void *rcvdata)
{
	int status, len;
	uint8_t sndbuf[1024], rcvbuf[1024], *mad;
	ib_rpc_v1_t *rpcv1 = (ib_rpc_v1_t *)rpc;
	int error = 0;

	if ((rpc->mgtclass & IB_MAD_RPC_VERSION_MASK) == IB_MAD_RPC_VERSION1)
		rpcv1->error = 0;
	do {
		len = 0;
		memset(sndbuf, 0, umad_size() + IB_MAD_SIZE);

		if ((len = mad_build_pkt(sndbuf, rpc, dport, NULL, payload)) < 0)
			return NULL;

		if ((len = _do_madrpc(port->port_id, sndbuf, rcvbuf,
				      port->class_agents[rpc->mgtclass & 0xff],
				      len, mad_get_timeout(port, rpc->timeout),
				      mad_get_retries(port), &error)) < 0) {
			if ((rpc->mgtclass & IB_MAD_RPC_VERSION_MASK) ==
			    IB_MAD_RPC_VERSION1)
				rpcv1->error = error;
			IBWARN("_do_madrpc failed; dport (%s)",
			       portid2str(dport));
			return NULL;
		}

		mad = umad_get_mad(rcvbuf);
		status = mad_get_field(mad, 0, IB_DRSMP_STATUS_F);

		/* check for exact match instead of only the redirect bit;
		 * that way, weird statuses cause an error, too */
		if (status == IB_MAD_STS_REDIRECT) {
			/* update dport for next request and retry */
			/* bail if redirection fails */
			if (redirect_port(dport, mad))
				break;
		} else
			break;
	} while (1);

	if ((rpc->mgtclass & IB_MAD_RPC_VERSION_MASK) == IB_MAD_RPC_VERSION1)
		rpcv1->error = error;
	rpc->rstatus = status;

	if (status != 0) {
		ERRS("MAD completed with error status 0x%x; dport (%s)",
		     status, portid2str(dport));
		errno = EIO;
		return NULL;
	}

	if (rcvdata)
		memcpy(rcvdata, mad + rpc->dataoffs, rpc->datasz);

	return rcvdata;
}

void *mad_rpc_rmpp(const struct ibmad_port *port, ib_rpc_t * rpc,
		   ib_portid_t * dport, ib_rmpp_hdr_t * rmpp, void *data)
{
	int status, len;
	uint8_t sndbuf[1024], rcvbuf[1024], *mad;
	ib_rpc_v1_t *rpcv1 = (ib_rpc_v1_t *)rpc;
	int error = 0;

	memset(sndbuf, 0, umad_size() + IB_MAD_SIZE);

	DEBUG("rmpp %p data %p", rmpp, data);

	if ((rpc->mgtclass & IB_MAD_RPC_VERSION_MASK) == IB_MAD_RPC_VERSION1)
		rpcv1->error = 0;
	if ((len = mad_build_pkt(sndbuf, rpc, dport, rmpp, data)) < 0)
		return NULL;

	if ((len = _do_madrpc(port->port_id, sndbuf, rcvbuf,
			      port->class_agents[rpc->mgtclass & 0xff],
			      len, mad_get_timeout(port, rpc->timeout),
			      mad_get_retries(port), &error)) < 0) {
		if ((rpc->mgtclass & IB_MAD_RPC_VERSION_MASK) == IB_MAD_RPC_VERSION1)
			rpcv1->error = error;
		IBWARN("_do_madrpc failed; dport (%s)", portid2str(dport));
		return NULL;
	}

	if ((rpc->mgtclass & IB_MAD_RPC_VERSION_MASK) == IB_MAD_RPC_VERSION1)
		rpcv1->error = error;

	mad = umad_get_mad(rcvbuf);

	if ((status = mad_get_field(mad, 0, IB_MAD_STATUS_F)) != 0) {
		ERRS("MAD completed with error status 0x%x; dport (%s)",
		     status, portid2str(dport));
		errno = EIO;
		return NULL;
	}

	if (rmpp) {
		rmpp->flags = mad_get_field(mad, 0, IB_SA_RMPP_FLAGS_F);
		if ((rmpp->flags & 0x3) &&
		    mad_get_field(mad, 0, IB_SA_RMPP_VERS_F) != 1) {
			IBWARN("bad rmpp version");
			return NULL;
		}
		rmpp->type = mad_get_field(mad, 0, IB_SA_RMPP_TYPE_F);
		rmpp->status = mad_get_field(mad, 0, IB_SA_RMPP_STATUS_F);
		DEBUG("rmpp type %d status %d", rmpp->type, rmpp->status);
		rmpp->d1.u = mad_get_field(mad, 0, IB_SA_RMPP_D1_F);
		rmpp->d2.u = mad_get_field(mad, 0, IB_SA_RMPP_D2_F);
	}

	if (data)
		memcpy(data, mad + rpc->dataoffs, rpc->datasz);

	rpc->recsz = mad_get_field(mad, 0, IB_SA_ATTROFFS_F);

	return data;
}

void *madrpc(ib_rpc_t * rpc, ib_portid_t * dport, void *payload, void *rcvdata)
{
	return mad_rpc(ibmp, rpc, dport, payload, rcvdata);
}

void *madrpc_rmpp(ib_rpc_t * rpc, ib_portid_t * dport, ib_rmpp_hdr_t * rmpp,
		  void *data)
{
	return mad_rpc_rmpp(ibmp, rpc, dport, rmpp, data);
}

void
madrpc_init(char *dev_name, int dev_port, int *mgmt_classes, int num_classes)
{
	int fd;

	if (umad_init() < 0)
		IBPANIC("can't init UMAD library");

	if ((fd = umad_open_port(dev_name, dev_port)) < 0)
		IBPANIC("can't open UMAD port (%s:%d)",
		dev_name ? dev_name : "(nil)", dev_port);

	if (num_classes >= MAX_CLASS)
		IBPANIC("too many classes %d requested", num_classes);

	ibmp->port_id = fd;
	memset(ibmp->class_agents, 0xff, sizeof ibmp->class_agents);
	while (num_classes--) {
		uint8_t rmpp_version = 0;
		int mgmt = *mgmt_classes++;

		if (mgmt == IB_SA_CLASS)
			rmpp_version = 1;
		if (mad_register_client_via(mgmt, rmpp_version, ibmp) < 0)
			IBPANIC("client_register for mgmt class %d failed",
				mgmt);
	}
}

struct ibmad_port *mad_rpc_open_port(char *dev_name, int dev_port,
				     int *mgmt_classes, int num_classes)
{
	struct ibmad_port *p;
	int port_id;
	char *debug_level_env;

	if (num_classes >= MAX_CLASS) {
		IBWARN("too many classes %d requested", num_classes);
		errno = EINVAL;
		return NULL;
	}

	if (umad_init() < 0) {
		IBWARN("can't init UMAD library");
		errno = ENODEV;
		return NULL;
	}

	debug_level_env = getenv("LIBIBMAD_DEBUG_LEVEL");
	if (debug_level_env) {
		ibdebug = atoi(debug_level_env);
	}

	p = malloc(sizeof(*p));
	if (!p) {
		errno = ENOMEM;
		return NULL;
	}
	memset(p, 0, sizeof(*p));

	if ((port_id = umad_open_port(dev_name, dev_port)) < 0) {
		IBWARN("can't open UMAD port (%s:%d)", dev_name, dev_port);
		if (!errno)
			errno = EIO;
		free(p);
		return NULL;
	}

	p->port_id = port_id;
	memset(p->class_agents, 0xff, sizeof p->class_agents);
	while (num_classes--) {
		uint8_t rmpp_version = 0;
		int mgmt = *mgmt_classes++;

		if (mgmt == IB_SA_CLASS)
			rmpp_version = 1;
		if (mgmt < 0 || mgmt >= MAX_CLASS ||
		    mad_register_client_via(mgmt, rmpp_version, p) < 0) {
			IBWARN("client_register for mgmt %d failed", mgmt);
			if (!errno)
				errno = EINVAL;
			umad_close_port(port_id);
			free(p);
			return NULL;
		}
	}

	return p;
}

void mad_rpc_close_port(struct ibmad_port *port)
{
	umad_close_port(port->port_id);
	free(port);
}

static int get_smi_gsi_pair(const char *ca_name, int portnum, struct ibmad_ports_pair *ports_pair,
			    unsigned enforce_smi)
{
	struct umad_ca_pair ca_pair;
	int smi_port_id = -1;
	int gsi_port_id = -1;
	int rc = -1;
	rc = umad_get_smi_gsi_pair_by_ca_name(ca_name, portnum, &ca_pair, enforce_smi);

	if (rc < 0) {
		IBWARN("Can't open UMAD port (%s) (%s:%d)", strerror(-rc), ca_name, portnum);
		return rc;
	}
	smi_port_id = umad_open_port(ca_pair.smi_name, ca_pair.smi_preferred_port);

	if (smi_port_id < 0 && enforce_smi) {
		IBWARN("Can't open SMI UMAD port (%s) (%s:%d)", strerror(-smi_port_id), ca_pair.smi_name, ca_pair.smi_preferred_port);
		return smi_port_id;
	}

	gsi_port_id = umad_open_port(ca_pair.gsi_name, ca_pair.gsi_preferred_port);

	if (gsi_port_id < 0) {
		IBWARN("Can't open GSI UMAD port (%s) (%s:%d)", strerror(-gsi_port_id), ca_pair.gsi_name, ca_pair.gsi_preferred_port);
		umad_close_port(smi_port_id);
		return gsi_port_id;
	}

	ports_pair->smi.port->port_id = smi_port_id;
	ports_pair->gsi.port->port_id = gsi_port_id;
	strncpy(ports_pair->smi.ca_name, ca_pair.smi_name, UMAD_CA_NAME_LEN);
	strncpy(ports_pair->gsi.ca_name, ca_pair.gsi_name, UMAD_CA_NAME_LEN);

	return 0;
}

static struct ibmad_port *get_port_for_class(struct ibmad_ports_pair *ports_pair, int mgmt_class, unsigned enforce_smi)
{
	if (mgmt_class != IB_SMI_CLASS && mgmt_class != IB_SMI_DIRECT_CLASS) {
		if (ports_pair->gsi.port->port_id < 0) {
			IBWARN("required port for GSI is invalid");
			return NULL;
		}
		return ports_pair->gsi.port;
	}

	if (ports_pair->smi.port->port_id < 0) {
		if (enforce_smi) {
			IBWARN("required port for SMI is invalid");
			return NULL;
		}
		return ports_pair->gsi.port;
	}

	return ports_pair->smi.port;
}

struct ibmad_ports_pair *mad_rpc_open_port2(char *dev_name, int dev_port,
				     int *mgmt_classes, int num_classes, unsigned enforce_smi)
{
	struct ibmad_port *smi;
	struct ibmad_port *gsi;
	struct ibmad_ports_pair *ports_pair;
	char *debug_level_env;

	if (num_classes >= MAX_CLASS) {
		IBWARN("too many classes %d requested", num_classes);
		errno = EINVAL;
		return NULL;
	}

	if (umad_init() < 0) {
		IBWARN("can't init UMAD library");
		errno = ENODEV;
		return NULL;
	}

	debug_level_env = getenv("LIBIBMAD_DEBUG_LEVEL");
	if (debug_level_env) {
		ibdebug = atoi(debug_level_env);
	}

	smi = malloc(sizeof(*smi));
	if (!smi) {
		errno = ENOMEM;
		goto smi_error;
	}
	memset(smi, 0, sizeof(*smi));

	gsi = malloc(sizeof(*gsi));
	if (!gsi) {
		errno = ENOMEM;
		goto gsi_error;
	}
	memset(gsi, 0, sizeof(*gsi));

	ports_pair = malloc(sizeof(*ports_pair));
	if (!ports_pair) {
		errno = ENOMEM;
		goto ports_pair_error;
	}
	memset(ports_pair, 0, sizeof(*ports_pair));
	ports_pair->smi.port = smi;
	ports_pair->gsi.port = gsi;

	if ((get_smi_gsi_pair(dev_name, dev_port, ports_pair, enforce_smi)) < 0) {
		IBWARN("can't open UMAD port (%s:%d)", dev_name, dev_port);
		if (!errno)
			errno = EIO;
		goto get_smi_error;
	}

	memset(ports_pair->smi.port->class_agents, 0xff, sizeof ports_pair->smi.port->class_agents);
	memset(ports_pair->gsi.port->class_agents, 0xff, sizeof ports_pair->gsi.port->class_agents);
	while (num_classes--) {
		uint8_t rmpp_version = 0;
		int mgmt = *mgmt_classes++;
		struct ibmad_port *p = get_port_for_class(ports_pair, mgmt, enforce_smi);
		if (mgmt == IB_SA_CLASS)
			rmpp_version = 1;
		if (mgmt < 0 || mgmt >= MAX_CLASS || !p ||
		    mad_register_client_via(mgmt, rmpp_version, p) < 0) {
			IBWARN("client_register for mgmt %d failed", mgmt);
			if (!errno)
				errno = EINVAL;
			goto mad_reg_error;
		}
	}

	return ports_pair;

 mad_reg_error:
	umad_close_port(ports_pair->smi.port->port_id);
	umad_close_port(ports_pair->gsi.port->port_id);
 get_smi_error:
	free(ports_pair);
 ports_pair_error:
	free(gsi);
 gsi_error:
	free(smi);
 smi_error:
	return NULL;
}

void mad_rpc_close_port2(struct ibmad_ports_pair *srcport)
{
	umad_close_port(srcport->smi.port->port_id);
	umad_close_port(srcport->gsi.port->port_id);
	free(srcport->smi.port);
	free(srcport->gsi.port);
	free(srcport);
}
