/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2019  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

#include "LinuxNetLink.hpp"

#include <unistd.h>
#include <linux/if_tun.h>

namespace ZeroTier {

struct nl_route_req {
	struct nlmsghdr nl;
	struct rtmsg rt;
	char buf[8192];
};

struct nl_if_req {
	struct nlmsghdr nl;
	struct ifinfomsg ifa;
	char buf[8192];
};

struct nl_adr_req {
	struct nlmsghdr nl;
	struct ifaddrmsg ifa;
	char buf[8192];
};

LinuxNetLink::LinuxNetLink()
	: _t()
	, _running(false)
	, _routes_ipv4()
	, _rv4_m()
	, _routes_ipv6()
	, _rv6_m()
	, _seq(0)
	, _interfaces()
	, _if_m()
	, _fd(socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE))
	, _la({0})
{
	// set socket timeout to 1 sec so we're not permablocking recv() calls
	_setSocketTimeout(_fd, 1);

	_la.nl_family = AF_NETLINK;
	_la.nl_pid = getpid()+1;
	_la.nl_groups = RTMGRP_LINK|RTMGRP_IPV4_IFADDR|RTMGRP_IPV6_IFADDR|RTMGRP_IPV4_ROUTE|RTMGRP_IPV6_ROUTE|RTMGRP_NOTIFY;
	if (bind(_fd, (struct sockaddr*)&_la, sizeof(_la))) {
		fprintf(stderr, "Error connecting to RTNETLINK: %s\n", strerror(errno));
		::exit(1);
	}

	_requestIPv4Routes();
	_requestIPv6Routes();
	_requestInterfaceList();

	_running = true;
	_t = Thread::start(this);
}

LinuxNetLink::~LinuxNetLink()
{
	_running = false;
	Thread::join(_t);
	::close(_fd);
}

void LinuxNetLink::_setSocketTimeout(int fd, int seconds) 
{
	struct timeval tv;
	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) != 0) {
#ifdef ZT_TRACE
		fprintf(stderr, "setsockopt failed: %s\n", strerror(errno));
#endif
	}
}

#define ZT_NL_BUF_SIZE 16384
int LinuxNetLink::_doRecv(int fd)
{
	char *const buf = (char *)valloc(ZT_NL_BUF_SIZE);
	if (!buf) {
		fprintf(stderr,"malloc failed!\n");
		::exit(1);
	}

	char *p = NULL;
	struct nlmsghdr *nlp;
	int nll = 0;
	int rtn = 0;
	p = buf;

	for(;;) {
		rtn = recv(fd, p, ZT_NL_BUF_SIZE - nll, 0);

		if (rtn > 0) {
			nlp = (struct nlmsghdr *)p;

			if(nlp->nlmsg_type == NLMSG_ERROR && (nlp->nlmsg_flags & NLM_F_ACK) != NLM_F_ACK) {
				struct nlmsgerr *err = (struct nlmsgerr*)NLMSG_DATA(nlp);
				if (err->error != 0) {
#ifdef ZT_TRACE
					//fprintf(stderr, "rtnetlink error: %s\n", strerror(-(err->error)));
#endif
				}
				p = buf;
				nll = 0;
				break;
			}

			if (nlp->nlmsg_type == NLMSG_NOOP) {
				break;
			}

			if( (nlp->nlmsg_flags & NLM_F_MULTI) == NLM_F_MULTI || (nlp->nlmsg_type == NLMSG_DONE))
			{
				if (nlp->nlmsg_type == NLMSG_DONE) {
					_processMessage(nlp, nll);
					p = buf;
					nll = 0;
					break;
				}
				p += rtn;
				nll += rtn;
			}

			if (nlp->nlmsg_type == NLMSG_OVERRUN) {
//#ifdef ZT_TRACE
				fprintf(stderr, "NLMSG_OVERRUN: Data lost\n");
//#endif
				p = buf;
				nll = 0;
				break;
			}
			
			nll += rtn;

			_processMessage(nlp, nll);

			p = buf;
			nll = 0;
			break;
		} else {
			break;
		}
	}

	free(buf);

	return rtn;
}

void LinuxNetLink::threadMain() throw()
{
	int rtn = 0;

	while(_running) {
		rtn = _doRecv(_fd);
		if (rtn <= 0) {
			Thread::sleep(100);
			continue;
		}
	}
}

