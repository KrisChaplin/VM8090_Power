/*
 * Copyright (C) 2013 Philippe Gerum <rpm@xenomai.org>.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <error.h>

#define DEFAULT_DEVICE   "/dev/ttyACM0"
#define ALIAS_DIRECTORY  "/etc/power"

static int on_map, off_map, toggle_map, cycle_map,
	status_map, query_firmware, debug;

static const struct option options[] = {
	{
#define help_opt	0
		.name = "help",
		.has_arg = 0,
		.flag = NULL,
	},
	{
#define turnon_opt	1
		.name = "on",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define turnoff_opt	2
		.name = "off",
		.has_arg = 2,
		.flag = NULL,
	},
	{
#define cycle_opt	3
		.name = "cycle",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define toggle_opt	4
		.name = "toggle",
		.has_arg = 2,
		.flag = NULL,
	},
	{
#define status_opt	5
		.name = "status",
		.has_arg = 2,
		.flag = NULL,
	},
	{
#define firmware_opt	6
		.name = "firmware",
		.has_arg = 0,
		.val = 1,
		.flag = &query_firmware,
	},
	{
#define device_opt	7
		.name = "device",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define debug_opt	8
		.name = "debug",
		.has_arg = 0,
		.val = 1,
		.flag = &debug,
	},
	{
		.name = NULL,
	},
};

struct device_alias {
	char *name;
	int map;
	struct device_alias *next;
} *alias_list;

static void usage(void)
{
        fprintf(stderr, "usage: power <options>, with:\n");
        fprintf(stderr, "--on=<relay-list>           turn relay(s) ON\n");
        fprintf(stderr, "--off[=<relay-list>]        turn relay(s) OFF (defaults to all)\n");
        fprintf(stderr, "--toggle[=<relay-list>]     toggle relay(s) (defaults to all)\n");
        fprintf(stderr, "--cycle=<relay-list>        power cycle relay(s)\n");
        fprintf(stderr, "--status=[<relay-list>]     get relay(s) status (defaults to all)\n");
        fprintf(stderr, "--firmware                  query firmware version\n");
        fprintf(stderr, "--debug                     dump serial traffic to stderr\n");
        fprintf(stderr, "--device=path               path to ACM device\n");
        fprintf(stderr, "--help                      this help\n");
        fprintf(stderr, "(relay-list: [1-8] or alias, [1-8]...)\n");
}

static int match_alias(const char *alias, char *buf, size_t buflen)
{
	ssize_t ret;
	char *path;

	ret = asprintf(&path, "%s/%s", ALIAS_DIRECTORY, alias);
	if (ret < 0)
		error(1, ENOMEM, "asprintf");

	ret = readlink(path, buf, buflen - 1);
	free(path);
	if (ret < 0)
		return 0;

	buf[ret] = '\0';

	return 1;
}

static void __parse_relay_list(const char *list, int *map, int *followed)
{
	char aliases[BUFSIZ], *s, *p, *sp;
	int n, guard = 9999;

	if (*list == '\0') {
		usage();
		exit(1);
	}
		
	s = strdup(list);
	if (s == NULL)
		error(1, ENOMEM, "malloc");

	p = strtok_r(s, ",", &sp);
	do {
		if (match_alias(p, aliases, sizeof(aliases))) {
			if (guard-- == 0)
				error(1, EINVAL, "circular aliasing in /etc/power?");
			(*followed)++;
			__parse_relay_list(aliases, map, followed);
			goto next;
		}
		n = atoi(p);
		if (n < 1 || n > 8) {
			usage();
			exit(1);
		}
		*map |= (1 << (n - 1));
	next:
		p = strtok_r(NULL, ",", &sp);
	} while (p);

	free(s);
}

static int parse_relay_list(const char *list, int *map)
{
	int followed = 0;

	__parse_relay_list(list, map, &followed);

	return followed;
}

static void load_alias_list(void)
{
	struct device_alias *alias;
	struct dirent *de;
	int followed;
	DIR *d;

	d = opendir(ALIAS_DIRECTORY);
	if (d == NULL)
		error(1, errno, "cannot open alias directory %s", ALIAS_DIRECTORY);

	
	for (;;) {
		de = readdir(d);
		if (de == NULL)
			break;	/* Assume ok. */
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		alias = malloc(sizeof(*alias));
		if (alias == NULL)
			error(1, ENOMEM, "cannot build alias list");
		alias->name = strdup(de->d_name);
		alias->map = 0;

		followed = parse_relay_list(de->d_name, &alias->map);
		/*
		 * We only want single direct aliases in the list,
		 * i.e. one_name -> one_relay. So we exclude all
		 * indirect aliases, and all group aliases, e.g. name
		 * -> name..., or name -> relay, relay...
		 */
		if (followed > 1 || alias->map == 0 || 
		    (alias->map & (alias->map - 1)) != 0) {
			free(alias->name);
			free(alias);
		} else {
			alias->next = alias_list;
			alias_list = alias;
		}
	}

	closedir(d);
}

