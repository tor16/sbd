/*
 * Copyright (C) 2013 Lars Marowsky-Bree <lmb@suse.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sbd.h"

/* These have to match the values in the header of the partition */
static char		sbd_magic[8] = "SBD_SBD_";
static char		sbd_version  = 0x02;

/* Tunable defaults: */
#if  defined(__s390__) || defined(__s390x__)
unsigned long	timeout_watchdog 	= 15;
int		timeout_msgwait		= 30;
#else
unsigned long	timeout_watchdog 	= 5;
int		timeout_msgwait		= 10;
#endif
unsigned long	timeout_watchdog_warn 	= 3;
int		timeout_allocate 	= 2;
int		timeout_loop	    	= 1;
int		timeout_io		= 3;
int		timeout_startup		= 120;

int	watchdog_use		= 1;
int	watchdog_set_timeout	= 1;
unsigned long	timeout_watchdog_crashdump = 240;
int	skip_rt			= 0;
int	check_pcmk		= 0;
int	debug			= 0;
int	debug_mode		= 0;
const char *watchdogdev		= "/dev/watchdog";
char *	local_uname;

/* Global, non-tunable variables: */
int	sector_size		= 0;
int	watchdogfd 		= -1;

/*const char	*devname;*/
const char	*cmdname;

void
usage(void)
{
	fprintf(stderr,
"Shared storage fencing tool.\n"
"Syntax:\n"
"	%s <options> <command> <cmdarguments>\n"
"Options:\n"
"-d <devname>	Block device to use (mandatory; can be specified up to 3 times)\n"
"-h		Display this help.\n"
"-n <node>	Set local node name; defaults to uname -n (optional)\n"
"\n"
"-R		Do NOT enable realtime priority (debugging only)\n"
"-W		Use watchdog (recommended) (watch only)\n"
"-w <dev>	Specify watchdog device (optional) (watch only)\n"
"-T		Do NOT initialize the watchdog timeout (watch only)\n"
"-S <0|1>	Set start mode if the node was previously fenced (watch only)\n"
"-p <path>	Write pidfile to the specified path (watch only)\n"
"-v		Enable some verbose debug logging (optional)\n"
"\n"
"-1 <N>		Set watchdog timeout to N seconds (optional, create only)\n"
"-2 <N>		Set slot allocation timeout to N seconds (optional, create only)\n"
"-3 <N>		Set daemon loop timeout to N seconds (optional, create only)\n"
"-4 <N>		Set msgwait timeout to N seconds (optional, create only)\n"
"-5 <N>		Warn if loop latency exceeds threshold (optional, watch only)\n"
"			(default is 3, set to 0 to disable)\n"
"-C <N>		Watchdog timeout to set before crashdumping (def: 240s, optional)\n"
"-I <N>		Async IO read timeout (defaults to 3 * loop timeout, optional)\n"
"-s <N>		Timeout to wait for devices to become available (def: 120s)\n"
"-t <N>		Dampening delay before faulty servants are restarted (optional)\n"
"			(default is 5, set to 0 to disable)\n"
"-F <N>		# of failures before a servant is considered faulty (optional)\n"
"			(default is 1, set to 0 to disable)\n"
"-P		Check Pacemaker quorum and node health (optional, watch only)\n"
"-Z		Enable trace mode. WARNING: UNSAFE FOR PRODUCTION!\n"
"Commands:\n"
"create		initialize N slots on <dev> - OVERWRITES DEVICE!\n"
"list		List all allocated slots on device, and messages.\n"
"dump		Dump meta-data header from device.\n"
"watch		Loop forever, monitoring own slot\n"
"allocate <node>\n"
"		Allocate a slot for node (optional)\n"
"message <node> (test|reset|off|clear|exit)\n"
"		Writes the specified message to node's slot.\n"
, cmdname);
}