void LinuxNetLink::_processMessage(struct nlmsghdr *nlp, int nll)
{
	for(; NLMSG_OK(nlp, nll); nlp=NLMSG_NEXT(nlp, nll))
	{
		switch(nlp->nlmsg_type) 
		{
		case RTM_NEWLINK:
			_linkAdded(nlp);
			break;
		case RTM_DELLINK:
			_linkDeleted(nlp);
			break;
		case RTM_NEWADDR:
			_ipAddressAdded(nlp);
			break;
		case RTM_DELADDR:
			_ipAddressDeleted(nlp);
			break;
		case RTM_NEWROUTE:
			_routeAdded(nlp);
			break;
		case RTM_DELROUTE:
			_routeDeleted(nlp);
			break;
		default:
			break;
		}
	}
}

void LinuxNetLink::_ipAddressAdded(struct nlmsghdr *nlp)
{
	struct ifaddrmsg *ifap = (struct ifaddrmsg *)NLMSG_DATA(nlp);
	struct rtattr *rtap = (struct rtattr *)IFA_RTA(ifap);
	int ifal = IFA_PAYLOAD(nlp);

	char addr[40] = {0};
	char local[40] = {0};
	char label[40] = {0};
	char bcast[40] = {0};
	
	for(;RTA_OK(rtap, ifal); rtap=RTA_NEXT(rtap,ifal))
	{
		switch(rtap->rta_type) {
		case IFA_ADDRESS:
			inet_ntop(ifap->ifa_family, RTA_DATA(rtap), addr, 40);
			break;
		case IFA_LOCAL:
			inet_ntop(ifap->ifa_family, RTA_DATA(rtap), local, 40);
			break;
		case IFA_LABEL:
			memcpy(label, RTA_DATA(rtap), 40);
			break;
		case IFA_BROADCAST:
			inet_ntop(ifap->ifa_family, RTA_DATA(rtap), bcast, 40);
			break;
		}
	}

#ifdef ZT_TRACE
	//fprintf(stderr,"Added IP Address %s local: %s label: %s broadcast: %s\n", addr, local, label, bcast);
#endif
}

void LinuxNetLink::_ipAddressDeleted(struct nlmsghdr *nlp)
{
	struct ifaddrmsg *ifap = (struct ifaddrmsg *)NLMSG_DATA(nlp);
	struct rtattr *rtap = (struct rtattr *)IFA_RTA(ifap);
	int ifal = IFA_PAYLOAD(nlp);

	char addr[40] = {0};
	char local[40] = {0};
	char label[40] = {0};
	char bcast[40] = {0};

	for(;RTA_OK(rtap, ifal); rtap=RTA_NEXT(rtap,ifal))
	{
		switch(rtap->rta_type) {
		case IFA_ADDRESS:
			inet_ntop(ifap->ifa_family, RTA_DATA(rtap), addr, 40);
			break;
		case IFA_LOCAL:
			inet_ntop(ifap->ifa_family, RTA_DATA(rtap), local, 40);
			break;
		case IFA_LABEL:
			memcpy(label, RTA_DATA(rtap), 40);
			break;
		case IFA_BROADCAST:
			inet_ntop(ifap->ifa_family, RTA_DATA(rtap), bcast, 40);
			break;
		}
	}

#ifdef ZT_TRACE
	//fprintf(stderr, "Removed IP Address %s local: %s label: %s broadcast: %s\n", addr, local, label, bcast);
#endif
}

void LinuxNetLink::_routeAdded(struct nlmsghdr *nlp)
{
	char dsts[40] = {0};
	char gws[40] = {0};
	char srcs[40] = {0};
	char ifs[16] = {0};
	char ms[24] = {0};

	struct rtmsg *rtp = (struct rtmsg *)NLMSG_DATA(nlp);
	struct rtattr *rtap = (struct rtattr *)RTM_RTA(rtp);
	int rtl = RTM_PAYLOAD(nlp);

	for(;RTA_OK(rtap, rtl); rtap=RTA_NEXT(rtap, rtl))
	{
		switch(rtap->rta_type)
		{
		case RTA_DST:
			inet_ntop(rtp->rtm_family, RTA_DATA(rtap), dsts, rtp->rtm_family == AF_INET ? 24 : 40);
			break;
		case RTA_SRC:
			inet_ntop(rtp->rtm_family, RTA_DATA(rtap), srcs, rtp->rtm_family == AF_INET ? 24: 40);
			break;
		case RTA_GATEWAY:
			inet_ntop(rtp->rtm_family, RTA_DATA(rtap), gws, rtp->rtm_family == AF_INET ? 24 : 40);
			break;
		case RTA_OIF:
			sprintf(ifs, "%d", *((int*)RTA_DATA(rtap)));
			break;
		}
	}
	sprintf(ms, "%d", rtp->rtm_dst_len);

#ifdef ZT_TRACE
	//fprintf(stderr, "Route Added: dst %s/%s gw %s src %s if %s\n", dsts, ms, gws, srcs, ifs);
#endif
}

