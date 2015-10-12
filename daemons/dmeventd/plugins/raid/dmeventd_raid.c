/*
 * Copyright (C) 2005-2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "dmeventd_lvm.h"
#include "libdevmapper-event.h"

DM_EVENT_LOG_FN("raid")

/* FIXME Reformat to 80 char lines. */

/*
 * run_repair is a close copy to
 * plugins/mirror/dmeventd_mirror.c:_remove_failed_devices()
 */
static int run_repair(const char *device)
{
	int r;
#define CMD_SIZE 256	/* FIXME Use system restriction */
	char cmd_str[CMD_SIZE];

	if (!dmeventd_lvm2_command(dmeventd_lvm2_pool(), cmd_str, sizeof(cmd_str),
				  "lvscan --cache", device))
		return -1;

	r = dmeventd_lvm2_run(cmd_str);

	if (!r)
		log_info("Re-scan of RAID device %s failed.", device);

	if (!dmeventd_lvm2_command(dmeventd_lvm2_pool(), cmd_str, sizeof(cmd_str),
				  "lvconvert --config devices{ignore_suspended_devices=1} "
				  "--repair --use-policies", device))
		return -1;

	/* if repair goes OK, report success even if lvscan has failed */
	r = dmeventd_lvm2_run(cmd_str);

	if (!r)
		log_info("Repair of RAID device %s failed.", device);

	return (r) ? 0 : -1;
}

static int _process_raid_event(char *params, const char *device)
{
	int i, n, failure = 0;
	char *p, *a[4];
	char *raid_type;
	char *num_devices;
	char *health_chars;
	char *resync_ratio;

	/*
	 * RAID parms:     <raid_type> <#raid_disks> \
	 *                 <health chars> <resync ratio>
	 */
	if (!dm_split_words(params, 4, 0, a)) {
		log_error("Failed to process status line for %s.", device);
		return -EINVAL;
	}
	raid_type = a[0];
	num_devices = a[1];
	health_chars = a[2];
	resync_ratio = a[3];

	if (!(n = atoi(num_devices))) {
		log_error("Failed to parse number of devices for %s: %s.",
			  device, num_devices);
		return -EINVAL;
	}

	for (i = 0; i < n; i++) {
		switch (health_chars[i]) {
		case 'A':
			/* Device is 'A'live and well */
		case 'a':
			/* Device is 'a'live, but not yet in-sync */
			break;
		case 'D':
			log_error("Device #%d of %s array, %s, has failed.",
				  i, raid_type, device);
			failure++;
			break;
		default:
			/* Unhandled character returned from kernel */
			break;
		}
		if (failure)
			return run_repair(device);
	}

	p = strstr(resync_ratio, "/");
	if (!p) {
		log_error("Failed to parse resync_ratio for %s: %s.",
			  device, resync_ratio);
		return -EINVAL;
	}
	p[0] = '\0';
	log_info("%s array, %s, is %s in-sync.",
		 raid_type, device, strcmp(resync_ratio, p+1) ? "not" : "now");

	return 0;
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask event __attribute__((unused)),
		   void **user)
{
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	const char *device = dm_task_get_name(dmt);

	dmeventd_lvm2_lock();

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);

		if (!target_type) {
			log_info("%s mapping lost.", device);
			continue;
		}

		if (strcmp(target_type, "raid")) {
			log_info("%s has non-raid portion.", device);
			continue;
		}

		if (_process_raid_event(params, device))
			log_error("Failed to process event for %s.",
				  device);
	} while (next);

	dmeventd_lvm2_unlock();
}

int register_device(const char *device,
		    const char *uuid __attribute__((unused)),
		    int major __attribute__((unused)),
		    int minor __attribute__((unused)),
		    void **user)
{
	if (!dmeventd_lvm2_init())
		return 0;

	log_info("Monitoring RAID device %s for events.", device);

	return 1;
}

int unregister_device(const char *device,
		      const char *uuid __attribute__((unused)),
		      int major __attribute__((unused)),
		      int minor __attribute__((unused)),
		      void **user)
{
	log_info("No longer monitoring RAID device %s for events.",
		 device);
	dmeventd_lvm2_exit();

	return 1;
}
