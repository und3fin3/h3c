/*
 * h3c.c
 * 
 * Copyright 2015 BK <renbaoke@gmail.com>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 * 
 * 
 */

#include "h3c.h"

#define send_pkt ((struct packet *)send_buf)

#ifdef AF_LINK
#define sdl(p) ((struct sockaddr_dl *)(p))
#define recv_pkt ((struct packet *)(recv_buf + \
		((struct bpf_hdr *)recv_buf)->bh_hdrlen))
#else
#define recv_pkt ((struct packet *)recv_buf)
#endif

#define eap_type(p) ((unsigned char *)(p) + sizeof(struct packet))
#define eap_data(p) (eap_type(p) + TYPE_LEN)

#define eap_id_info(p) eap_data(p)
#define eap_id_username(p) ((eap_id_info(p)) + sizeof(VERSION_INFO))

#define eap_md5_length(p) eap_data(p)
#define eap_md5_data(p) ((eap_md5_length(p)) + MD5_LEN_LEN)
#define eap_md5_username(p) ((eap_md5_data(p)) + MD5_LEN)

#define eap_h3c_length(p) eap_data(p)
#define eap_h3c_password(p) ((eap_h3c_length(p)) + H3C_LEN_LEN)
#define eap_h3c_username(p) (eap_h3c_password(p) + password_length)

static int sockfd;

static char username[USR_LEN];
static char password[PWD_LEN];

static unsigned char send_buf[BUF_LEN];
static unsigned char recv_buf[BUF_LEN];

#ifdef AF_LINK
static struct bpf_insn insns[] = {
	BPF_STMT(BPF_LD+BPF_H+BPF_ABS, 12),
	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, ETH_P_PAE, 0, 1),
	BPF_STMT(BPF_RET+BPF_K, (u_int)-1),
	BPF_STMT(BPF_RET+BPF_K, 0)
};
static struct bpf_program filter = {
	sizeof(insns) / sizeof(insns[0]),
	insns
};
#else
static struct sockaddr_ll addr;
#endif

static void (*verbose_callback)(char *) = NULL;

static inline void set_eapol_header(unsigned char type, \
		unsigned short length)
{
	send_pkt->eapol_header.version = EAPOL_VERSION;
	send_pkt->eapol_header.type = type;
	send_pkt->eapol_header.length = length;
}

static inline void set_eap_header(unsigned char code, \
		unsigned char id, unsigned short length)
{
	send_pkt->eap_header.code = code;
	send_pkt->eap_header.id = id;
	send_pkt->eap_header.length = length;
}

static int sendout(int length)
{
#ifdef DEBUG
	char msg[MSG_LEN];
	if (verbose_callback)
	{
		snprintf(msg, MSG_LEN, "Sending %d bytes:\n", length);
		verbose_callback(msg);
		int i;
		for (i = 0; i < length; i++)
		{
			if (i == 0)
				;
			else if (i % 16 == 0)
				verbose_callback("\n");
			else
				verbose_callback(" ");

			snprintf(msg, MSG_LEN, "%2.2x", send_buf[i]);
			verbose_callback(msg);
		}
		verbose_callback("\n");
	}
#endif

#ifdef AF_LINK
	return write(sockfd, send_buf, length);
#else
	return sendto(sockfd, send_buf, length, 0, \
			(struct sockaddr*)&addr, sizeof(addr));
#endif
}

static int recvin(int length)
{
	int ret;

#ifdef AF_LINK
	if (read(sockfd, recv_buf, length) == -1)
		return -1;

	ret = ((struct bpf_hdr*)recv_buf)->bh_datalen;
#else
	socklen_t len;
	len = sizeof(addr);

	ret =  recvfrom(sockfd, recv_buf, length, 0, \
			(struct sockaddr *)&addr, &len);
#endif

#ifdef DEBUG
	char msg[MSG_LEN];
	if (verbose_callback)
	{
		snprintf(msg, MSG_LEN, "Received %d bytes:\n", ret);
		verbose_callback(msg);
		int i;
		for (i = 0; i < ret; i++)
		{
			if (i == 0)
				;
			else if (i % 16 == 0)
				verbose_callback("\n");
			else
				verbose_callback(" ");

			snprintf(msg, MSG_LEN, "%2.2x", \
					((unsigned char *)recv_pkt)[i]);
			verbose_callback(msg);
		}
		verbose_callback("\n");
	}
#endif

	return ret;
}