int
watchdog_init_interval(void)
{
	int     timeout = timeout_watchdog;

	if (watchdogfd < 0) {
		return 0;
	}


	if (watchdog_set_timeout == 0) {
		cl_log(LOG_INFO, "NOT setting watchdog timeout on explicit user request!");
		return 0;
	}

	if (ioctl(watchdogfd, WDIOC_SETTIMEOUT, &timeout) < 0) {
		cl_perror( "WDIOC_SETTIMEOUT"
				": Failed to set watchdog timer to %u seconds.",
				timeout);
		cl_log(LOG_CRIT, "Please validate your watchdog configuration!");
		cl_log(LOG_CRIT, "Choose a different watchdog driver or specify -T to skip this if you are completely sure.");
		return -1;
	} else {
		cl_log(LOG_INFO, "Set watchdog timeout to %u seconds.",
				timeout);
	}
	return 0;
}

int
watchdog_tickle(void)
{
	if (watchdogfd >= 0) {
		if (write(watchdogfd, "", 1) != 1) {
			cl_perror("Watchdog write failure: %s!",
					watchdogdev);
			return -1;
		}
	}
	return 0;
}

int
watchdog_init(void)
{
	if (watchdogfd < 0 && watchdogdev != NULL) {
		watchdogfd = open(watchdogdev, O_WRONLY);
		if (watchdogfd >= 0) {
			cl_log(LOG_NOTICE, "Using watchdog device: %s",
					watchdogdev);
			if ((watchdog_init_interval() < 0)
					|| (watchdog_tickle() < 0)) {
				return -1;
			}
		}else{
			cl_perror("Cannot open watchdog device: %s",
					watchdogdev);
			return -1;
		}
	}
	return 0;
}

void
watchdog_close(void)
{
	if (watchdogfd >= 0) {
		if (write(watchdogfd, "V", 1) != 1) {
			cl_perror(
			"Watchdog write magic character failure: closing %s!",
				watchdogdev);
		}
		if (close(watchdogfd) < 0) {
			cl_perror("Watchdog close(2) failed.");
		}
		watchdogfd = -1;
	}
}

/* This duplicates some code from linux/ioprio.h since these are not included
 * even in linux-kernel-headers. Sucks. See also
 * /usr/src/linux/Documentation/block/ioprio.txt and ioprio_set(2) */
extern int sys_ioprio_set(int, int, int);
int ioprio_set(int which, int who, int ioprio);
inline int ioprio_set(int which, int who, int ioprio)
{
        return syscall(__NR_ioprio_set, which, who, ioprio);
}

enum {
        IOPRIO_CLASS_NONE,
        IOPRIO_CLASS_RT,
        IOPRIO_CLASS_BE,
        IOPRIO_CLASS_IDLE,
};

enum {
        IOPRIO_WHO_PROCESS = 1,
        IOPRIO_WHO_PGRP,
        IOPRIO_WHO_USER,
};

#define IOPRIO_BITS             (16)
#define IOPRIO_CLASS_SHIFT      (13)
#define IOPRIO_PRIO_MASK        ((1UL << IOPRIO_CLASS_SHIFT) - 1)

#define IOPRIO_PRIO_CLASS(mask) ((mask) >> IOPRIO_CLASS_SHIFT)
#define IOPRIO_PRIO_DATA(mask)  ((mask) & IOPRIO_PRIO_MASK)
#define IOPRIO_PRIO_VALUE(class, data)  (((class) << IOPRIO_CLASS_SHIFT) | data)

void
maximize_priority(void)
{
	if (skip_rt) {
		cl_log(LOG_INFO, "Not elevating to realtime (-R specified).");
		return;
	}

	cl_make_realtime(-1, 100, 256, 256);

	if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(),
			IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 1)) != 0) {
		cl_perror("ioprio_set() call failed.");
	}
}

void
close_device(struct sbd_context *st)
{
	close(st->devfd);
	free(st);
}

