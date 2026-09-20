/* ================================================================
 * stats.c: Latency statistics (histogram + percentiles) and
 * response-time schedulability analysis.
 *
 * Determinism is a property of the tail of the latency distribution,
 * not the mean, so we record a full histogram and report
 * p50 / p99 / p99.9 / max in addition to min and mean.
 * ================================================================ */

#include "rtos.h"
#include "semihosting.h"

/* Core clock (MHz) for the cycles->microseconds conversion. Set by
 * the Makefile (-DRTOS_CPU_MHZ): 8 for the QEMU model, 168 for HW. */
#ifndef RTOS_CPU_MHZ
#define RTOS_CPU_MHZ  8u
#endif

/* Context-switch statistics, populated from PendSV. */
rtos_stats_t rtos_switch_stats;

/* ---- Small integer -> string helpers (no libc) ----------------- */
static void put_u32(uint32_t v)
{
    char buf[12];
    int i = 11;
    buf[i--] = '\0';
    if (v == 0) { buf[i--] = '0'; }
    while (v > 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    sh_puts(&buf[i + 1]);
}

/* Print a cycle count converted to microseconds with two decimals
 * (cycles / MHz = microseconds). */
static void put_us(uint32_t cycles)
{
    uint32_t hundredths = (uint32_t)(((uint64_t)cycles * 100u) / RTOS_CPU_MHZ);
    put_u32(hundredths / 100u);
    sh_puts(".");
    uint32_t frac = hundredths % 100u;
    if (frac < 10) { sh_puts("0"); }
    put_u32(frac);
}

/* ================================================================
 * Histogram
 * ================================================================ */
void rtos_stats_init(rtos_stats_t *s, uint32_t bin_width)
{
    s->count     = 0;
    s->min       = 0xFFFFFFFFu;
    s->max       = 0;
    s->sum       = 0;
    s->bin_width = (bin_width == 0) ? 1u : bin_width;
    for (uint32_t i = 0; i <= RTOS_STAT_BINS; i++) {
        s->hist[i] = 0;
    }
}

void rtos_stats_add(rtos_stats_t *s, uint32_t sample)
{
    s->count++;
    s->sum += sample;
    if (sample < s->min) { s->min = sample; }
    if (sample > s->max) { s->max = sample; }

    uint32_t bin = sample / s->bin_width;
    if (bin >= RTOS_STAT_BINS) { bin = RTOS_STAT_BINS; }  /* overflow bin */
    s->hist[bin]++;
}

/* Percentile in per-mille (e.g. 500=p50, 990=p99, 999=p99.9). Returns
 * the upper edge of the bin in which the percentile falls.
 *
 * If the percentile falls in the OVERFLOW bin, the upper edge is a
 * fiction, the bin has no upper edge, so s->max is returned
 * instead. Returning (RTOS_STAT_BINS+1)*bin_width there is how a
 * saturated histogram comes to report the same plausible-looking
 * number for every subject in a comparison, which reads as "these
 * designs are identical" when it means "the instrument was out of
 * range". rtos_stats_report() flags the condition explicitly; see
 * rtos_stats_overflow(). */
uint32_t rtos_stats_percentile(const rtos_stats_t *s, uint32_t permille)
{
    if (s->count == 0) { return 0; }
    uint64_t target = ((uint64_t)s->count * permille) / 1000u;
    if (target == 0) { target = 1; }

    uint64_t cum = 0;
    for (uint32_t i = 0; i <= RTOS_STAT_BINS; i++) {
        cum += s->hist[i];
        if (cum >= target) {
            if (i == RTOS_STAT_BINS) {
                return s->max;               /* overflow bin: no edge */
            }
            return (i + 1u) * s->bin_width;  /* upper edge of the bin */
        }
    }
    return s->max;
}

/* Number of samples that landed above the histogram's range. Any
 * non-zero value means the percentiles are approximations and the
 * bin width was chosen too small for this data. */
uint32_t rtos_stats_overflow(const rtos_stats_t *s)
{
    return s->hist[RTOS_STAT_BINS];
}

void rtos_stats_report(const char *label, const rtos_stats_t *s)
{
    sh_puts(label);
    sh_puts(": n=");        put_u32(s->count);
    if (s->count == 0) { sh_puts(" (no samples)\r\n"); return; }

    uint32_t mean  = (uint32_t)(s->sum / s->count);
    uint32_t p50   = rtos_stats_percentile(s, 500);
    uint32_t p99   = rtos_stats_percentile(s, 990);
    uint32_t p999  = rtos_stats_percentile(s, 999);

    /* Cycles first ... */
    sh_puts(" min=");   put_u32(s->min);
    sh_puts(" mean=");  put_u32(mean);
    sh_puts(" p50=");   put_u32(p50);
    sh_puts(" p99=");   put_u32(p99);
    sh_puts(" p99.9="); put_u32(p999);
    sh_puts(" max=");   put_u32(s->max);
    sh_puts(" cyc\r\n");

    /* A saturated histogram must never be reported as if it were a
     * clean one: say so, in the same breath as the numbers it
     * affects, rather than leaving it to be noticed. */
    if (rtos_stats_overflow(s) != 0) {
        sh_puts("            *** ");
        put_u32(rtos_stats_overflow(s));
        sh_puts(" of ");
        put_u32(s->count);
        sh_puts(" samples exceeded the histogram range (bin_width=");
        put_u32(s->bin_width);
        sh_puts(" x ");
        put_u32(RTOS_STAT_BINS);
        sh_puts(" bins). min/mean/max are exact; percentiles are\r\n"
                "                approximate - increase bin_width. ***\r\n");
    }

    /* ... then the same figures converted to microseconds. */
    sh_puts("            (us)  min=");   put_us(s->min);
    sh_puts(" mean=");  put_us(mean);
    sh_puts(" p50=");   put_us(p50);
    sh_puts(" p99=");   put_us(p99);
    sh_puts(" p99.9="); put_us(p999);
    sh_puts(" max=");   put_us(s->max);
    sh_puts("\r\n");
}

/* Dump the non-empty histogram bins as "[lo,hi): count" so the shape
 * of the distribution (not just its percentiles) is visible. */
void rtos_stats_histogram(const char *label, const rtos_stats_t *s)
{
    sh_puts(label);
    sh_puts(" histogram (cycles):\r\n");
    if (s->count == 0) { sh_puts("  (no samples)\r\n"); return; }
    for (uint32_t i = 0; i <= RTOS_STAT_BINS; i++) {
        if (s->hist[i] == 0) { continue; }
        sh_puts("  [");
        put_u32(i * s->bin_width);
        if (i == RTOS_STAT_BINS) {
            sh_puts("+       ) ");
        } else {
            sh_puts(",");
            put_u32((i + 1u) * s->bin_width);
            sh_puts(") ");
        }
        put_u32(s->hist[i]);
        sh_puts("\r\n");
    }
}

/* PendSV calls this once per context switch. Kept tiny: it runs in
 * exception context with interrupts disabled.
 *
 * A context switch on this kernel costs a few hundred cycles at most.
 * Samples far larger than that cannot be real; on QEMU they are the
 * SysTick-fallback counter momentarily reading backwards across a
 * timer reload (see rtos_cycles()). We reject them so they do not
 * pollute the distribution. On hardware (monotonic DWT) this branch
 * never triggers. */
#define RTOS_SWITCH_SANE_MAX  20000u
void rtos_stats_record_switch(uint32_t cycles)
{
    if (rtos_switch_stats.bin_width == 0) {
        rtos_stats_init(&rtos_switch_stats, 8u);  /* 8 cycles/bin */
    }
    if (cycles > RTOS_SWITCH_SANE_MAX) {
        return;   /* counter-wrap artifact, not a real measurement */
    }
    rtos_stats_add(&rtos_switch_stats, cycles);
}

/* ================================================================
 * Response-time schedulability analysis (fixed-priority, RMA)
 *
 * 1) Liu & Layland utilisation bound: sum(C_i/T_i) <= n(2^(1/n)-1)
 *    (sufficient, not necessary).
 * 2) Exact response-time analysis: R_i = C_i + sum_{hp(i)} ceil(R_i/T_j)*C_j
 *    iterated to a fixed point; schedulable iff R_i <= T_i for all i.
 *
 * Tasks are assumed rate-monotonic (shorter period = higher priority).
 * All times are in the same integer unit (ticks).
 * ================================================================ */
static void put_frac3(uint32_t num, uint32_t den)   /* prints num/den to 3dp */
{
    if (den == 0) { den = 1; }
    uint32_t whole = num / den;
    uint32_t milli = (uint32_t)(((uint64_t)(num - whole * den) * 1000u) / den);
    put_u32(whole); sh_puts(".");
    if (milli < 100) sh_puts("0");
    if (milli < 10)  sh_puts("0");
    put_u32(milli);
}

int rtos_schedulability_report(const rtos_taskspec_t *tasks, uint32_t n)
{
    sh_puts("\r\n--- Schedulability analysis (RMA) ---\r\n");

    /* Utilisation, scaled by 1e6 to stay in integer math. */
    uint64_t U_ppm = 0;
    for (uint32_t i = 0; i < n; i++) {
        U_ppm += ((uint64_t)tasks[i].wcet * 1000000u) / tasks[i].period;
        sh_puts("  "); sh_puts(tasks[i].name);
        sh_puts(": C="); put_u32(tasks[i].wcet);
        sh_puts(" T=");  put_u32(tasks[i].period);
        sh_puts(" U=");  put_frac3((uint32_t)(((uint64_t)tasks[i].wcet * 1000u) / tasks[i].period), 1000);
        sh_puts("\r\n");
    }

    /* Liu & Layland bound n(2^(1/n)-1), tabulated *1000. */
    static const uint32_t ll_bound_ppm[9] = {
        1000000, 1000000, 828427, 779763, 756828,
        743492,  734772,  728627, 724062
    };
    uint32_t bound = (n <= 8) ? ll_bound_ppm[n] : 693147; /* ln2 asymptote */
    sh_puts("  Total U="); put_frac3((uint32_t)(U_ppm / 1000u), 1000);
    sh_puts("  LL-bound="); put_frac3(bound / 1000u, 1000);
    sh_puts(U_ppm <= bound ? "  => PASS (U bound)\r\n"
                           : "  => inconclusive by U bound; using RTA\r\n");

    /* Exact response-time analysis (assumes tasks[] sorted by priority,
     * highest first; here priority = shorter period). */
    int all_ok = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t R = tasks[i].wcet;
        for (int iter = 0; iter < 100; iter++) {
            uint32_t interference = 0;
            for (uint32_t j = 0; j < i; j++) {   /* higher priority */
                uint32_t ceil_div = (R + tasks[j].period - 1u) / tasks[j].period;
                interference += ceil_div * tasks[j].wcet;
            }
            uint32_t R_new = tasks[i].wcet + interference;
            if (R_new == R) { break; }
            R = R_new;
            if (R > tasks[i].period * 4u) { break; } /* diverged */
        }
        int ok = (R <= tasks[i].period);
        if (!ok) { all_ok = 0; }
        sh_puts("  R("); sh_puts(tasks[i].name); sh_puts(")=");
        put_u32(R); sh_puts(tasks[i].period ? " <= T? " : " ");
        sh_puts(ok ? "yes\r\n" : "NO\r\n");
    }
    sh_puts(all_ok ? "  => Task set is SCHEDULABLE (RTA)\r\n"
                   : "  => Task set NOT schedulable (RTA)\r\n");
    sh_puts("\r\n");
    return all_ok;
}
