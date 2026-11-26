/*
 * zxponctl - minimal generic-netlink client for the "zxpon" family.
 *
 * The bench verbs: hand a downstream PLOAM or OMCI frame to the driver, or
 * write one T-CONT / GEM table entry, and let the driver's own readback say
 * whether the register took it.  libnl is not present on this image, so the
 * few netlink messages involved are built by hand.
 *
 *   zxponctl ploam <96 hex chars>   # 48-byte frame, MIC recomputed in kernel
 *   zxponctl omci-rx <96 hex chars> # downstream OMCI; the reply is sent out
 *   zxponctl omci <96 hex chars>    # push one OMCI frame to the IDM queue
 *   zxponctl tcont <alloc>          # add a T-CONT holding that AllocID
 *   zxponctl omcc <alloc>           # pin T-CONT 0 (the OMCC) to that AllocID
 *   zxponctl gem <gem id>           # mark a downstream GEM port present
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
#define CMD_SET_IDENTITY	2
#define ATTR_SN			2
#define ATTR_REGID		3
#define CMD_PLOAM_RX		8
#define ATTR_PLOAM_FRAME	9
#define CMD_OMCI_TX		7
#define CMD_OMCI_RX		9
#define ATTR_OMCI_FRAME		8
#define CMD_ADD_TCONT		3
#define CMD_ADD_GEM		4
#define ATTR_ALLOC		4
#define ATTR_GEM		5
#define ATTR_OMCC		6
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

/*
 * ITU-T G.984.3 serial number: the 12-character text form (4 ASCII vendor
 * chars + 8 hex digits) packs into the 8 bytes the PLOAM carries.  Vendor
 * "CMDC" + vssn A133E348 -> 43 4d 44 43 a1 33 e3 48.
 */
static int sn_to_bytes(const char *s, unsigned char out[8])
{
	int i;

	if (strlen(s) != 12)
		return -1;
	memcpy(out, s, 4);
	for (i = 0; i < 4; i++) {
		int hi = hexval((unsigned char)s[4 + i * 2]);
		int lo = hexval((unsigned char)s[5 + i * 2]);

		if (hi < 0 || lo < 0)
			return -1;
		out[4 + i] = (unsigned char)((hi << 4) | lo);
	}
	return 0;
}

/* CMD_SET_IDENTITY carries two fixed-length attributes; the driver copies
 * them verbatim, arms the activation and lets the PLOAM machine start. */
static int send_identity(int fd, int family, const char *snstr,
			 const char *regstr)
{
	struct nlreq r;
	unsigned char sn[8];
	unsigned char regid[36];

	if (sn_to_bytes(snstr, sn)) {
		fprintf(stderr,
			"bad SN '%s': want 12 chars, e.g. CMDCA133E348\n",
			snstr);
		return -EINVAL;
	}
	memset(regid, 0, sizeof(regid));
	strncpy((char *)regid, regstr, sizeof(regid));

	memset(&r, 0, sizeof(r));
	r.nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	r.nlh.nlmsg_type = family;
	r.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	r.nlh.nlmsg_seq = 5;
	r.gnl.cmd = CMD_SET_IDENTITY;
	r.gnl.version = GENL_VERSION;
	if (!nla_put(&r, ATTR_SN, sn, sizeof(sn)) ||
	    !nla_put(&r, ATTR_REGID, regid, sizeof(regid)))
		return -EMSGSIZE;
	if (nl_send(fd, &r) < 0)
		return -EIO;
	return nl_recv_ack(fd);
}

/* One u16 attribute, no payload: the table verbs. */
static int send_u16(int fd, int family, int cmd, int attr, unsigned int value)
{
	struct nlreq r;
	unsigned short v = (unsigned short)value;
	int ret;

	memset(&r, 0, sizeof(r));
	r.nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	r.nlh.nlmsg_type = family;
	r.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	r.nlh.nlmsg_seq = 3;
	r.gnl.cmd = cmd;
	r.gnl.version = GENL_VERSION;
	if (!nla_put(&r, attr, &v, sizeof(v)))
		return -EMSGSIZE;
	if (nl_send(fd, &r) < 0)
		return -EIO;
	ret = nl_recv_ack(fd);
	return ret;
}

int main(int argc, char **argv)
{
	int fd, family, ret;
	struct nlreq r;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s identity <12-char-sn> <reg-id> | ploam|omci-rx|omci <96 hex chars> | tcont|omcc|gem <id> | family\n",
			argv[0]);
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
	if (!strcmp(argv[1], "identity")) {
		if (argc < 4) {
			fprintf(stderr,
				"identity needs <12-char-sn> <reg-id>\n");
			close(fd);
			return 2;
		}
		ret = send_identity(fd, family, argv[2], argv[3]);
		close(fd);
		if (ret) {
			fprintf(stderr, "identity failed: %s (%d)\n",
				strerror(-ret), ret);
			return 1;
		}
		printf("identity accepted\n");
		return 0;
	}
	if (!strcmp(argv[1], "tcont") || !strcmp(argv[1], "omcc") ||
	    !strcmp(argv[1], "gem")) {
		unsigned long value;
		char *end;

		if (argc < 3) {
			fprintf(stderr, "%s needs a number\n", argv[1]);
			close(fd);
			return 2;
		}
		value = strtoul(argv[2], &end, 0);
		if (*end || value > 0xffff) {
			fprintf(stderr, "bad number %s\n", argv[2]);
			close(fd);
			return 2;
		}
		if (!strcmp(argv[1], "gem")) {
			ret = send_u16(fd, family, CMD_ADD_GEM, ATTR_GEM,
				       (unsigned int)value);
		} else if (!strcmp(argv[1], "omcc")) {
			memset(&r, 0, sizeof(r));
			r.nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
			r.nlh.nlmsg_type = family;
			r.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
			r.nlh.nlmsg_seq = 4;
			r.gnl.cmd = CMD_ADD_TCONT;
			r.gnl.version = GENL_VERSION;
			{
				unsigned short alloc = (unsigned short)value;
				unsigned char omcc = 1;

				if (!nla_put(&r, ATTR_ALLOC, &alloc,
					     sizeof(alloc)) ||
				    !nla_put(&r, ATTR_OMCC, &omcc,
					     sizeof(omcc))) {
					close(fd);
					return 2;
				}
			}
			ret = nl_send(fd, &r) < 0 ? -EIO : nl_recv_ack(fd);
		} else {
			ret = send_u16(fd, family, CMD_ADD_TCONT, ATTR_ALLOC,
				       (unsigned int)value);
		}
		close(fd);
		if (ret) {
			fprintf(stderr, "%s failed: %s (%d)\n", argv[1],
				strerror(-ret), ret);
			return 1;
		}
		return 0;
	}

	int cmd, attr;

	if (!strcmp(argv[1], "ploam")) {
		cmd = CMD_PLOAM_RX;
		attr = ATTR_PLOAM_FRAME;
	} else if (!strcmp(argv[1], "omci-rx")) {
		cmd = CMD_OMCI_RX;
		attr = ATTR_OMCI_FRAME;
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