struct sbd_context *
open_device(const char* devname, int loglevel)
{
	struct sbd_context *st;

	if (!devname)
		return NULL;

	st = malloc(sizeof(struct sbd_context));
	if (!st)
		return NULL;
	memset(st, 0, sizeof(struct sbd_context));

	if (io_setup(1, &st->ioctx) != 0) {
		cl_perror("io_setup failed");
		free(st);
		return NULL;
	}
	
	st->devfd = open(devname, O_SYNC|O_RDWR|O_DIRECT);

	if (st->devfd == -1) {
		if (loglevel == LOG_DEBUG) {
			DBGLOG(loglevel, "Opening device %s failed.", devname);
		} else {
			cl_log(loglevel, "Opening device %s failed.", devname);
		}
		free(st);
		return NULL;
	}

	ioctl(st->devfd, BLKSSZGET, &sector_size);

	if (sector_size == 0) {
		cl_perror("Get sector size failed.\n");
		close_device(st);
		return NULL;
	}

	return st;
}

signed char
cmd2char(const char *cmd)
{
	if (strcmp("clear", cmd) == 0) {
		return SBD_MSG_EMPTY;
	} else if (strcmp("test", cmd) == 0) {
		return SBD_MSG_TEST;
	} else if (strcmp("reset", cmd) == 0) {
		return SBD_MSG_RESET;
	} else if (strcmp("off", cmd) == 0) {
		return SBD_MSG_OFF;
	} else if (strcmp("exit", cmd) == 0) {
		return SBD_MSG_EXIT;
	} else if (strcmp("crashdump", cmd) == 0) {
		return SBD_MSG_CRASHDUMP;
	}
	return -1;
}

void *
sector_alloc(void)
{
	void *x;

	x = valloc(sector_size);
	if (!x) {
		exit(1);
	}
	memset(x, 0, sector_size);

	return x;
}

const char*
char2cmd(const char cmd)
{
	switch (cmd) {
		case SBD_MSG_EMPTY:
			return "clear";
			break;
		case SBD_MSG_TEST:
			return "test";
			break;
		case SBD_MSG_RESET:
			return "reset";
			break;
		case SBD_MSG_OFF:
			return "off";
			break;
		case SBD_MSG_EXIT:
			return "exit";
			break;
		case SBD_MSG_CRASHDUMP:
			return "crashdump";
			break;
		default:
			return "undefined";
			break;
	}
}

static int
sector_io(struct sbd_context *st, int sector, void *data, int rw)
{
	struct timespec	timeout;
	struct io_event event;
	struct iocb	*ios[1] = { &st->io };
	long		r;

	timeout.tv_sec  = timeout_io;
	timeout.tv_nsec = 0;

	memset(&st->io, 0, sizeof(struct iocb));
	if (rw) {
		io_prep_pwrite(&st->io, st->devfd, data, sector_size, sector_size * sector);
	} else {
		io_prep_pread(&st->io, st->devfd, data, sector_size, sector_size * sector);
	}

	if (io_submit(st->ioctx, 1, ios) != 1) {
		cl_log(LOG_ERR, "Failed to submit IO request! (rw=%d)", rw);
		return -1;
	}

	errno = 0;
	r = io_getevents(st->ioctx, 1L, 1L, &event, &timeout);

	if (r < 0 ) {
		cl_log(LOG_ERR, "Failed to retrieve IO events (rw=%d)", rw);
		return -1;
	} else if (r < 1L) {
		cl_log(LOG_INFO, "Cancelling IO request due to timeout (rw=%d)", rw);
		r = io_cancel(st->ioctx, ios[0], &event);
		if (r) {
			DBGLOG(LOG_INFO, "Could not cancel IO request (rw=%d)", rw);
			/* Doesn't really matter, debugging information.
			 */
		}
		return -1;
	} else if (r > 1L) {
		cl_log(LOG_ERR, "More than one IO was returned (r=%ld)", r);
		return -1;
	}

	
	/* IO is happy */
	if (event.res == sector_size) {
		return 0;
	} else {
		cl_log(LOG_ERR, "Short IO (rw=%d, res=%lu, sector_size=%d)",
				rw, event.res, sector_size);
		return -1;
	}
}

