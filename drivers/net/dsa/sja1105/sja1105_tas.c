// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include "sja1105.h"

#define SJA1105_TAS_CLKSRC_DISABLED	0
#define SJA1105_TAS_CLKSRC_STANDALONE	1
#define SJA1105_TAS_CLKSRC_AS6802	2
#define SJA1105_TAS_CLKSRC_PTP		3
#define SJA1105_GATE_MASK		GENMASK_ULL(SJA1105_NUM_TC - 1, 0)
#define SJA1105_TAS_MAX_DELTA		BIT(19)

/* This is not a preprocessor macro because the "ns" argument may or may not be
 * u64 at caller side. This ensures it is properly type-cast before div_u64.
 */
static u64 sja1105_tas_cycles(u64 ns)
{
	return div_u64(ns, 200);
}

/* Lo and behold: the egress scheduler from hell.
 *
 * At the hardware level, the Time-Aware Shaper holds a global linear arrray of
 * all schedule entries for all ports. These are the Gate Control List (GCL)
 * entries, let's call them "timeslots" for short. This linear array of
 * timeslots is held in BLK_IDX_SCHEDULE.
 *
 * Then there are a maximum of 8 "execution threads" inside the switch, which
 * iterate cyclically through the "schedule". Each "cycle" has an entry point
 * and an exit point, both being timeslot indices in the schedule table. The
 * hardware calls each cycle a "subschedule".
 *
 * Subschedule (cycle) i starts when PTPCLKVAL >= BLK_IDX_SCHEDULE_ENTRY_POINTS[i].delta.
 * The hardware scheduler iterates BLK_IDX_SCHEDULE with a k ranging from
 * k = BLK_IDX_SCHEDULE_ENTRY_POINTS[i].address to
 * k = BLK_IDX_SCHEDULE_PARAMS.subscheind[i]
 * For each schedule entry (timeslot) k, the engine executes the gate control
 * list entry for the duration of BLK_IDX_SCHEDULE[k].delta.
 *
 *         +---------+
 *         |         | BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS
 *         +---------+
 *              |
 *              | .actsubsch
 *              +-----------------+
 *                                |
 *                                |
 *                                |
 *  BLK_IDX_SCHEDULE_ENTRY_POINTS v
 *                 +---------+---------+
 *                 | cycle 0 | cycle 1 |
 *                 +---------+---------+
 *                   |  |         |  |
 *  +----------------+  |         |  +-----------------------------------------------+
 *  |   .subschindx     |         |                          .subschindx             |
 *  |                   |         +-------------------+                              |
 *  |          .address |           .address          |                              |
 *  |                   |                             |                              |
 *  |                   |                             |                              |
 *  |  BLK_IDX_SCHEDULE v                             v                              |
 *  |              +---------+---------+---------+---------+---------+---------+     |
 *  |              | entry 0 | entry 1 | entry 2 | entry 3 | entry 4 | entry 5 |     |
 *  |              +---------+---------+---------+---------+---------+---------+     |
 *  |                                       ^                           ^  ^  ^      |
 *  |                                       |                           |  |  |      |
 *  |         +-----------------------------+                           |  |  |      |
 *  |         |                                                         |  |  |      |
 *  |         |                  +--------------------------------------+  |  |      |
 *  |         |                  |                                         |  |      |
 *  |         |                  |                 +-----------------------+  |      |
 *  |         |                  |                 |                          |      |
 *  |         |                  |                 | BLK_IDX_SCHEDULE_PARAMS  |      |
 *  | +----------------------------------------------------------------------------+ |
 *  | | .subscheind[0] <= .subscheind[1] <= .subscheind[2] <= ... <= subscheind[7] | |
 *  | +----------------------------------------------------------------------------+ |
 *  |         ^                  ^                                                   |
 *  |         |                  |                                                   |
 *  +---------+                  +---------------------------------------------------+
 *
 *  In the above picture there are two subschedules (cycles):
 *
 *  - cycle 0: iterates the schedule table from 0 to 2 (and back)
 *  - cycle 1: iterates the schedule table from 3 to 5 (and back)
 *
 *  All other possible execution threads must be marked as unused by making
 *  their "subschedule end index" (subscheind) equal to the last valid
 *  subschedule's end index (in this case 5).
 */