static int send_id(unsigned char packet_id)
{
	int username_length = strlen(username);
	unsigned short len = htons( sizeof(struct eap) + TYPE_LEN + \
			sizeof(VERSION_INFO) + username_length );

	if (verbose_callback)
		verbose_callback("Sending Identity...\n");

	set_eapol_header(EAPOL_EAPPACKET, len);
	set_eap_header(EAP_RESPONSE, packet_id, len);
	*eap_type(send_pkt) = EAP_TYPE_ID;

	memcpy(eap_id_info(send_pkt), VERSION_INFO, sizeof(VERSION_INFO));
	memcpy(eap_id_username(send_pkt), username, username_length);

	return sendout(sizeof(struct packet) + TYPE_LEN + \
			sizeof(VERSION_INFO) + username_length);
}

static int send_md5(unsigned char packet_id, unsigned char *md5data)
{
	int username_length = strlen(username);
	unsigned char md5[MD5_LEN];
	unsigned short len = htons(sizeof(struct eap) + TYPE_LEN + \
			MD5_LEN_LEN + MD5_LEN + username_length);

	if (verbose_callback)
		verbose_callback("Sending MD5-Challenge...\n");

	memset(md5, 0, MD5_LEN);
	memcpy(md5, password, MD5_LEN);

	/* Learned from yah3c. */
	int i;
	for (i = 0; i < MD5_LEN; i++)
		md5[i] ^= md5data[i];

	set_eapol_header(EAPOL_EAPPACKET, len);
	set_eap_header(EAP_RESPONSE, packet_id, len);
	*eap_type(send_pkt) = EAP_TYPE_MD5;

	*eap_md5_length(send_pkt) = MD5_LEN;
	memcpy(eap_md5_data(send_pkt), md5, MD5_LEN);
	memcpy(eap_md5_username(send_pkt), username, username_length);

	return sendout(sizeof(struct packet) + TYPE_LEN + MD5_LEN_LEN + \
			MD5_LEN + username_length);
}

static int send_h3c(unsigned char packet_id)
{
	/* Not called so far as i can observe. */
	int username_length = strlen(username);
	int password_length = strlen(password);
	unsigned short len = htons(sizeof(struct eap) + 1 + 1 + \
			password_length + username_length);

	if (verbose_callback)
		verbose_callback("Sending Allocated...\n");

	set_eapol_header(EAPOL_EAPPACKET, len);
	set_eap_header(EAP_RESPONSE, packet_id, len);
	*eap_type(send_pkt) = EAP_TYPE_H3C;

	*eap_h3c_length(send_pkt) = (unsigned char)password_length;
	memcpy(eap_h3c_password(send_pkt), password, password_length);
	memcpy(eap_h3c_username(send_pkt), username, username_length);

	return sendout(sizeof(struct packet) + TYPE_LEN + H3C_LEN_LEN + \
			password_length + username_length);
}