void LinuxNetLink::_routeDeleted(struct nlmsghdr *nlp)
{
	char dsts[40] = {0};
	char gws[40] = {0};
	char srcs[40] = {0};
	char ifs[16] = {0};
	char ms[24] = {0};

	struct rtmsg *rtp = (struct rtmsg *) NLMSG_DATA(nlp);
	struct rtattr *rtap = (struct rtattr *)RTM_RTA(rtp);
	int rtl = RTM_PAYLOAD(nlp);

	for(;RTA_OK(rtap, rtl); rtap=RTA_NEXT(rtap, rtl))
	{
		switch(rtap->rta_type)
		{
		case RTA_DST:
			inet_ntop(rtp->rtm_family, RTA_DATA(rtap), dsts, rtp->rtm_family == AF_INET ? 24 : 40);
			break;
		case RTA_SRC:
			inet_ntop(rtp->rtm_family, RTA_DATA(rtap), srcs, rtp->rtm_family == AF_INET ? 24 : 40);
			break;
		case RTA_GATEWAY:
			inet_ntop(rtp->rtm_family, RTA_DATA(rtap), gws, rtp->rtm_family == AF_INET ? 24 : 40);
			break;
		case RTA_OIF:
			sprintf(ifs, "%d", *((int*)RTA_DATA(rtap)));
			break;
		}
	}
	sprintf(ms, "%d", rtp->rtm_dst_len);

#ifdef ZT_TRACE
	//fprintf(stderr, "Route Deleted: dst %s/%s gw %s src %s if %s\n", dsts, ms, gws, srcs, ifs);
#endif
}

void LinuxNetLink::_linkAdded(struct nlmsghdr *nlp)
{
	unsigned char mac_bin[6] = {0};
	unsigned int mtu = 0;
	char ifname[IFNAMSIZ] = {0};

	struct ifinfomsg *ifip = (struct ifinfomsg *)NLMSG_DATA(nlp);
	struct rtattr *rtap = (struct rtattr *)IFLA_RTA(ifip);
	int ifil = RTM_PAYLOAD(nlp);

	const char *ptr = (const char *)0;
	for(;RTA_OK(rtap, ifil);rtap=RTA_NEXT(rtap, ifil))
	{
		switch(rtap->rta_type) {
		case IFLA_ADDRESS:
			ptr = (const char *)RTA_DATA(rtap);
			memcpy(mac_bin, ptr, 6);
			break;
		case IFLA_IFNAME:
			ptr = (const char *)RTA_DATA(rtap);
			memcpy(ifname, ptr, strlen(ptr));
			break;
		case IFLA_MTU:
			memcpy(&mtu, RTA_DATA(rtap), sizeof(unsigned int));
			break;
		}
	}

	{
		Mutex::Lock l(_if_m);
		struct iface_entry &entry = _interfaces[ifip->ifi_index];
		entry.index = ifip->ifi_index;
		memcpy(entry.ifacename, ifname, sizeof(ifname));
		snprintf(entry.mac,sizeof(entry.mac),"%.02x:%.02x:%.02x:%.02x:%.02x:%.02x",(unsigned int)mac_bin[0],(unsigned int)mac_bin[1],(unsigned int)mac_bin[2],(unsigned int)mac_bin[3],(unsigned int)mac_bin[4],(unsigned int)mac_bin[5]);
		memcpy(entry.mac_bin, mac_bin, 6);
		entry.mtu = mtu;
	}
}

void LinuxNetLink::_linkDeleted(struct nlmsghdr *nlp)
{
	unsigned int mtu = 0;
	char ifname[40] = {0};

	struct ifinfomsg *ifip = (struct ifinfomsg *)NLMSG_DATA(nlp);
	struct rtattr *rtap = (struct rtattr *)IFLA_RTA(ifip);
	int ifil = RTM_PAYLOAD(nlp);

	const char *ptr = (const char *)0;
	for(;RTA_OK(rtap, ifil);rtap=RTA_NEXT(rtap, ifil))
	{
		switch(rtap->rta_type) {
		case IFLA_IFNAME:
			ptr = (const char*)RTA_DATA(rtap);
			memcpy(ifname, ptr, strlen(ptr));
			break;
		case IFLA_MTU:
			memcpy(&mtu, RTA_DATA(rtap), sizeof(unsigned int));
			break;
		}
	}

	{
		Mutex::Lock l(_if_m);
		if(_interfaces.contains(ifip->ifi_index)) {
			_interfaces.erase(ifip->ifi_index);
		}
	}
}

