/*
 * Bankshot2 kernel device driver
 *
 * 2/26/2014 - Andiry Xu <jix024@cs.ucsd.edu>
 *
 */

/*
 * Bankshot2 Timing measurement.
 */

#include "bankshot2.h"

const char *Timingstring[TIMING_NUM] = 
{
	"cache_data",
	"get_extent",
	"xip_read",
	"xip_write",
	"allocation",
	"check_mmap",
	"mmap",
	"copy_to_cache_for_read",
	"copy_to_cache_for_write",
	"copy_from_cache",
	"copy_to_user",
	"copy_from_user",
	"add_extent",
	"evict_cache",
};

void bankshot2_print_time_stats(struct bankshot2_device *bs2_dev)
{
	int i;

	bs2_info("Bankshot2 kernel timing stats:\n");
	for (i = 0; i < TIMING_NUM; i++) {
		if (measure_timing) {
			bs2_info("%s: count %llu, timing %llu, average %llu\n",
				Timingstring[i],
				bs2_dev->countstats[i],
				bs2_dev->timingstats[i],
				bs2_dev->countstats[i] ?
				bs2_dev->timingstats[i] /
					bs2_dev->countstats[i] : 0);
		} else {
			bs2_info("%s: count %llu\n",
				Timingstring[i],
				bs2_dev->countstats[i]);
		}
	}
}

void bankshot2_clear_time_stats(struct bankshot2_device *bs2_dev)
{
	int i;

	for (i = 0; i < TIMING_NUM; i++) {
		bs2_dev->countstats[i] = 0;
		bs2_dev->timingstats[i] = 0;
	}
}

