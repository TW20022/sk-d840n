/*
 * zxponctl - minimal generic-netlink client for the "zxpon" family.
 *
 * Only one verb is needed on the bench: hand a downstream PLOAM frame to the
 * driver so the O2/O3 -> O4 -> O5 half of the state machine can be exercised
 * with no fibre in the jack.  libnl is not present on this image, so the few
 * netlink messages involved are built by hand.
 *
 *   zxponctl ploam <96 hex chars>   # 48-byte frame, MIC recomputed in kernel
 *   zxponctl family                 # print the resolved family id
 *
 * Build (OpenWrt musl toolchain):
 *   aarch64-openwrt-linux-musl-gcc -static -Os -o zxponctl zxponctl.c
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <linux/netlink.h>
#include <linux/genetlink.h>

#define GENL_FAMILY_NAME	"zxpon"
#define GENL_VERSION		1
#define CMD_PLOAM_RX		8
#define ATTR_PLOAM_FRAME	9
#define CMD_OMCI_TX		7
#define ATTR_OMCI_FRAME		8
#define PLOAM_LEN		48

#define NLA_ALIGNTO_		4
#define NLA_ALIGN_(len)		(((len) + NLA_ALIGNTO_ - 1) & ~(NLA_ALIGNTO_ - 1))
#define NLA_HDRLEN_		NLA_ALIGN_(sizeof(struct nlattr))

struct nlreq {
	struct nlmsghdr nlh;
	struct genlmsghdr gnl;
	char buf[512];
};

static int nl_open(void)
{
	struct sockaddr_nl sa;
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		close(fd);
		return -1;
	}
	return fd;
}

static struct nlattr *nla_put(struct nlreq *r, int type, const void *data,
			      int len)
{
	struct nlattr *a;
	int off = NLMSG_ALIGN(r->nlh.nlmsg_len);
	int total = NLA_HDRLEN_ + NLA_ALIGN_(len);

	if (off + total > (int)sizeof(r->buf) + (int)((char *)&r->buf -
						     (char *)&r->nlh))
		return NULL;
	a = (struct nlattr *)((char *)&r->nlh + off);
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN_ + len;
	memcpy((char *)a + NLA_HDRLEN_, data, len);
	memset((char *)a + NLA_HDRLEN_ + len, 0, NLA_ALIGN_(len) - len);
	r->nlh.nlmsg_len = off + total;
	return a;
}

static int nl_send(int fd, struct nlreq *r)
{
	struct sockaddr_nl sa;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (sendto(fd, r, r->nlh.nlmsg_len, 0, (struct sockaddr *)&sa,
		   sizeof(sa)) < 0) {
		perror("sendto");
		return -1;
	}
	return 0;
}

/* Returns the NLMSG_ERROR code of the reply (0 = ack) or a negative errno. */
static int nl_recv_ack(int fd)
{
	char buf[4096];
	int n;

	n = recv(fd, buf, sizeof(buf), 0);
	if (n < 0) {
		perror("recv");
		return -errno;
	}
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;

	if (nlh->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(nlh);

		return e->error;
	}
	return -EIO;
}

static int genl_family_id(int fd)
{
	struct nlreq r;
	struct nlattr *a;
	int n;

	/* The family id has to be asked for first; it is not a constant. */
	memset(&r, 0, sizeof(r));
	r.nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	r.nlh.nlmsg_type = GENL_ID_CTRL;
	r.nlh.nlmsg_flags = NLM_F_REQUEST;
	r.nlh.nlmsg_seq = 1;
	r.gnl.cmd = CTRL_CMD_GETFAMILY;
	r.gnl.version = 1;
	if (!nla_put(&r, CTRL_ATTR_FAMILY_NAME, GENL_FAMILY_NAME,
		     sizeof(GENL_FAMILY_NAME)))
		return -EMSGSIZE;
	if (nl_send(fd, &r) < 0)
		return -EIO;

	n = recv(fd, &r, sizeof(r), 0);
	if (n < 0) {
		perror("recv");
		return -errno;
	}
	if (r.nlh.nlmsg_type == NLMSG_ERROR)
		return -ENOENT;

	a = (struct nlattr *)((char *)&r.gnl + GENL_HDRLEN);
	for (; (char *)a < (char *)&r + n && a->nla_len >= NLA_HDRLEN_;
	     a = (struct nlattr *)((char *)a + NLA_ALIGN_(a->nla_len))) {
		if (a->nla_type == CTRL_ATTR_FAMILY_ID)
			return *(unsigned short *)((char *)a + NLA_HDRLEN_);
	}
	return -ENOENT;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int main(int argc, char **argv)
{
	int fd, family, ret;
	struct nlreq r;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s ploam <96 hex chars> | family\n", argv[0]);
		return 2;
	}
	fd = nl_open();
	if (fd < 0)
		return 1;
	family = genl_family_id(fd);
	if (family < 0) {
		fprintf(stderr, "zxpon: family not found (%d)\n", family);
		close(fd);
		return 1;
	}
	if (!strcmp(argv[1], "family")) {
		printf("%d\n", family);
		close(fd);
		return 0;
	}
	int cmd, attr;

	if (!strcmp(argv[1], "ploam")) {
		cmd = CMD_PLOAM_RX;
		attr = ATTR_PLOAM_FRAME;
	} else if (!strcmp(argv[1], "omci")) {
		cmd = CMD_OMCI_TX;
		attr = ATTR_OMCI_FRAME;
	} else {
		fprintf(stderr, "unknown verb %s\n", argv[1]);
		close(fd);
		return 2;
	}
	if (argc < 3 || strlen(argv[2]) != PLOAM_LEN * 2) {
		fprintf(stderr, "%s needs %d hex chars\n", argv[1],
			PLOAM_LEN * 2);
		close(fd);
		return 2;
	}

	unsigned char frame[PLOAM_LEN];
	for (int i = 0; i < PLOAM_LEN; i++) {
		int hi = hexval(argv[2][2 * i]), lo = hexval(argv[2][2 * i + 1]);

		if (hi < 0 || lo < 0) {
			fprintf(stderr, "bad hex at byte %d\n", i);
			close(fd);
			return 2;
		}
		frame[i] = (unsigned char)((hi << 4) | lo);
	}

	memset(&r, 0, sizeof(r));
	r.nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	r.nlh.nlmsg_type = family;
	r.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	r.nlh.nlmsg_seq = 2;
	r.gnl.cmd = cmd;
	r.gnl.version = GENL_VERSION;
	if (!nla_put(&r, attr, frame, PLOAM_LEN)) {
		fprintf(stderr, "message too large\n");
		close(fd);
		return 2;
	}
	if (nl_send(fd, &r) < 0) {
		close(fd);
		return 1;
	}
	ret = nl_recv_ack(fd);
	close(fd);
	if (ret) {
		fprintf(stderr, "inject failed: %s (%d)\n", strerror(-ret), ret);
		return 1;
	}
	return 0;
}