int
sector_write(struct sbd_context *st, int sector, void *data)
{
	return sector_io(st, sector, data, 1);
}

int
sector_read(struct sbd_context *st, int sector, void *data)
{
	return sector_io(st, sector, data, 0);
}

int
slot_read(struct sbd_context *st, int slot, struct sector_node_s *s_node)
{
	return sector_read(st, SLOT_TO_SECTOR(slot), s_node);
}

int
slot_write(struct sbd_context *st, int slot, struct sector_node_s *s_node)
{
	return sector_write(st, SLOT_TO_SECTOR(slot), s_node);
}

int
mbox_write(struct sbd_context *st, int mbox, struct sector_mbox_s *s_mbox)
{
	return sector_write(st, MBOX_TO_SECTOR(mbox), s_mbox);
}

int
mbox_read(struct sbd_context *st, int mbox, struct sector_mbox_s *s_mbox)
{
	return sector_read(st, MBOX_TO_SECTOR(mbox), s_mbox);
}

int
mbox_write_verify(struct sbd_context *st, int mbox, struct sector_mbox_s *s_mbox)
{
	void *data;
	int rc = 0;

	if (sector_write(st, MBOX_TO_SECTOR(mbox), s_mbox) < 0)
		return -1;

	data = sector_alloc();
	if (sector_read(st, MBOX_TO_SECTOR(mbox), data) < 0) {
		rc = -1;
		goto out;
	}


	if (memcmp(s_mbox, data, sector_size) != 0) {
		cl_log(LOG_ERR, "Write verification failed!");
		rc = -1;
		goto out;
	}
	rc = 0;
out:
	free(data);
	return rc;
}

int header_write(struct sbd_context *st, struct sector_header_s *s_header)
{
	s_header->sector_size = htonl(s_header->sector_size);
	s_header->timeout_watchdog = htonl(s_header->timeout_watchdog);
	s_header->timeout_allocate = htonl(s_header->timeout_allocate);
	s_header->timeout_loop = htonl(s_header->timeout_loop);
	s_header->timeout_msgwait = htonl(s_header->timeout_msgwait);
	return sector_write(st, 0, s_header);
}

int
header_read(struct sbd_context *st, struct sector_header_s *s_header)
{
	if (sector_read(st, 0, s_header) < 0)
		return -1;

	s_header->sector_size = ntohl(s_header->sector_size);
	s_header->timeout_watchdog = ntohl(s_header->timeout_watchdog);
	s_header->timeout_allocate = ntohl(s_header->timeout_allocate);
	s_header->timeout_loop = ntohl(s_header->timeout_loop);
	s_header->timeout_msgwait = ntohl(s_header->timeout_msgwait);
	/* This sets the global defaults: */
	timeout_watchdog = s_header->timeout_watchdog;
	timeout_allocate = s_header->timeout_allocate;
	timeout_loop     = s_header->timeout_loop;
	timeout_msgwait  = s_header->timeout_msgwait;

	return 0;
}

int
valid_header(const struct sector_header_s *s_header)
{
	if (memcmp(s_header->magic, sbd_magic, sizeof(s_header->magic)) != 0) {
		cl_log(LOG_ERR, "Header magic does not match.");
		return -1;
	}
	if (s_header->version != sbd_version) {
		cl_log(LOG_ERR, "Header version does not match.");
		return -1;
	}
	if (s_header->sector_size != sector_size) {
		cl_log(LOG_ERR, "Header sector size does not match.");
		return -1;
	}
	return 0;
}

struct sector_header_s *
header_get(struct sbd_context *st)
{
	struct sector_header_s *s_header;
	s_header = sector_alloc();

	if (header_read(st, s_header) < 0) {
		cl_log(LOG_ERR, "Unable to read header from device %d", st->devfd);
		return NULL;
	}

	if (valid_header(s_header) < 0) {
		cl_log(LOG_ERR, "header on device %d is not valid.", st->devfd);
		return NULL;
	}

	/* cl_log(LOG_INFO, "Found version %d header with %d slots",
			s_header->version, s_header->slots); */

	return s_header;
}

