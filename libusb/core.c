/*
 * Core functions for libusb
 * Copyright (C) 2007 Daniel Drake <dsd@gentoo.org>
 * Copyright (c) 2001 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <features.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libusb.h"
#include "libusbi.h"

static struct list_head usb_devs;
struct list_head open_devs;

static int scan_device(char *busdir, const char *devnum)
{
	char path[PATH_MAX + 1];
	unsigned char raw_desc[DEVICE_DESC_LENGTH];
	struct libusb_dev *dev = malloc(sizeof(*dev));
	int fd = 0;
	int i;
	int r;
	int tmp;

	if (!dev)
		return -1;

	snprintf(path, PATH_MAX, "%s/%s", busdir, devnum);
	usbi_dbg("%s", path);
	fd = open(path, O_RDWR);
	if (!fd) {
		usbi_dbg("open '%s' failed, ret=%d errno=%d", path, fd, errno);
		r = -1;
		goto err;
	}

	r = read(fd, raw_desc, DEVICE_DESC_LENGTH);
	if (r < 0) {
		usbi_err("read failed ret=%d errno=%d", r, errno);
		goto err;
	}
	/* FIXME: short read handling? */

	usbi_parse_descriptor(raw_desc, "bbWbbbbWWWbbbb", &dev->desc);

	/* Now try to fetch the rest of the descriptors */
	if (dev->desc.bNumConfigurations > USB_MAXCONFIG) {
		usbi_err("too many configurations");
		r = -1;
		goto err;
	}

	if (dev->desc.bNumConfigurations < 1) {
		usbi_dbg("no configurations?");
		r = -1;
		goto err;
	}

	tmp = dev->desc.bNumConfigurations * sizeof(struct libusb_config_descriptor);
	dev->config = malloc(tmp);
	if (!dev->config) {
		r = -1;
		goto err;
	}

	memset(dev->config, 0, tmp);

	for (i = 0; i < dev->desc.bNumConfigurations; i++) {
		unsigned char buffer[8], *bigbuffer;
		struct libusb_config_descriptor config;

		/* Get the first 8 bytes to figure out what the total length is */
		r = read(fd, buffer, sizeof(buffer));
		if (r < sizeof(buffer)) {
			usbi_err("short descriptor read (%d/%d)", r, sizeof(buffer));
			goto err;
		}

		usbi_parse_descriptor(buffer, "bbw", &config);

		bigbuffer = malloc(config.wTotalLength);
		if (!bigbuffer)
			goto err;

		/* Read the rest of the config descriptor */
		memcpy(bigbuffer, buffer, sizeof(buffer));

		tmp = config.wTotalLength - 8;
		r = read(fd, bigbuffer + 8, tmp);
		if (r < tmp) {
			usbi_err("short descriptor read (%d/%d)", r, tmp);
			free(bigbuffer);
			goto err;
		}

		r = usbi_parse_configuration(&dev->config[i], bigbuffer);
		if (r > 0)
			usbi_warn("descriptor data still left\n");
		free(bigbuffer);
	}

	dev->nodepath = strdup(path);
	if (!dev->nodepath)
		goto err;

	usbi_dbg("found device %04x:%04x", dev->desc.idVendor, dev->desc.idProduct);
	list_add(&dev->list, &usb_devs);
	r = 0;

err:
	if (fd)
		close(fd);
	if (r < 0 && dev)
		free(dev);
	return r;
}

static int scan_busdir(const char *busnum)
{
	DIR *dir;
	char dirpath[PATH_MAX + 1];
	struct dirent *entry;

	snprintf(dirpath, PATH_MAX, "%s/%s", USBFS_PATH, busnum);
	usbi_dbg("%s", dirpath);
	dir = opendir(dirpath);
	if (!dir) {
		usbi_err("opendir '%s' failed, errno=%d", dirpath, errno);
		return -1;
	}

	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;
		/* deliberately ignoring errors due to valid unplug race conditions */
		scan_device(dirpath, entry->d_name);
	}

	return 0;
}

API_EXPORTED int libusb_find_devices(void)
{
	DIR *buses;
	struct dirent *entry;
	usbi_dbg("");

	buses = opendir(USBFS_PATH);
	if (!buses) {
		usbi_err("opendir buses failed errno=%d", errno);
		return -1;
	}

	while ((entry = readdir(buses))) {
		if (entry->d_name[0] == '.')
			continue;
		/* deliberately ignoring errors, valid race conditions exist
		 * e.g. unplugging of hubs in the middle of this loop*/
		scan_busdir(entry->d_name);
	}

	return 0;
}

API_EXPORTED struct libusb_dev *libusb_get_devices(void)
{
	if (list_empty(&usb_devs))
		return NULL;
	return list_entry(usb_devs.next, struct libusb_dev, list);
}

