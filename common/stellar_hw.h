// common/stellar_hw.h
#ifndef STELLAR_HW_H
#define STELLAR_HW_H

#define MAX_GPUS 16

// EDID is at most 512 bytes in our probe; hex-encoded that's 1024 chars + NUL.
#define MAX_EDID_HEX_LEN 1025

struct GpuInfo {
    char driver[64];
    char bus_id[64];
	char name[128];
};

// A single live, currently-active output as seen by the running (ZaphodHeads)
// X server. The caller (which holds the Display / IPC data) fills these in;
// the common layer never opens its own X connection.
struct LiveOutput {
    char output_name[64];            // DDX name, e.g. "DisplayPort-1"
    char edid_hex[MAX_EDID_HEX_LEN]; // lowercase hex; empty if no EDID was read
};

void probe_hardware(struct GpuInfo *gpus, int *gpu_count);

// GPU-identity check only (driver / bus_id / count). X-free; safe to call
// before X is up. Returns 1 if a re-probe is needed, 0 if GPUs match.
int check_hardware_mismatch(void);

// Per-output EDID check for monitor-swap detection. Compares the caller-supplied
// live active outputs against the cached per-output EDID in hardware.json,
// matching by output name. Cached outputs that are not present in `live`
// (i.e. inactive in ZaphodHeads mode, hence invisible to the live server) are
// NOT treated as mismatches - we simply can't see them, which is expected.
// Returns 1 if any live output's EDID differs from the cache (a swap), else 0.
int check_monitor_mismatch(const struct LiveOutput *live, int live_count);

#endif // STELLAR_HW_H