int
init_device(struct sbd_context *st)
{
	struct sector_header_s	*s_header;
	struct sector_node_s	*s_node;
	struct sector_mbox_s	*s_mbox;
	struct stat 		s;
	char			uuid[37];
	int			i;
	int			rc = 0;

	s_header = sector_alloc();
	s_node = sector_alloc();
	s_mbox = sector_alloc();
	memcpy(s_header->magic, sbd_magic, sizeof(s_header->magic));
	s_header->version = sbd_version;
	s_header->slots = 255;
	s_header->sector_size = sector_size;
	s_header->timeout_watchdog = timeout_watchdog;
	s_header->timeout_allocate = timeout_allocate;
	s_header->timeout_loop = timeout_loop;
	s_header->timeout_msgwait = timeout_msgwait;

	s_header->minor_version = 1;
	uuid_generate(s_header->uuid);
	uuid_unparse_lower(s_header->uuid, uuid);

	fstat(st->devfd, &s);
	/* printf("st_size = %ld, st_blksize = %ld, st_blocks = %ld\n",
			s.st_size, s.st_blksize, s.st_blocks); */

	cl_log(LOG_INFO, "Creating version %d.%d header on device %d (uuid: %s)",
			s_header->version, s_header->minor_version,
			st->devfd, uuid);
	fprintf(stdout, "Creating version %d.%d header on device %d (uuid: %s)\n",
			s_header->version, s_header->minor_version,
			st->devfd, uuid);
	if (header_write(st, s_header) < 0) {
		rc = -1; goto out;
	}
	cl_log(LOG_INFO, "Initializing %d slots on device %d",
			s_header->slots,
			st->devfd);
	fprintf(stdout, "Initializing %d slots on device %d\n",
			s_header->slots,
			st->devfd);
	for (i=0;i < s_header->slots;i++) {
		if (slot_write(st, i, s_node) < 0) {
			rc = -1; goto out;
		}
		if (mbox_write(st, i, s_mbox) < 0) {
			rc = -1; goto out;
		}
	}

out:	free(s_node);
	free(s_header);
	free(s_mbox);
	return(rc);
}

/* Check if there already is a slot allocated to said name; returns the
 * slot number. If not found, returns -1.
 * This is necessary because slots might not be continuous. */
int
slot_lookup(struct sbd_context *st, const struct sector_header_s *s_header, const char *name)
{
	struct sector_node_s	*s_node = NULL;
	int 			i;
	int			rc = -1;

	if (!name) {
		cl_log(LOG_ERR, "slot_lookup(): No name specified.\n");
		goto out;
	}

	s_node = sector_alloc();

	for (i=0; i < s_header->slots; i++) {
		if (slot_read(st, i, s_node) < 0) {
			rc = -2; goto out;
		}
		if (s_node->in_use != 0) {
			if (strncasecmp(s_node->name, name,
						sizeof(s_node->name)) == 0) {
				DBGLOG(LOG_INFO, "%s owns slot %d", name, i);
				rc = i; goto out;
			}
		}
	}

out:	free(s_node);
	return rc;
}

int
slot_unused(struct sbd_context *st, const struct sector_header_s *s_header)
{
	struct sector_node_s	*s_node;
	int 			i;
	int			rc = -1;

	s_node = sector_alloc();

	for (i=0; i < s_header->slots; i++) {
		if (slot_read(st, i, s_node) < 0) {
			rc = -1; goto out;
		}
		if (s_node->in_use == 0) {
			rc = i; goto out;
		}
	}

out:	free(s_node);
	return rc;
}