int h3c_init(char *_interface)
{
	struct ifreq ifr;

	if (verbose_callback)
		verbose_callback("Initilizing...\n");

	/* Set destination mac address. */
	memcpy(send_pkt->eth_header.ether_dhost, PAE_GROUP_ADDR, ETH_ALEN);

	/* Set ethernet type. */
	send_pkt->eth_header.ether_type = htons(ETH_P_PAE);

	strcpy(ifr.ifr_name, _interface);

#ifdef AF_LINK
	struct ifaddrs *ifhead, *ifa;
	char device[] = "/dev/bpf0";
	int n = 0;

	do {
		sockfd = open(device, O_RDWR);
	} while ((sockfd == -1) && (errno == EBUSY) && (device[8]++ != '9'));

	if (sockfd == -1)
		return -1;

	n = BUF_LEN;
	if (ioctl(sockfd, BIOCSBLEN, &n) == -1)
		return -1;

	if (ioctl(sockfd, BIOCSETIF, &ifr) == -1)
		return -1;

	if (ioctl(sockfd, BIOCSETF, &filter) == -1)
		return -1;

	n = 1;
	if (ioctl(sockfd, BIOCIMMEDIATE, &n) == -1)
		return -1;

#ifdef __NetBSD__
	n = 0;
	if (ioctl(sockfd, BIOCSSEESENT, &n) == -1)
		return -1;
#endif
#ifdef __FreeBSD__
	n = BPF_D_IN;
	if (ioctl(sockfd, BIOCSDIRECTION, &n) == -1)
		return -1;
#endif
#ifdef __OpenBSD__
	n = BPF_DIRECTION_OUT;
	if (ioctl(sockfd, BIOCSDIRFILT, &n) == -1)
		return -1;
#endif

#else
	if ((sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_PAE))) == -1)
		return -1;

	if (ioctl(sockfd,SIOCGIFINDEX,&ifr) == -1)
		return -1;
	else
		addr.sll_ifindex = ifr.ifr_ifindex;

	if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) == -1)
		return -1;

	/* Set source mac address. */
	memcpy(send_pkt->eth_header.ether_shost, \
			ifr.ifr_hwaddr.sa_data, ETH_ALEN);
#endif

	return 0;
}

void h3c_set_username(char *_username)
{
	strcpy(username, _username);
}

void h3c_set_password(char *_password)
{
	strcpy(password, _password);
}

void h3c_set_verbose(void (*_verbose_callback)(char *))
{
	verbose_callback = _verbose_callback;
}

int h3c_start()
{
	if (verbose_callback)
		verbose_callback("Starting...\n");

	set_eapol_header(EAPOL_START, 0);
	return sendout(sizeof(struct ether_header)+ sizeof(struct eapol));
}

int h3c_logoff()
{
	if (verbose_callback)
		verbose_callback("Logging off...\n");

	set_eapol_header(EAPOL_LOGOFF, 0);
	return sendout(sizeof(struct ether_header)+ sizeof(struct eapol));
}

int h3c_response(void (*success_callback)(), void (*failure_callback)())
{
	if (verbose_callback)
		verbose_callback("Responsing...\n");

	if (recvin(BUF_LEN) == -1)
		return -1;

	if (recv_pkt->eapol_header.type != EAPOL_EAPPACKET)
		/* Got unknown eapol type. */
		return 0;

	if (recv_pkt->eap_header.code == EAP_SUCCESS)
	{
		/* Got success. */
		if (success_callback)
			success_callback();

		return 0;
	}
	else if (recv_pkt->eap_header.code == EAP_FAILURE)
	{
		/* Got failure. */
		if (failure_callback)
			failure_callback();

		return 0;
	}
	else if (recv_pkt->eap_header.code == EAP_REQUEST)
		/*
		 * Got request.
		 * Response according to request type.
		 */
		if (*eap_type(recv_pkt) == EAP_TYPE_ID)
			return send_id(recv_pkt->eap_header.id);
		else if (*eap_type(recv_pkt) == EAP_TYPE_MD5)
			return send_md5(recv_pkt->eap_header.id, \
					eap_md5_data(recv_pkt));
		else if (*eap_type(recv_pkt) == EAP_TYPE_H3C)
			return send_h3c(recv_pkt->eap_header.id);
		else
			return 0;
	else if (recv_pkt->eap_header.code == EAP_RESPONSE)
		/* Got response. */
		return 0;
	else
		/* Got unkown eap type. */
		return 0;
}

void h3c_clean()
{
	close(sockfd);
}