void LinuxNetLink::_requestIPv4Routes()
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		fprintf(stderr, "Error opening RTNETLINK socket: %s\n", strerror(errno));
		return;
	}

	_setSocketTimeout(fd);

	struct sockaddr_nl la;
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();
	la.nl_groups = RTMGRP_IPV4_ROUTE;
	if(bind(fd, (struct sockaddr*)&la, sizeof(la))) {
		fprintf(stderr, "Error binding RTNETLINK (_requiestIPv4Routes #1): %s\n", strerror(errno));
		close(fd);
		return;
	}

	struct nl_route_req req;
	bzero(&req, sizeof(req));
	req.nl.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nl.nlmsg_type = RTM_GETROUTE;
	req.nl.nlmsg_pid = 0;
	req.nl.nlmsg_seq = ++_seq;
	req.rt.rtm_family = AF_INET;
	req.rt.rtm_table = RT_TABLE_MAIN;

	struct sockaddr_nl pa;
	bzero(&pa, sizeof(pa));
	pa.nl_family = AF_NETLINK;

	struct msghdr msg;
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void*)&pa;
	msg.msg_namelen = sizeof(pa);

	struct iovec iov;
	bzero(&iov, sizeof(iov));
	iov.iov_base = (void*)&req.nl;
	iov.iov_len = req.nl.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	sendmsg(fd, &msg, 0);

	_doRecv(fd);

	close(fd);
}

void LinuxNetLink::_requestIPv6Routes()
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		fprintf(stderr, "Error opening RTNETLINK socket: %s\n", strerror(errno));
		return;
	}

	_setSocketTimeout(fd);

	struct sockaddr_nl la;
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();
	la.nl_groups = RTMGRP_IPV6_ROUTE;
	if(bind(fd, (struct sockaddr*)&la, sizeof(struct sockaddr_nl))) {
		fprintf(stderr, "Error binding RTNETLINK (_requestIPv6Routes #1): %s\n", strerror(errno));
		close(fd);
		return;
	}

	struct nl_route_req req;
	bzero(&req, sizeof(req));
	req.nl.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nl.nlmsg_type = RTM_GETROUTE;
	req.nl.nlmsg_pid = 0;
	req.nl.nlmsg_seq = ++_seq;
	req.rt.rtm_family = AF_INET6;
	req.rt.rtm_table = RT_TABLE_MAIN;

	struct sockaddr_nl pa;
	bzero(&pa, sizeof(pa));
	pa.nl_family = AF_NETLINK;

	struct msghdr msg;
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void*)&pa;
	msg.msg_namelen = sizeof(pa);

	struct iovec iov;
	bzero(&iov, sizeof(iov));
	iov.iov_base = (void*)&req.nl;
	iov.iov_len = req.nl.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	sendmsg(fd, &msg, 0);

	_doRecv(fd);

	close(fd);
}

void LinuxNetLink::_requestInterfaceList()
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		fprintf(stderr, "Error opening RTNETLINK socket: %s\n", strerror(errno));
		return;
	}

	_setSocketTimeout(fd);

	struct sockaddr_nl la;
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();
	la.nl_groups = RTMGRP_LINK;
	if(bind(fd, (struct sockaddr*)&la, sizeof(struct sockaddr_nl))) {
		fprintf(stderr, "Error binding RTNETLINK (_requestInterfaceList #1): %s\n", strerror(errno));
		close(fd);
		return;
	}

	struct nl_if_req req;
	bzero(&req, sizeof(req));
	req.nl.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nl.nlmsg_type = RTM_GETLINK;
	req.nl.nlmsg_pid = 0;
	req.nl.nlmsg_seq = ++_seq;
	req.ifa.ifi_family = AF_UNSPEC;

	struct sockaddr_nl pa;
	bzero(&pa, sizeof(pa));
	pa.nl_family = AF_NETLINK;

	struct msghdr msg;
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void*)&pa;
	msg.msg_namelen = sizeof(pa);

	struct iovec iov;
	bzero(&iov, sizeof(iov));
	iov.iov_base = (void*)&req.nl;
	iov.iov_len = req.nl.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	sendmsg(fd, &msg, 0);

	_doRecv(fd);

	close(fd);
}