int
slot_allocate(struct sbd_context *st, const char *name)
{
	struct sector_header_s	*s_header = NULL;
	struct sector_node_s	*s_node = NULL;
	struct sector_mbox_s	*s_mbox = NULL;
	int			i;
	int			rc = 0;

	if (!name) {
		cl_log(LOG_ERR, "slot_allocate(): No name specified.\n");
		fprintf(stderr, "slot_allocate(): No name specified.\n");
		rc = -1; goto out;
	}

	s_header = header_get(st);
	if (!s_header) {
		rc = -1; goto out;
	}

	s_node = sector_alloc();
	s_mbox = sector_alloc();

	while (1) {
		i = slot_lookup(st, s_header, name);
		if ((i >= 0) || (i == -2)) {
			/* -1 is "no slot found", in which case we
			 * proceed to allocate a new one.
			 * -2 is "read error during lookup", in which
			 * case we error out too
			 * >= 0 is "slot already allocated" */
			rc = i; goto out;
		}

		i = slot_unused(st, s_header);
		if (i >= 0) {
			cl_log(LOG_INFO, "slot %d is unused - trying to own", i);
			fprintf(stdout, "slot %d is unused - trying to own\n", i);
			memset(s_node, 0, sizeof(*s_node));
			s_node->in_use = 1;
			strncpy(s_node->name, name, sizeof(s_node->name));
			if (slot_write(st, i, s_node) < 0) {
				rc = -1; goto out;
			}
			sleep(timeout_allocate);
		} else {
			cl_log(LOG_ERR, "No more free slots.");
			fprintf(stderr, "No more free slots.\n");
			rc = -1; goto out;
		}
	}

out:	free(s_node);
	free(s_header);
	free(s_mbox);
	return(rc);
}

int
slot_list(struct sbd_context *st)
{
	struct sector_header_s	*s_header = NULL;
	struct sector_node_s	*s_node = NULL;
	struct sector_mbox_s	*s_mbox = NULL;
	int 			i;
	int			rc = 0;

	s_header = header_get(st);
	if (!s_header) {
		rc = -1; goto out;
	}

	s_node = sector_alloc();
	s_mbox = sector_alloc();

	for (i=0; i < s_header->slots; i++) {
		if (slot_read(st, i, s_node) < 0) {
			rc = -1; goto out;
		}
		if (s_node->in_use > 0) {
			if (mbox_read(st, i, s_mbox) < 0) {
				rc = -1; goto out;
			}
			printf("%d\t%s\t%s\t%s\n",
				i, s_node->name, char2cmd(s_mbox->cmd),
				s_mbox->from);
		}
	}

out:	free(s_node);
	free(s_header);
	free(s_mbox);
	return rc;
}

int
slot_msg(struct sbd_context *st, const char *name, const char *cmd)
{
	struct sector_header_s	*s_header = NULL;
	struct sector_mbox_s	*s_mbox = NULL;
	int			mbox;
	int			rc = 0;
	char			uuid[37];

	if (!name || !cmd) {
		cl_log(LOG_ERR, "slot_msg(): No recipient / cmd specified.\n");
		rc = -1; goto out;
	}

	s_header = header_get(st);
	if (!s_header) {
		rc = -1; goto out;
	}

	if (strcmp(name, "LOCAL") == 0) {
		name = local_uname;
	}
	
	if (s_header->minor_version > 0) {
		uuid_unparse_lower(s_header->uuid, uuid);
		cl_log(LOG_INFO, "Device UUID: %s", uuid);
	}

	mbox = slot_lookup(st, s_header, name);
	if (mbox < 0) {
		cl_log(LOG_ERR, "slot_msg(): No slot found for %s.", name);
		rc = -1; goto out;
	}

	s_mbox = sector_alloc();

	s_mbox->cmd = cmd2char(cmd);
	if (s_mbox->cmd < 0) {
		cl_log(LOG_ERR, "slot_msg(): Invalid command %s.", cmd);
		rc = -1; goto out;
	}

	strncpy(s_mbox->from, local_uname, sizeof(s_mbox->from)-1);

	cl_log(LOG_INFO, "Writing %s to node slot %s",
			cmd, name);
	if (mbox_write_verify(st, mbox, s_mbox) < -1) {
		rc = -1; goto out;
	}
	if (strcasecmp(cmd, "exit") != 0) {
		cl_log(LOG_INFO, "Messaging delay: %d",
				(int)timeout_msgwait);
		sleep(timeout_msgwait);
	}
	cl_log(LOG_INFO, "%s successfully delivered to %s",
			cmd, name);

out:	free(s_mbox);
	free(s_header);
	return rc;
}