static int sja1105_init_scheduling(struct sja1105_private *priv)
{
	struct sja1105_schedule_entry_points_entry *schedule_entry_points;
	struct sja1105_schedule_entry_points_params_entry
					*schedule_entry_points_params;
	struct sja1105_schedule_params_entry *schedule_params;
	struct sja1105_schedule_entry *schedule;
	struct sja1105_table *table;
	int subscheind[8] = {0};
	int schedule_start_idx;
	u64 entry_point_delta;
	int schedule_end_idx;
	int num_entries = 0;
	int num_cycles = 0;
	int cycle = 0;
	int i, k = 0;
	int port;

	/* Discard previous Schedule Table */
	table = &priv->static_config.tables[BLK_IDX_SCHEDULE];
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	/* Discard previous Schedule Entry Points Parameters Table */
	table = &priv->static_config.tables[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS];
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	/* Discard previous Schedule Parameters Table */
	table = &priv->static_config.tables[BLK_IDX_SCHEDULE_PARAMS];
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	/* Discard previous Schedule Entry Points Table */
	table = &priv->static_config.tables[BLK_IDX_SCHEDULE_ENTRY_POINTS];
	if (table->entry_count) {
		kfree(table->entries);
		table->entry_count = 0;
	}

	/* Figure out the dimensioning of the problem */
	for (port = 0; port < SJA1105_NUM_PORTS; port++) {
		if (priv->tas_config[port]) {
			num_entries += priv->tas_config[port]->num_entries;
			num_cycles++;
		}
	}

	/* Nothing to do */
	if (!num_cycles)
		return 0;

	/* Pre-allocate space in the static config tables */

	/* Schedule Table */
	table = &priv->static_config.tables[BLK_IDX_SCHEDULE];
	table->entries = kcalloc(num_entries, table->ops->unpacked_entry_size,
				 GFP_ATOMIC);
	if (!table->entries)
		return -ENOMEM;
	table->entry_count = num_entries;
	schedule = table->entries;

	/* Schedule Points Parameters Table */
	table = &priv->static_config.tables[BLK_IDX_SCHEDULE_ENTRY_POINTS_PARAMS];
	table->entries = kcalloc(SJA1105_MAX_SCHEDULE_ENTRY_POINTS_PARAMS_COUNT,
				 table->ops->unpacked_entry_size, GFP_ATOMIC);
	if (!table->entries)
		return -ENOMEM;
	table->entry_count = SJA1105_MAX_SCHEDULE_ENTRY_POINTS_PARAMS_COUNT;
	schedule_entry_points_params = table->entries;

	/* Schedule Parameters Table */
	table = &priv->static_config.tables[BLK_IDX_SCHEDULE_PARAMS];
	table->entries = kcalloc(SJA1105_MAX_SCHEDULE_PARAMS_COUNT,
				 table->ops->unpacked_entry_size, GFP_ATOMIC);
	if (!table->entries)
		return -ENOMEM;
	table->entry_count = SJA1105_MAX_SCHEDULE_PARAMS_COUNT;
	schedule_params = table->entries;

	/* Schedule Entry Points Table */
	table = &priv->static_config.tables[BLK_IDX_SCHEDULE_ENTRY_POINTS];
	table->entries = kcalloc(num_cycles, table->ops->unpacked_entry_size,
				 GFP_ATOMIC);
	if (!table->entries)
		return -ENOMEM;
	table->entry_count = num_cycles;
	schedule_entry_points = table->entries;

	/* Finally start populating the static config tables */
	schedule_entry_points_params->clksrc = SJA1105_TAS_CLKSRC_STANDALONE;
	schedule_entry_points_params->actsubsch = num_cycles - 1;

	for (port = 0; port < SJA1105_NUM_PORTS; port++) {
		const struct tc_taprio_qopt_offload *tas_config;

		tas_config = priv->tas_config[port];
		if (!tas_config)
			continue;

		schedule_start_idx = k;
		schedule_end_idx = k + tas_config->num_entries - 1;
		/* TODO this is only a relative base time for the subschedule
		 * (relative to PTPSCHTM). But as we're using standalone and
		 * not PTP clock as time reference, leave it like this for now.
		 * Later we'll have to enforce that all ports' base times are
		 * within SJA1105_TAS_MAX_DELTA 200ns cycles of one another.
		 */
		entry_point_delta = sja1105_tas_cycles(tas_config->base_time);

		schedule_entry_points[cycle].subschindx = cycle;
		schedule_entry_points[cycle].delta = entry_point_delta;
		schedule_entry_points[cycle].address = schedule_start_idx;

		for (i = cycle; i < 8; i++)
			subscheind[i] = schedule_end_idx;

		for (i = 0; i < tas_config->num_entries; i++, k++) {
			u64 delta_ns = tas_config->entries[i].interval;

			schedule[k].delta = sja1105_tas_cycles(delta_ns);
			schedule[k].destports = BIT(port);
			schedule[k].resmedia_en = true;
			schedule[k].resmedia = SJA1105_GATE_MASK &
					~tas_config->entries[i].gate_mask;
		}
		cycle++;
	}

	for (i = 0; i < 8; i++)
		schedule_params->subscheind[i] = subscheind[i];

	return 0;
}