void LinuxNetLink::addRoute(const InetAddress &target, const InetAddress &via, const InetAddress &src, const char *ifaceName)
{
	if (!target) return;

	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		fprintf(stderr, "Error opening RTNETLINK socket: %s\n", strerror(errno));
		return;
	}

	_setSocketTimeout(fd);

	struct sockaddr_nl la;
	bzero(&la, sizeof(la));
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();

	if(bind(fd, (struct sockaddr*)&la, sizeof(struct sockaddr_nl))) {
		fprintf(stderr, "Error binding RTNETLINK (addRoute #1): %s\n", strerror(errno));
		close(fd);
		return;
	}

#ifdef ZT_TRACE
	//char  tmp[64];
	//char tmp2[64];
	//char tmp3[64];
	//fprintf(stderr, "Adding Route. target: %s via: %s src: %s iface: %s\n", target.toString(tmp), via.toString(tmp2), src.toString(tmp3), ifaceName);
#endif

	int rtl = sizeof(struct rtmsg);
	struct nl_route_req req;
	bzero(&req, sizeof(req));

	struct rtattr *rtap = (struct rtattr *)req.buf;
	rtap->rta_type = RTA_DST;
	if (target.isV4()) {
		rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
		memcpy(RTA_DATA(rtap), &((struct sockaddr_in*)&target)->sin_addr, sizeof(struct in_addr));
	} else {
		rtap->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
		memcpy(RTA_DATA(rtap), &((struct sockaddr_in6*)&target)->sin6_addr, sizeof(struct in6_addr));
	}
	rtl += rtap->rta_len;

	if(via) {
		rtap = (struct rtattr *)(((char*)rtap)+rtap->rta_len);
		rtap->rta_type = RTA_GATEWAY;
		if(via.isV4()) {
			rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
			memcpy(RTA_DATA(rtap), &((struct sockaddr_in*)&via)->sin_addr, sizeof(struct in_addr));
		} else {
			rtap->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
			memcpy(RTA_DATA(rtap), &((struct sockaddr_in6*)&via)->sin6_addr, sizeof(struct in6_addr));
		}
		rtl += rtap->rta_len;
	} else if (src) {
		rtap = (struct rtattr *)(((char*)rtap)+rtap->rta_len);
		rtap->rta_type = RTA_SRC;
		if(src.isV4()) {
			rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
			memcpy(RTA_DATA(rtap), &((struct sockaddr_in*)&src)->sin_addr, sizeof(struct in_addr));
			
		} else {
			rtap->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
			memcpy(RTA_DATA(rtap), &((struct sockaddr_in6*)&src)->sin6_addr, sizeof(struct in6_addr));
		}
		req.rt.rtm_src_len = src.netmaskBits();
	}

	if (ifaceName != NULL) {
		int interface_index = _indexForInterface(ifaceName);
		if (interface_index != -1) {
			rtap = (struct rtattr *) (((char*)rtap) + rtap->rta_len);
			rtap->rta_type = RTA_OIF;
			rtap->rta_len = RTA_LENGTH(sizeof(int));
			memcpy(RTA_DATA(rtap), &interface_index, sizeof(int));
			rtl += rtap->rta_len;
		}
	}

	req.nl.nlmsg_len = NLMSG_LENGTH(rtl);
	req.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_EXCL | NLM_F_CREATE | NLM_F_ACK;
	req.nl.nlmsg_type = RTM_NEWROUTE;
	req.nl.nlmsg_pid = 0;
	req.nl.nlmsg_seq = ++_seq;
	req.rt.rtm_family = target.ss_family;
	req.rt.rtm_table = RT_TABLE_MAIN;
	req.rt.rtm_protocol = RTPROT_STATIC;
	req.rt.rtm_scope = RT_SCOPE_UNIVERSE;
	req.rt.rtm_type = RTN_UNICAST;
	req.rt.rtm_dst_len = target.netmaskBits();
	req.rt.rtm_flags = 0;

	struct sockaddr_nl pa;
	bzero(&pa, sizeof(pa));
	pa.nl_family = AF_NETLINK;

	struct msghdr msg;
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void*)&pa;
	msg.msg_namelen = sizeof(pa);

	struct iovec iov;
	bzero(&iov, sizeof(iov));
	iov.iov_base = (void*)&req.nl;
	iov.iov_len = req.nl.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	sendmsg(fd, &msg, 0);

	_doRecv(fd);

	close(fd);
}