API_EXPORTED struct libusb_dev *libusb_dev_next(struct libusb_dev *dev)
{
	struct list_head *head = &dev->list;
	if (!head || head->next == &usb_devs)
		return NULL;
	return list_entry(head->next, struct libusb_dev, list);
}

API_EXPORTED struct libusb_dev_descriptor *libusb_dev_get_descriptor(
	struct libusb_dev *dev)
{
	return &dev->desc;
}

API_EXPORTED struct libusb_config_descriptor *libusb_dev_get_config(
	struct libusb_dev *dev)
{
	return dev->config;
}

API_EXPORTED struct libusb_dev_handle *libusb_open(struct libusb_dev *dev)
{
	struct libusb_dev_handle *devh;
	int fd;
	usbi_dbg("open %04x:%04x", dev->desc.idVendor, dev->desc.idProduct);

	fd = open(dev->nodepath, O_RDWR);
	if (!fd) {
		usbi_err("open failed, code %d errno %d", fd, errno);
		return NULL;
	}

	devh = malloc(sizeof(*devh));
	if (!devh) {
		close(fd);
		return NULL;
	}

	devh->fd = fd;
	devh->dev = dev;
	list_add(&devh->list, &open_devs);
	usbi_add_pollfd(fd, POLLOUT);
	return devh;
}

static void do_close(struct libusb_dev_handle *devh)
{
	usbi_remove_pollfd(devh->fd);
	close(devh->fd);
}

API_EXPORTED void libusb_close(struct libusb_dev_handle *devh)
{
	if (!devh)
		return;
	usbi_dbg("");

	list_del(&devh->list);
	do_close(devh);
	free(devh);
}

API_EXPORTED struct libusb_dev *libusb_devh_get_dev(struct libusb_dev_handle *devh)
{
	return devh->dev;
}

API_EXPORTED int libusb_claim_interface(struct libusb_dev_handle *dev,
	int iface)
{
	int r;
	usbi_dbg("interface %d", iface);
	
	r = ioctl(dev->fd, IOCTL_USB_CLAIMINTF, &iface);
	if (r < 0)
		usbi_err("claim interface failed, error %d", r);
	return r;
}

API_EXPORTED int libusb_release_interface(struct libusb_dev_handle *dev,
	int iface)
{
	int r;
	usbi_dbg("interface %d", iface);

	r = ioctl(dev->fd, IOCTL_USB_RELEASEINTF, &iface);
	if (r < 0)
		usbi_err("release interface failed, error %d", r);
	return r;
}

API_EXPORTED int libusb_init(void)
{
	/* FIXME: find correct usb node path */
	usbi_dbg("");
	list_init(&usb_devs);
	list_init(&open_devs);
	usbi_io_init();
	return 0;
}

API_EXPORTED void libusb_exit(void)
{
	struct libusb_dev_handle *devh;
	usbi_dbg("");
	if (!list_empty(&open_devs)) {
		usbi_dbg("naughty app left some devices open!\n");
		list_for_each_entry(devh, &open_devs, list)
			do_close(devh);
	}
}

API_EXPORTED size_t libusb_get_pollfds(struct libusb_pollfd **pollfds)
{
	struct libusb_dev_handle *devh;
	struct libusb_pollfd *ret;
	size_t cnt = 0;
	size_t i = 0;

	/* count number of open devices */
	list_for_each_entry(devh, &open_devs, list)
		cnt++;

	/* create array */
	ret = calloc(cnt, sizeof(struct libusb_pollfd));
	if (!ret)
		return -ENOMEM;

	/* add fds */
	list_for_each_entry(devh, &open_devs, list) {
		ret[i++].fd = devh->fd;
		ret[i].events = POLLOUT;
	}
	
	*pollfds = ret;
	return cnt;
}

void usbi_log(enum usbi_log_level level, const char *function,
	const char *format, ...)
{
	va_list args;
	FILE *stream = stdout;
	const char *prefix;

	switch (level) {
	case LOG_LEVEL_INFO:
		prefix = "info";
		break;
	case LOG_LEVEL_WARNING:
		stream = stderr;
		prefix = "warning";
		break;
	case LOG_LEVEL_ERROR:
		stream = stderr;
		prefix = "error";
		break;
	case LOG_LEVEL_DEBUG:
		stream = stderr;
		prefix = "debug";
		break;
	default:
		stream = stderr;
		prefix = "unknown";
		break;
	}

	fprintf(stream, "libusb:%s [%s] ", prefix, function);

	va_start (args, format);
	vfprintf(stream, format, args);
	va_end (args);

	fprintf(stream, "\n");
}