static int open_device(const char *devname)
{
	struct termios ios;
	int fd;

	fd = open(devname, O_RDWR | O_NOCTTY);
	if (fd < 0)
		error(1, errno, "open(%s)", devname);

	/* Switch to 19200, raw mode. */
	tcgetattr(fd, &ios);
	cfsetispeed(&ios, B19200);
	cfsetospeed(&ios, B19200);
	ios.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	ios.c_oflag &= ~OPOST;
	ios.c_cflag &= ~(CSTOPB | PARENB | PARODD | CSIZE);
	ios.c_cflag |= (CS8 | CLOCAL | CREAD);
	ios.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	ios.c_cc[VMIN] = 5; ios.c_cc[VTIME] = 8;
	ios.c_cc[VMIN] = 0; ios.c_cc[VTIME] = 0;
	ios.c_cc[VMIN] = 2; ios.c_cc[VTIME] = 0;
	ios.c_cc[VMIN] = 0; ios.c_cc[VTIME] = 8;
	tcsetattr(fd ,TCSAFLUSH, &ios);

	return fd;
}

#define PACKET_STX   0x04
#define PACKET_ETX   0x0f
#define GET_STATUS   0x18
#define TURN_ON      0x11
#define TURN_OFF     0x12
#define TOGGLE       0x14
#define GET_VERSION  0x71

#define for_each_relay(n, map)			\
	for (n = 0; n < 8; n++)			\
		if ((map) & (1 << n))

struct k8090_packet {
	struct k8090_payload {
		uint8_t stx;
		uint8_t command;
		uint8_t mask;
		uint8_t param1;
		uint8_t param2;
	} payload;
	uint8_t checksum;
	uint8_t etx;
} __attribute__((packed));

static inline uint8_t do_checksum(struct k8090_payload *p)
{
	uint8_t checksum, *r;

	for (checksum = 0, r = (uint8_t *)p; r < (uint8_t *)(p + 1); r++)
		checksum += *r;

	return ~checksum + 1;
}

static void dump_packet(const char *dir, struct k8090_packet *pkt)
{
	printf("%s { %.2x, %.2x, %.2x, %.2x, %.2x, %.2x, %.2x }\n",
	       dir,
	       pkt->payload.stx, 
	       pkt->payload.command,
	       pkt->payload.mask,
	       pkt->payload.param1,
	       pkt->payload.param2,
	       pkt->checksum,
	       pkt->etx);
}

static void write_device(int fd, struct k8090_packet *pkt)
{
	int ret;

	pkt->payload.stx = PACKET_STX;
	pkt->checksum = do_checksum(&pkt->payload);
	pkt->etx = PACKET_ETX;

	if (debug)
		dump_packet("=>", pkt);

	ret = write(fd, pkt, sizeof(*pkt));
	if (ret < 0)
		error(1, errno, "write");
}

static void read_device(int fd, struct k8090_packet *pkt)
{
	int ret;

	ret = read(fd, pkt, sizeof(*pkt));
	if (ret < 0)
		error(1, errno, "read");

	if (debug)
		dump_packet("<=", pkt);

	if (do_checksum(&pkt->payload) != pkt->checksum)
		error(1, EINVAL, "bad checksum on read");
}