void LinuxNetLink::delRoute(const InetAddress &target, const InetAddress &via, const InetAddress &src, const char *ifaceName)
{
	if (!target) return;

	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		fprintf(stderr, "Error opening RTNETLINK socket: %s\n", strerror(errno));
		return;
	}

	_setSocketTimeout(fd);

	struct sockaddr_nl la;
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();

	if(bind(fd, (struct sockaddr*)&la, sizeof(struct sockaddr_nl))) {
		fprintf(stderr, "Error binding RTNETLINK (delRoute #1): %s\n", strerror(errno));
		close(fd);
		return;
	}

#ifdef ZT_TRACE
	//char  tmp[64];
	//char tmp2[64];
	//char tmp3[64];
	//fprintf(stderr, "Removing Route. target: %s via: %s src: %s iface: %s\n", target.toString(tmp), via.toString(tmp2), src.toString(tmp3), ifaceName);
#endif

	int rtl = sizeof(struct rtmsg);
	struct nl_route_req req;
	bzero(&req, sizeof(req));

	struct rtattr *rtap = (struct rtattr *)req.buf;
	rtap->rta_type = RTA_DST;
	if (target.isV4()) {
		rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
		memcpy(RTA_DATA(rtap), &((struct sockaddr_in*)&target)->sin_addr, sizeof(struct in_addr));
	} else {
		rtap->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
		memcpy(RTA_DATA(rtap), &((struct sockaddr_in6*)&target)->sin6_addr, sizeof(struct in6_addr));
	}
	rtl += rtap->rta_len;

	if(via) {
		rtap = (struct rtattr *)(((char*)rtap)+rtap->rta_len);
		rtap->rta_type = RTA_GATEWAY;
		if(via.isV4()) {
			rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
			memcpy(RTA_DATA(rtap), &((struct sockaddr_in*)&via)->sin_addr, sizeof(struct in_addr));
		} else {
			rtap->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
			memcpy(RTA_DATA(rtap), &((struct sockaddr_in6*)&via)->sin6_addr, sizeof(struct in6_addr));
		}
		rtl += rtap->rta_len;
	} else if (src) {
		rtap = (struct rtattr *)(((char*)rtap)+rtap->rta_len);
		rtap->rta_type = RTA_SRC;
		if(src.isV4()) {
			rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
			memcpy(RTA_DATA(rtap), &((struct sockaddr_in*)&src)->sin_addr, sizeof(struct in_addr));
			
		} else {
			rtap->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
			memcpy(RTA_DATA(rtap), &((struct sockaddr_in6*)&src)->sin6_addr, sizeof(struct in6_addr));
		}
		req.rt.rtm_src_len = src.netmaskBits();
	}

	if (ifaceName != NULL) {
		int interface_index = _indexForInterface(ifaceName);
		if (interface_index != -1) {
			rtap = (struct rtattr *) (((char*)rtap) + rtap->rta_len);
			rtap->rta_type = RTA_OIF;
			rtap->rta_len = RTA_LENGTH(sizeof(int));
			memcpy(RTA_DATA(rtap), &interface_index, sizeof(int));
			rtl += rtap->rta_len;
		}
	}

	req.nl.nlmsg_len = NLMSG_LENGTH(rtl);
	req.nl.nlmsg_flags = NLM_F_REQUEST;
	req.nl.nlmsg_type = RTM_DELROUTE;
	req.nl.nlmsg_pid = 0;
	req.nl.nlmsg_seq = ++_seq;
	req.rt.rtm_family = target.ss_family;
	req.rt.rtm_table = RT_TABLE_MAIN;
	req.rt.rtm_protocol = RTPROT_STATIC;
	req.rt.rtm_scope = RT_SCOPE_UNIVERSE;
	req.rt.rtm_type = RTN_UNICAST;
	req.rt.rtm_dst_len = target.netmaskBits();
	req.rt.rtm_flags = 0;

	struct sockaddr_nl pa;
	bzero(&pa, sizeof(pa));
	pa.nl_family = AF_NETLINK;

	struct msghdr msg;
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void*)&pa;
	msg.msg_namelen = sizeof(pa);

	struct iovec iov;
	bzero(&iov, sizeof(iov));
	iov.iov_base = (void*)&req.nl;
	iov.iov_len = req.nl.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	sendmsg(fd, &msg, 0);

	_doRecv(fd);

	close(fd);
}

