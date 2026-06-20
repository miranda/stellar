// common/stellar_hw.c
#include "stellar_hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <xf86drm.h>
#include <pciaccess.h>

#include "cJSON.h"

#define HARDWARE_CACHE_PATH "/var/cache/stellar/hardware.json"

// Slurp the whole cache file into a malloc'd NUL-terminated buffer.
// Returns NULL if the file is missing or unreadable; caller frees.
static char *read_cache_file(void) {
    FILE *f = fopen(HARDWARE_CACHE_PATH, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    if (length < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    char *data = malloc(length + 1);
    if (!data) { fclose(f); return NULL; }

    size_t got = fread(data, 1, length, f);
    fclose(f);
    data[got] = '\0';
    return data;
}

void probe_hardware(struct GpuInfo *gpus, int *gpu_count) {
    DIR *dir;
    struct dirent *ent;
    
    *gpu_count = 0;
    pci_system_init();

    dir = opendir("/dev/dri");
    if (!dir) return;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "card", 4) != 0) continue;
        if (*gpu_count >= MAX_GPUS) break; // Prevent buffer overflow

        char path[256];
        snprintf(path, sizeof(path), "/dev/dri/%s", ent->d_name);
        
        // Open as READ ONLY so a normal user doesn't get permission denied
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        drmVersionPtr version = drmGetVersion(fd);
        if (version) {
            snprintf(gpus[*gpu_count].driver, sizeof(gpus[*gpu_count].driver), "%s", version->name);
            drmFreeVersion(version);
        }

        drmDevicePtr device;
        if (drmGetDevice2(fd, 0, &device) == 0) {
            if (device->bustype == DRM_BUS_PCI) {
                // Keep existing bus ID formatting
                snprintf(gpus[*gpu_count].bus_id, sizeof(gpus[*gpu_count].bus_id), "PCI:%d:%d:%d",
                         device->businfo.pci->bus,
                         device->businfo.pci->dev,
                         device->businfo.pci->func);

                // Use libpciaccess to match the slot to the system's pci.ids database
                struct pci_device *pci_dev = pci_device_find_by_slot(
                    device->businfo.pci->domain,
                    device->businfo.pci->bus,
                    device->businfo.pci->dev,
                    device->businfo.pci->func
                );

                if (pci_dev) {
                    pci_device_probe(pci_dev); // Populate the extended strings
                    
                    // 1. Try the highly-specific subsystem name first
                    const char *sub_name = pci_device_get_subdevice_name(pci_dev);
                    
                    // 2. Fall back to the generic device name if the subsystem name is missing
                    const char *dev_name = pci_device_get_device_name(pci_dev);
                    
                    if (sub_name && strlen(sub_name) > 0) {
                        snprintf(gpus[*gpu_count].name, sizeof(gpus[*gpu_count].name), "%s", sub_name);
                    } else if (dev_name) {
                        snprintf(gpus[*gpu_count].name, sizeof(gpus[*gpu_count].name), "%s", dev_name);
                    } else {
                        snprintf(gpus[*gpu_count].name, sizeof(gpus[*gpu_count].name), "Unknown GPU");
                    }
                } else {
                    snprintf(gpus[*gpu_count].name, sizeof(gpus[*gpu_count].name), "Unknown GPU");
                }
            }
            drmFreeDevice(&device);
        }
        close(fd);
        (*gpu_count)++;
    }
    closedir(dir);
    pci_system_cleanup();
}