int
slot_ping(struct sbd_context *st, const char *name)
{
	struct sector_header_s	*s_header = NULL;
	struct sector_mbox_s	*s_mbox = NULL;
	int			mbox;
	int			waited = 0;
	int			rc = 0;

	if (!name) {
		cl_log(LOG_ERR, "slot_ping(): No recipient specified.\n");
		rc = -1; goto out;
	}

	s_header = header_get(st);
	if (!s_header) {
		rc = -1; goto out;
	}

	if (strcmp(name, "LOCAL") == 0) {
		name = local_uname;
	}

	mbox = slot_lookup(st, s_header, name);
	if (mbox < 0) {
		cl_log(LOG_ERR, "slot_msg(): No slot found for %s.", name);
		rc = -1; goto out;
	}

	s_mbox = sector_alloc();
	s_mbox->cmd = SBD_MSG_TEST;

	strncpy(s_mbox->from, local_uname, sizeof(s_mbox->from)-1);

	DBGLOG(LOG_DEBUG, "Pinging node %s", name);
	if (mbox_write(st, mbox, s_mbox) < -1) {
		rc = -1; goto out;
	}

	rc = -1;
	while (waited <= timeout_msgwait) {
		if (mbox_read(st, mbox, s_mbox) < 0)
			break;
		if (s_mbox->cmd != SBD_MSG_TEST) {
			rc = 0;
			break;
		}
		sleep(1);
		waited++;
	}

	if (rc == 0) {
		cl_log(LOG_DEBUG, "%s successfully pinged.", name);
	} else {
		cl_log(LOG_ERR, "%s failed to ping.", name);
	}

out:	free(s_mbox);
	free(s_header);
	return rc;
}

void
sysrq_init(void)
{
	FILE* procf;
	int c;
	procf = fopen("/proc/sys/kernel/sysrq", "r");
	if (!procf) {
		cl_perror("cannot open /proc/sys/kernel/sysrq for read.");
		return;
	}
	if (fscanf(procf, "%d", &c) != 1) {
		cl_perror("Parsing sysrq failed");
		c = 0;
	}
	fclose(procf);
	if (c == 1)
		return;
	/* 8 for debugging dumps of processes, 
	   128 for reboot/poweroff */
	c |= 136; 
	procf = fopen("/proc/sys/kernel/sysrq", "w");
	if (!procf) {
		cl_perror("cannot open /proc/sys/kernel/sysrq for writing");
		return;
	}
	fprintf(procf, "%d", c);
	fclose(procf);
	return;
}

void
sysrq_trigger(char t)
{
	FILE *procf;

	procf = fopen("/proc/sysrq-trigger", "a");
	if (!procf) {
		cl_perror("Opening sysrq-trigger failed.");
		return;
	}
	cl_log(LOG_INFO, "sysrq-trigger: %c\n", t);
	fprintf(procf, "%c\n", t);
	fclose(procf);
	return;
}

void
do_crashdump(void)
{
	if (timeout_watchdog_crashdump) {
		timeout_watchdog = timeout_watchdog_crashdump;
		watchdog_init_interval();
		watchdog_tickle();
	}
	sysrq_trigger('c');
	/* is it possible to reach the following line? */
	cl_reboot(5, "sbd is triggering crashdumping");
	exit(1);
}

