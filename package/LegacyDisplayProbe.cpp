// SPDX-License-Identifier: BSD-2-Clause-Patent
// Minimal dormant hooks for the offline component-load probes of the DEVELOPMENT kernel
// (patches/xnu/offline-component-load-probes.patch): bsd_init references
// native_legacy_display_probe() unconditionally, but the probe only runs with
// offline-legacy-register=1, which this board never passes. This stub keeps the link
// closed: it maps and starts nothing and only counts its calls for the diagnostics.

extern "C" volatile unsigned int native_legacy_display_probe_calls;
volatile unsigned int native_legacy_display_probe_calls;

extern "C" void
native_legacy_display_probe (void)
{
	native_legacy_display_probe_calls = native_legacy_display_probe_calls + 1;
}