void LinuxNetLink::addAddress(const InetAddress &addr, const char *iface)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		fprintf(stderr, "Error opening RTNETLINK socket: %s\n", strerror(errno));
		return;
	}

	_setSocketTimeout(fd);

	struct sockaddr_nl la;
	memset(&la,0,sizeof(la));
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();
	if (addr.isV4()) {
		la.nl_groups = RTMGRP_IPV4_IFADDR;
	} else {
		la.nl_groups = RTMGRP_IPV6_IFADDR;
	}

	if(bind(fd, (struct sockaddr*)&la, sizeof(struct sockaddr_nl))) {
		fprintf(stderr, "Error binding RTNETLINK (addAddress #1): %s\n", strerror(errno));
		close(fd);
		return;
	}

#ifdef ZT_TRACE
	//char tmp[128];
	//fprintf(stderr, "Adding IP address %s to interface %s", addr.toString(tmp), iface);
#endif

	int interface_index = _indexForInterface(iface);
	for (int reps = 0; interface_index == -1 && reps < 10; ++reps) {
		Thread::sleep(100);
		interface_index = _indexForInterface(iface);
	}

	if (interface_index == -1) {
		fprintf(stderr, "Unable to find index for interface %s\n", iface);
		close(fd);
		return;
	}
	
	int rtl = sizeof(struct ifaddrmsg);
	struct nl_adr_req req;
	bzero(&req, sizeof(struct nl_adr_req));

	struct rtattr *rtap = (struct rtattr *)req.buf;;
	if(addr.isV4()) {
		struct sockaddr_in *addr_v4 = (struct sockaddr_in*)&addr;
		rtap->rta_type = IFA_ADDRESS;
		rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
		memcpy(RTA_DATA(rtap), &addr_v4->sin_addr, sizeof(struct in_addr));
		rtl += rtap->rta_len;

		rtap = (struct rtattr*)(((char*)rtap) + rtap->rta_len);
		rtap->rta_type = IFA_LOCAL;
		rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
		memcpy(RTA_DATA(rtap), &addr_v4->sin_addr, sizeof(struct in_addr));
		rtl += rtap->rta_len;

		InetAddress broadcast = addr.broadcast();
		if(broadcast) {
			rtap = (struct rtattr*)(((char*)rtap)+rtap->rta_len);
			struct sockaddr_in *bcast = (struct sockaddr_in*)&broadcast;
			rtap->rta_type = IFA_BROADCAST;
			rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
			memcpy(RTA_DATA(rtap), &bcast->sin_addr, sizeof(struct in_addr));
			rtl += rtap->rta_len;
		}
	} else { //V6
		rtap->rta_type = IFA_ADDRESS;
		struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6*)&addr;
		rtap->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
		memcpy(RTA_DATA(rtap), &addr_v6->sin6_addr, sizeof(struct in6_addr));
		rtl += rtap->rta_len;
	}

	if (iface) {
		rtap = (struct rtattr*)(((char*)rtap)+rtap->rta_len);
		rtap->rta_type = IFA_LABEL;
		rtap->rta_len = RTA_LENGTH(strlen(iface));
		memcpy(RTA_DATA(rtap), iface, strlen(iface));
		rtl += rtap->rta_len;
	}

	req.nl.nlmsg_len = NLMSG_LENGTH(rtl);
	req.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
	req.nl.nlmsg_type = RTM_NEWADDR;
	req.nl.nlmsg_pid = 0;
	req.nl.nlmsg_seq = ++_seq;
	req.ifa.ifa_family = addr.ss_family;
	req.ifa.ifa_prefixlen = addr.port();
	req.ifa.ifa_flags = IFA_F_PERMANENT;
	req.ifa.ifa_scope = 0;
	req.ifa.ifa_index = interface_index;

	struct sockaddr_nl pa;
	bzero(&pa, sizeof(sockaddr_nl));
	pa.nl_family = AF_NETLINK;

	struct msghdr msg;
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void*)&pa;
	msg.msg_namelen = sizeof(pa);

	struct iovec iov;
	iov.iov_base = (void*)&req.nl;
	iov.iov_len = req.nl.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	sendmsg(fd, &msg, 0);

	_doRecv(fd);

	close(fd);
}

