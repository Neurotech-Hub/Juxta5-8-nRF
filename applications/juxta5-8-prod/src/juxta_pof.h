#ifndef JUXTA_POF_H_
#define JUXTA_POF_H_

#include <stdbool.h>
#include <stdint.h>

/* POFCON supply-droop witness (crash analysis §5.4).
 *
 * Arms the nRF52840 power-fail comparator so a VDDH sag below the warning
 * threshold fires an IRQ while the CPU still has headroom to write a
 * `.noinit` witness struct.  The struct survives soft resets (DOG / LOCKUP /
 * SREQ) but is wiped by a true POR — which is itself informative: a clean
 * POR with *no* witness is ESD-class or a full supply collapse, while a
 * witness followed by a reset proves the supply sagged first.
 *
 * The witness from the previous life is snapshotted at juxta_pof_init() and
 * exposed via juxta_pof_prev_life() so main.c can emit a `pof_witness` JXS
 * row once the clock is valid.
 */

struct juxta_pof_witness
{
	uint32_t count;			/* POFWARN events this life */
	int64_t first_uptime_ms;	/* uptime at first event */
	int64_t last_uptime_ms;		/* uptime at most recent event */
	uint32_t first_unix;		/* clock at first event (0 if unset) */
	uint32_t last_unix;		/* clock at most recent event (0 if unset) */
};

/* Snapshot any witness left by the previous life, reset the retained struct
 * for this life, and arm the comparator.  Call once, early in main(). */
void juxta_pof_init(void);

/* True if POFWARN has fired at least once since this boot's juxta_pof_init(). */
bool juxta_pof_hit_this_life(void);

/* Witness carried over from the previous life (survived a soft reset).
 * Returns true and fills `out` when one exists; consume-once semantics via
 * juxta_pof_prev_life_clear() after the JXS row has been appended. */
bool juxta_pof_prev_life(struct juxta_pof_witness *out);
void juxta_pof_prev_life_clear(void);

/* Disable the comparator (called on shelf entry; System OFF disables the
 * POF block anyway, this just makes the teardown explicit). */
void juxta_pof_disarm(void);

#endif /* JUXTA_POF_H_ */