int check_hardware_mismatch(void) {
    struct GpuInfo live_gpus[16];
    int live_count = 0;
    
    // 1. Get current live hardware (No root required for this read-only step)
    probe_hardware(live_gpus, &live_count);

    // 2. Read the cache file
    char *data = read_cache_file();
    if (!data) {
        return 1; // File missing/unreadable, probe needed
    }

    cJSON *json = cJSON_Parse(data);
    free(data);
    if (!json) return 1; // Invalid JSON, probe needed

    cJSON *cards = cJSON_GetObjectItemCaseSensitive(json, "cards");
    if (!cards) {
        cJSON_Delete(json);
        return 1;
    }

    // 3. Compare counts
    int cached_count = cJSON_GetArraySize(cards);
    if (cached_count != live_count) {
        cJSON_Delete(json);
        return 1; // Number of GPUs changed
    }

    // 4. Compare drivers and Bus IDs
    for (int i = 0; i < live_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "%d", i);
        cJSON *card = cJSON_GetObjectItemCaseSensitive(cards, key);
        
        if (!card) {
            cJSON_Delete(json);
            return 1;
        }

        cJSON *driver = cJSON_GetObjectItemCaseSensitive(card, "driver");
        cJSON *bus_id = cJSON_GetObjectItemCaseSensitive(card, "bus_id");

        if (!cJSON_IsString(driver) || !cJSON_IsString(bus_id)) {
            cJSON_Delete(json);
            return 1;
        }

        if (strcmp(driver->valuestring, live_gpus[i].driver) != 0 ||
            strcmp(bus_id->valuestring, live_gpus[i].bus_id) != 0) {
            cJSON_Delete(json);
            return 1; // Hardware identity mismatch
        }
    }

    cJSON_Delete(json);
    return 0; // Everything matches perfectly
}

// Case-insensitive lowercase-hex compare. EDID hex from the probe is always
// lowercase, but compare defensively in case a live source emits uppercase.
static int hex_equal_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'F') ca += 32;
        if (cb >= 'A' && cb <= 'F') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

int check_monitor_mismatch(const struct LiveOutput *live, int live_count) {
    if (!live || live_count <= 0) {
        // Nothing live to compare (e.g. headless or no info supplied).
        // Not a mismatch on its own - the GPU check governs in that case.
        return 0;
    }

    char *data = read_cache_file();
    if (!data) return 1; // No cache => we have nothing to validate against; re-probe.

    cJSON *json = cJSON_Parse(data);
    free(data);
    if (!json) return 1;

    cJSON *cards = cJSON_GetObjectItemCaseSensitive(json, "cards");
    if (!cards) {
        cJSON_Delete(json);
        return 1;
    }

    int mismatch = 0;

    // For each live active output, locate the cache entry with the same output
    // name (searching across all cards) and compare EDID.
    for (int i = 0; i < live_count && !mismatch; i++) {
        const char *want_name = live[i].output_name;
        if (want_name[0] == '\0') continue;

        // A live output with no EDID we could read is inconclusive; skip it
        // rather than risk a false positive against a populated cache entry.
        if (live[i].edid_hex[0] == '\0') continue;

        const char *cached_edid = NULL;
        cJSON *card = NULL;

        cJSON_ArrayForEach(card, cards) {
            cJSON *outputs = cJSON_GetObjectItemCaseSensitive(card, "outputs");
            if (!cJSON_IsArray(outputs)) continue;

            cJSON *output = NULL;
            cJSON_ArrayForEach(output, outputs) {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(output, "name");
                if (!cJSON_IsString(name)) continue;
                if (strcmp(name->valuestring, want_name) != 0) continue;

                cJSON *edid = cJSON_GetObjectItemCaseSensitive(output, "edid");
                if (cJSON_IsString(edid)) cached_edid = edid->valuestring;
                break;
            }
            if (cached_edid) break;
        }

        if (!cached_edid) {
            // This output is live now but wasn't recorded (or had no EDID) in
            // the cache. Topology changed since the last probe -> re-probe.
            mismatch = 1;
            break;
        }

        // Cache recorded an empty EDID for this port but we can read one live:
        // also a meaningful change.
        if (cached_edid[0] == '\0') {
            mismatch = 1;
            break;
        }

        if (!hex_equal_ci(cached_edid, live[i].edid_hex)) {
            mismatch = 1; // Same port, different monitor -> swap detected.
            break;
        }
    }

    cJSON_Delete(json);
    return mismatch;
}