void LinuxNetLink::removeAddress(const InetAddress &addr, const char *iface)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		fprintf(stderr, "Error opening RTNETLINK socket: %s\n", strerror(errno));
		return;
	}

	_setSocketTimeout(fd);

	struct sockaddr_nl la;
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();
	if (addr.isV4()) {
		la.nl_groups = RTMGRP_IPV4_IFADDR;
	} else {
		la.nl_groups = RTMGRP_IPV6_IFADDR;
	}
	if(bind(fd, (struct sockaddr*)&la, sizeof(struct sockaddr_nl))) {
		fprintf(stderr, "Error binding RTNETLINK (removeAddress #1): %s\n", strerror(errno));
		close(fd);
		return;
	}

#ifdef ZT_TRACE
	//char tmp[128];
	//fprintf(stderr, "Removing IP address %s from interface %s", addr.toString(tmp), iface);
#endif

	int interface_index = _indexForInterface(iface);

	if (interface_index == -1) {
		fprintf(stderr, "Unable to find index for interface %s\n", iface);
		close(fd);
		return;
	}
	
	int rtl = sizeof(struct ifaddrmsg);
	struct nl_adr_req req;
	bzero(&req, sizeof(struct nl_adr_req));

	struct rtattr *rtap = (struct rtattr *)req.buf;
	if(addr.isV4()) {
		struct sockaddr_in *addr_v4 = (struct sockaddr_in*)&addr;
		rtap->rta_type = IFA_ADDRESS;
		rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
		memcpy(RTA_DATA(rtap), &addr_v4->sin_addr, sizeof(struct in_addr));
		rtl += rtap->rta_len;

		rtap = (struct rtattr*)(((char*)rtap) + rtap->rta_len);
		rtap->rta_type = IFA_LOCAL;
		rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
		memcpy(RTA_DATA(rtap), &addr_v4->sin_addr, sizeof(struct in_addr));
		rtl += rtap->rta_len;

		InetAddress broadcast = addr.broadcast();
		if(broadcast) {
			rtap = (struct rtattr*)(((char*)rtap)+rtap->rta_len);
			struct sockaddr_in *bcast = (struct sockaddr_in*)&broadcast;
			rtap->rta_type = IFA_BROADCAST;
			rtap->rta_len = RTA_LENGTH(sizeof(struct in_addr));
			memcpy(RTA_DATA(rtap), &bcast->sin_addr, sizeof(struct in_addr));
			rtl += rtap->rta_len;
		}
	} else { //V6
		rtap->rta_type = IFA_ADDRESS;
		struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6*)&addr;
		rtap->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
		memcpy(RTA_DATA(rtap), &addr_v6->sin6_addr, sizeof(struct in6_addr));
		rtl += rtap->rta_len;
	}

	if (iface) {
		rtap = (struct rtattr*)(((char*)rtap)+rtap->rta_len);
		rtap->rta_type = IFA_LABEL;
		rtap->rta_len = RTA_LENGTH(strlen(iface));
		memcpy(RTA_DATA(rtap), iface, strlen(iface));
		rtl += rtap->rta_len;
	}

	req.nl.nlmsg_len = NLMSG_LENGTH(rtl);
	req.nl.nlmsg_flags = NLM_F_REQUEST;
	req.nl.nlmsg_type = RTM_DELADDR;
	req.nl.nlmsg_pid = 0;
	req.nl.nlmsg_seq = ++_seq;
	req.ifa.ifa_family = addr.ss_family;
	req.ifa.ifa_prefixlen = addr.port();
	req.ifa.ifa_flags = IFA_F_PERMANENT;
	req.ifa.ifa_scope = 0;
	req.ifa.ifa_index = interface_index;

	struct sockaddr_nl pa;
	bzero(&pa, sizeof(sockaddr_nl));
	pa.nl_family = AF_NETLINK;

	struct msghdr msg;
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void*)&pa;
	msg.msg_namelen = sizeof(pa);

	struct iovec iov;
	iov.iov_base = (void*)&req.nl;
	iov.iov_len = req.nl.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	sendmsg(fd, &msg, 0);

	_doRecv(fd);

	close(fd);
}

RouteList LinuxNetLink::getIPV4Routes() const 
{
	return _routes_ipv4;
}

RouteList LinuxNetLink::getIPV6Routes() const
{
	return _routes_ipv6;
}

int LinuxNetLink::_indexForInterface(const char *iface)
{
	Mutex::Lock l(_if_m);
	int interface_index = -1;
	Hashtable<int, iface_entry>::Iterator iter(_interfaces);
	int *k = NULL;
	iface_entry *v = NULL;
	while(iter.next(k,v)) {
		if(strcmp(iface, v->ifacename) == 0) {
			interface_index = v->index;
			break;
		}
	}
	return interface_index;
}

} // namespace ZeroTier