static struct tc_taprio_qopt_offload
*tc_taprio_qopt_offload_copy(const struct tc_taprio_qopt_offload *from)
{
	struct tc_taprio_qopt_offload *to;
	size_t size;

	size = sizeof(*from) +
		from->num_entries * sizeof(struct tc_taprio_sched_entry);

	to = kzalloc(size, GFP_ATOMIC);
	if (!to)
		return ERR_PTR(-ENOMEM);

	memcpy(to, from, size);

	return to;
}

/* Be there 2 port subschedules, each executing an arbitrary number of gate
 * open/close events cyclically.
 * None of those gate events must ever occur at the exact same time, otherwise
 * the switch is known to act in exotically strange ways.
 * However the hardware doesn't bother performing these integrity checks - the
 * designers probably said "nah, let's leave that to the experts" - oh well,
 * now we're the experts.
 * So here we are with the task of validating whether the new @qopt has any
 * conflict with the already established TAS configuration in priv->tas_config.
 * We already know the other ports are in harmony with one another, otherwise
 * we wouldn't have saved them.
 * Each gate event executes periodically, with a period of @cycle_time and a
 * phase given by its cycle's @base_time plus its offset within the cycle
 * (which in turn is given by the length of the events prior to it).
 * There are two aspects to possible collisions:
 * - Collisions within one cycle's (actually the longest cycle's) time frame.
 *   For that, we need to compare the cartesian product of each possible
 *   occurrence of each event within one cycle time.
 * - Collisions in the future. Events may not collide within one cycle time,
 *   but if two port schedules don't have the same periodicity (aka the cycle
 *   times aren't multiples of one another), they surely will some time in the
 *   future (actually they will collide an infinite amount of times).
 */
