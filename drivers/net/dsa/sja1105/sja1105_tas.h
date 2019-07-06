/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#ifndef _SJA1105_TAS_H
#define _SJA1105_TAS_H

#if IS_ENABLED(CONFIG_NET_DSA_SJA1105_TAS)

void sja1105_tas_config_work(struct work_struct *work);

int sja1105_setup_taprio(struct dsa_switch *ds, int port,
			 struct tc_taprio_qopt_offload *qopt);

#else

#define sja1105_tas_config_work NULL

#define sja1105_setup_taprio NULL

#endif /* IS_ENABLED(CONFIG_NET_DSA_SJA1105_TAS) */

#endif /* _SJA1105_TAS_H */