static inline void init_packet(struct k8090_packet *pkt, int command)
{
	memset(pkt, 0, sizeof(*pkt));
	pkt->payload.command = command;
}

static void power_on(int fd, int map)
{
	struct k8090_packet pkt;

	init_packet(&pkt, TURN_ON);
	pkt.payload.mask = map;
	write_device(fd, &pkt);
}

static void power_off(int fd, int map)
{
	struct k8090_packet pkt;

	init_packet(&pkt, TURN_OFF);
	pkt.payload.mask = map;
	write_device(fd, &pkt);
}

static void power_toggle(int fd)
{
	struct k8090_packet pkt;

	init_packet(&pkt, TOGGLE);
	pkt.payload.mask = toggle_map;
	write_device(fd, &pkt);
}

static void power_cycle(int fd)
{
	power_off(fd, cycle_map);
	sleep(2);
	power_on(fd, cycle_map);
}

static const char *get_alias_name(int relay)
{
	struct device_alias *alias;
	static char raw_name[3];

	for (alias = alias_list; alias; alias = alias->next) {
		if (alias->map & (1 << relay))
			return alias->name;
	}

	raw_name[0] = '#';
	raw_name[1] = relay + '0' + 1;
	raw_name[2] = '\0';

	return raw_name;
}

static void power_status(int fd)
{
	struct k8090_packet pkt;
	int n;

	init_packet(&pkt, GET_STATUS);
	write_device(fd, &pkt);
	read_device(fd, &pkt);

	for_each_relay(n, status_map) {
		printf("%-8s => %s%s\n", get_alias_name(n),
		       (pkt.payload.param1 & (1 << n)) ? "ON" : "--",
		       (pkt.payload.param2 & (1 << n)) ? " (TIMED)" : "");
	}
}

static void firmware_version(int fd)
{
	struct k8090_packet pkt;

	init_packet(&pkt, GET_VERSION);
	write_device(fd, &pkt);
	read_device(fd, &pkt);

	printf("%u.%d\n", pkt.payload.param1 - 16 + 2010, pkt.payload.param2);
}

int main(int argc, char *const *argv)
{
	const char *device = DEFAULT_DEVICE;
	int lindex, c, fd;

	for (;;) {
		lindex = -1;
		c = getopt_long_only(argc, argv, "", options, &lindex);
		if (c == EOF)
			break;
		switch (lindex) {
		case help_opt:
			usage();
			exit(0);
		case turnon_opt:
			parse_relay_list(optarg, &on_map);
			break;
		case turnoff_opt:
			if (optarg)
				parse_relay_list(optarg, &off_map);
			else
				off_map = 0xff;
			break;
		case toggle_opt:
			if (optarg)
				parse_relay_list(optarg, &toggle_map);
			else
				toggle_map = 0xff;
			break;
		case cycle_opt:
			parse_relay_list(optarg, &cycle_map);
			break;
		case status_opt:
			if (optarg)
				parse_relay_list(optarg, &status_map);
			else
				status_map = 0xff;
			break;
		case device_opt:
			device = optarg;
			break;
		case firmware_opt:
		case debug_opt:
			break;
		default:
			usage();
			exit(1);
		}
	}

	/* Off has precedence over power on and cycling. */
	cycle_map &= ~off_map;
	on_map &= ~off_map;
	/* Cycling leaves the relay on, no need to repeat. */
	on_map &= ~cycle_map;
	/* Toggle has precedence over all other switches. */
	on_map &= ~toggle_map;
	off_map &= ~toggle_map;
	cycle_map &= ~toggle_map;

	fd = open_device(device);

	if (query_firmware)
		firmware_version(fd);
		
	if (off_map)
		power_off(fd, off_map);

	if (on_map)
		power_on(fd, on_map);
		
	if (toggle_map)
		power_toggle(fd);
		
	if (cycle_map)
		power_cycle(fd);
		
	if (status_map) {
		load_alias_list();
		power_status(fd);
	}

	return 0;
}