void
do_reset(void)
{
	if (debug_mode == 1) {
		cl_log(LOG_ERR, "Request to suicide changed to kdump due to DEBUG MODE!");
		watchdog_close();
		sysrq_trigger('c');
		exit(0);
	} else if (debug_mode == 2) {
		cl_log(LOG_ERR, "Skipping request to suicide due to DEBUG MODE!");
		watchdog_close();
		exit(0);
	} else if (debug_mode == 3) {
		/* The idea is to give the system some time to flush
		 * logs to disk before rebooting. */
		cl_log(LOG_ERR, "Delaying request to suicide by 10s due to DEBUG MODE!");
		watchdog_close();
		sync();
		sync();
		sleep(10);
		cl_log(LOG_ERR, "Debug mode is now becoming real ...");
	}
	sysrq_trigger('b');
	cl_reboot(5, "sbd is self-fencing (reset)");
	sleep(timeout_watchdog * 2);
	exit(1);
}

void
do_off(void)
{
	if (debug_mode == 1) {
		cl_log(LOG_ERR, "Request to power-off changed to kdump due to DEBUG MODE!");
		watchdog_close();
		sysrq_trigger('c');
		exit(0);
	} else 	if (debug_mode == 2) {
		cl_log(LOG_ERR, "Skipping request to power-off due to DEBUG MODE!");
		watchdog_close();
		exit(0);
	} else if (debug_mode == 3) {
		/* The idea is to give the system some time to flush
		 * logs to disk before rebooting. */
		cl_log(LOG_ERR, "Delaying request to power-off by 10s due to DEBUG MODE!");
		watchdog_close();
		sync();
		sync();
		sleep(10);
		cl_log(LOG_ERR, "Debug mode is now becoming real ...");
	}
	sysrq_trigger('o');
	cl_reboot(5, "sbd is self-fencing (power-off)");
	sleep(timeout_watchdog * 2);
	exit(1);
}

pid_t
make_daemon(void)
{
	pid_t			pid;
	const char *		devnull = "/dev/null";

	pid = fork();
	if (pid < 0) {
		cl_log(LOG_ERR, "%s: could not start daemon\n",
				cmdname);
		cl_perror("fork");
		exit(1);
	}else if (pid > 0) {
		return pid;
	}

	cl_log_enable_stderr(FALSE);

	/* This is the child; ensure privileges have not been lost. */
	maximize_priority();
	sysrq_init();

	umask(022);
	close(0);
	(void)open(devnull, O_RDONLY);
	close(1);
	(void)open(devnull, O_WRONLY);
	close(2);
	(void)open(devnull, O_WRONLY);
	cl_cdtocoredir();
	return 0;
}

int
header_dump(struct sbd_context *st)
{
	struct sector_header_s *s_header;
	char uuid[37];

	s_header = header_get(st);
	if (s_header == NULL)
		return -1;

	printf("Header version     : %u.%u\n", s_header->version,
			s_header->minor_version);
	if (s_header->minor_version > 0) {
		uuid_unparse_lower(s_header->uuid, uuid);
		printf("UUID               : %s\n", uuid);
	}

	printf("Number of slots    : %u\n", s_header->slots);
	printf("Sector size        : %lu\n",
			(unsigned long)s_header->sector_size);
	printf("Timeout (watchdog) : %lu\n",
			(unsigned long)s_header->timeout_watchdog);
	printf("Timeout (allocate) : %lu\n",
			(unsigned long)s_header->timeout_allocate);
	printf("Timeout (loop)     : %lu\n",
			(unsigned long)s_header->timeout_loop);
	printf("Timeout (msgwait)  : %lu\n",
			(unsigned long)s_header->timeout_msgwait);
	return 0;
}

void
sbd_get_uname(void)
{
	struct utsname		uname_buf;
	int i;

	if (uname(&uname_buf) < 0) {
		cl_perror("uname() failed?");
		exit(1);
	}

	local_uname = strdup(uname_buf.nodename);

	for (i = 0; i < strlen(local_uname); i++)
		local_uname[i] = tolower(local_uname[i]);
}