static bool
sja1105_tas_check_conflicts(struct sja1105_private *priv,
			    const struct tc_taprio_qopt_offload *qopt)
{
	int port;

	for (port = 0; port < SJA1105_NUM_PORTS; port++) {
		const struct tc_taprio_qopt_offload *tas_config;
		u64 max_cycle_time, min_cycle_time;
		u64 delta1, delta2;
		u64 rbt1, rbt2;
		u64 stop_time;
		u64 t1, t2;
		int i, j;
		s32 rem;

		tas_config = priv->tas_config[port];

		if (!tas_config)
			continue;

		/* Check if the two cycle times are multiples of one another.
		 * If they aren't, then they will surely collide.
		 */
		max_cycle_time = max(tas_config->cycle_time, qopt->cycle_time);
		min_cycle_time = min(tas_config->cycle_time, qopt->cycle_time);
		div_u64_rem(max_cycle_time, min_cycle_time, &rem);
		if (rem)
			return true;

		/* Calculate the "reduced" base time of each of the two cycles
		 * (transposed back as close to 0 as possible) by dividing to
		 * the cycle time.
		 */
		div_u64_rem(tas_config->base_time, tas_config->cycle_time,
			    &rem);
		rbt1 = rem;

		div_u64_rem(qopt->base_time, qopt->cycle_time, &rem);
		rbt2 = rem;

		stop_time = max_cycle_time + max(rbt1, rbt2);

		/* delta1 is the relative base time of each GCL entry within
		 * the established ports' TAS config.
		 */
		for (i = 0, delta1 = 0;
		     i < tas_config->num_entries;
		     delta1 += tas_config->entries[i].interval, i++) {

			/* delta2 is the relative base time of each GCL entry
			 * within the newly added TAS config.
			 */
			for (j = 0, delta2 = 0;
			     j < qopt->num_entries;
			     delta2 += qopt->entries[j].interval, j++) {

				/* t1 follows all possible occurrences of the
				 * established ports' GCL entry i within the
				 * first cycle time.
				 */
				for (t1 = rbt1 + delta1;
				     t1 <= stop_time;
				     t1 += tas_config->cycle_time) {

					/* t2 follows all possible occurrences
					 * of the newly added GCL entry j
					 * within the first cycle time.
					 */
					for (t2 = rbt2 + delta2;
					     t2 <= stop_time;
					     t2 += qopt->cycle_time) {

						if (t1 == t2) {
							dev_warn(priv->ds->dev,
								 "GCL entry %d collides with entry %d of port %d\n",
								 j, i, port);
							return true;
						}
					}
				}
			}
		}
	}

	return false;
}

#define to_sja1105(d) \
	container_of((d), struct sja1105_private, tas_config_work)

void sja1105_tas_config_work(struct work_struct *work)
{
	struct sja1105_private *priv = to_sja1105(work);
	struct dsa_switch *ds = priv->ds;
	int rc;

	rc = sja1105_static_config_reload(priv);
	if (rc)
		dev_err(ds->dev, "Failed to change scheduling settings\n");
}

int sja1105_setup_taprio(struct dsa_switch *ds, int port,
			 const struct tc_taprio_qopt_offload *qopt)
{
	struct tc_taprio_qopt_offload *tas_config;
	struct sja1105_private *priv = ds->priv;
	int rc;
	int i;

	/* Can't change an already configured port (must delete qdisc first).
	 * Can't delete the qdisc from an unconfigured port.
	 */
	if (!!priv->tas_config[port] == qopt->enable)
		return -EINVAL;

	if (!qopt->enable) {
		kfree(priv->tas_config[port]);
		priv->tas_config[port] = NULL;
		rc = sja1105_init_scheduling(priv);
		if (rc < 0)
			return rc;

		schedule_work(&priv->tas_config_work);
		return 0;
	}

	/* What is this? */
	if (qopt->cycle_time_extension)
		return -ENOTSUPP;

	if (!sja1105_tas_cycles(qopt->base_time)) {
		dev_err(ds->dev, "A base time of zero is not hardware-allowed\n");
		return -ERANGE;
	}

	tas_config = tc_taprio_qopt_offload_copy(qopt);
	if (IS_ERR_OR_NULL(tas_config))
		return PTR_ERR(tas_config);

	if (!tas_config->cycle_time) {
		for (i = 0; i < tas_config->num_entries; i++) {
			u64 delta_ns = tas_config->entries[i].interval;
			u64 delta_cycles = sja1105_tas_cycles(delta_ns);
			bool too_long, too_short;

			/* The cycle_time may not be provided. In that case it
			 * will be sum of all time interval of the entries in
			 * the schedule.
			 */
			tas_config->cycle_time += delta_ns;

			too_long = (delta_cycles >= SJA1105_TAS_MAX_DELTA);
			too_short = (delta_cycles == 0);
			if (too_long || too_short) {
				dev_err(priv->ds->dev,
					"Interval %llu too %s for GCL entry %d\n",
					delta_ns, too_long ? "long" : "short", i);
				return -ERANGE;
			}
		}
	}

	if (sja1105_tas_check_conflicts(priv, tas_config)) {
		kfree(tas_config);
		return -ERANGE;
	}

	priv->tas_config[port] = tas_config;

	rc = sja1105_init_scheduling(priv);
	if (rc < 0)
		return rc;

	schedule_work(&priv->tas_config_work);
	return 0;
}
