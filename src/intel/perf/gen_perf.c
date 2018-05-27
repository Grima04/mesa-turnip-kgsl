/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <drm-uapi/i915_drm.h>

#include "gen_perf.h"
#include "perf/gen_perf_metrics.h"

#include "dev/gen_debug.h"
#include "dev/gen_device_info.h"
#include "util/bitscan.h"

#define FILE_DEBUG_FLAG DEBUG_PERFMON

static bool
get_sysfs_dev_dir(struct gen_perf *perf, int fd)
{
   struct stat sb;
   int min, maj;
   DIR *drmdir;
   struct dirent *drm_entry;
   int len;

   perf->sysfs_dev_dir[0] = '\0';

   if (fstat(fd, &sb)) {
      DBG("Failed to stat DRM fd\n");
      return false;
   }

   maj = major(sb.st_rdev);
   min = minor(sb.st_rdev);

   if (!S_ISCHR(sb.st_mode)) {
      DBG("DRM fd is not a character device as expected\n");
      return false;
   }

   len = snprintf(perf->sysfs_dev_dir,
                  sizeof(perf->sysfs_dev_dir),
                  "/sys/dev/char/%d:%d/device/drm", maj, min);
   if (len < 0 || len >= sizeof(perf->sysfs_dev_dir)) {
      DBG("Failed to concatenate sysfs path to drm device\n");
      return false;
   }

   drmdir = opendir(perf->sysfs_dev_dir);
   if (!drmdir) {
      DBG("Failed to open %s: %m\n", perf->sysfs_dev_dir);
      return false;
   }

   while ((drm_entry = readdir(drmdir))) {
      if ((drm_entry->d_type == DT_DIR ||
           drm_entry->d_type == DT_LNK) &&
          strncmp(drm_entry->d_name, "card", 4) == 0)
      {
         len = snprintf(perf->sysfs_dev_dir,
                        sizeof(perf->sysfs_dev_dir),
                        "/sys/dev/char/%d:%d/device/drm/%s",
                        maj, min, drm_entry->d_name);
         closedir(drmdir);
         if (len < 0 || len >= sizeof(perf->sysfs_dev_dir))
            return false;
         else
            return true;
      }
   }

   closedir(drmdir);

   DBG("Failed to find cardX directory under /sys/dev/char/%d:%d/device/drm\n",
       maj, min);

   return false;
}

static bool
read_file_uint64(const char *file, uint64_t *val)
{
    char buf[32];
    int fd, n;

    fd = open(file, 0);
    if (fd < 0)
       return false;
    while ((n = read(fd, buf, sizeof (buf) - 1)) < 0 &&
           errno == EINTR);
    close(fd);
    if (n < 0)
       return false;

    buf[n] = '\0';
    *val = strtoull(buf, NULL, 0);

    return true;
}

static bool
read_sysfs_drm_device_file_uint64(struct gen_perf *perf,
                                  const char *file,
                                  uint64_t *value)
{
   char buf[512];
   int len;

   len = snprintf(buf, sizeof(buf), "%s/%s", perf->sysfs_dev_dir, file);
   if (len < 0 || len >= sizeof(buf)) {
      DBG("Failed to concatenate sys filename to read u64 from\n");
      return false;
   }

   return read_file_uint64(buf, value);
}

static void
register_oa_config(struct gen_perf *perf,
                   const struct gen_perf_query_info *query,
                   uint64_t config_id)
{
   struct gen_perf_query_info *registred_query =
      gen_perf_query_append_query_info(perf, 0);

   *registred_query = *query;
   registred_query->oa_metrics_set_id = config_id;
   DBG("metric set registred: id = %" PRIu64", guid = %s\n",
       registred_query->oa_metrics_set_id, query->guid);
}

static void
enumerate_sysfs_metrics(struct gen_perf *perf)
{
   DIR *metricsdir = NULL;
   struct dirent *metric_entry;
   char buf[256];
   int len;

   len = snprintf(buf, sizeof(buf), "%s/metrics", perf->sysfs_dev_dir);
   if (len < 0 || len >= sizeof(buf)) {
      DBG("Failed to concatenate path to sysfs metrics/ directory\n");
      return;
   }

   metricsdir = opendir(buf);
   if (!metricsdir) {
      DBG("Failed to open %s: %m\n", buf);
      return;
   }

   while ((metric_entry = readdir(metricsdir))) {
      struct hash_entry *entry;

      if ((metric_entry->d_type != DT_DIR &&
           metric_entry->d_type != DT_LNK) ||
          metric_entry->d_name[0] == '.')
         continue;

      DBG("metric set: %s\n", metric_entry->d_name);
      entry = _mesa_hash_table_search(perf->oa_metrics_table,
                                      metric_entry->d_name);
      if (entry) {
         uint64_t id;

         len = snprintf(buf, sizeof(buf), "%s/metrics/%s/id",
                        perf->sysfs_dev_dir, metric_entry->d_name);
         if (len < 0 || len >= sizeof(buf)) {
            DBG("Failed to concatenate path to sysfs metric id file\n");
            continue;
         }

         if (!read_file_uint64(buf, &id)) {
            DBG("Failed to read metric set id from %s: %m", buf);
            continue;
         }

         register_oa_config(perf, (const struct gen_perf_query_info *)entry->data, id);
      } else
         DBG("metric set not known by mesa (skipping)\n");
   }

   closedir(metricsdir);
}

static bool
kernel_has_dynamic_config_support(struct gen_perf *perf, int fd)
{
   hash_table_foreach(perf->oa_metrics_table, entry) {
      struct gen_perf_query_info *query = entry->data;
      char config_path[280];
      uint64_t config_id;

      snprintf(config_path, sizeof(config_path), "%s/metrics/%s/id",
               perf->sysfs_dev_dir, query->guid);

      /* Look for the test config, which we know we can't replace. */
      if (read_file_uint64(config_path, &config_id) && config_id == 1) {
         return perf->ioctl(fd, DRM_IOCTL_I915_PERF_REMOVE_CONFIG,
                            &config_id) < 0 && errno == ENOENT;
      }
   }

   return false;
}

bool
gen_perf_load_metric_id(struct gen_perf *perf, const char *guid,
                        uint64_t *metric_id)
{
   char config_path[280];

   snprintf(config_path, sizeof(config_path), "%s/metrics/%s/id",
            perf->sysfs_dev_dir, guid);

   /* Don't recreate already loaded configs. */
   return read_file_uint64(config_path, metric_id);
}

static void
init_oa_configs(struct gen_perf *perf, int fd)
{
   hash_table_foreach(perf->oa_metrics_table, entry) {
      const struct gen_perf_query_info *query = entry->data;
      struct drm_i915_perf_oa_config config;
      uint64_t config_id;
      int ret;

      if (gen_perf_load_metric_id(perf, query->guid, &config_id)) {
         DBG("metric set: %s (already loaded)\n", query->guid);
         register_oa_config(perf, query, config_id);
         continue;
      }

      memset(&config, 0, sizeof(config));

      memcpy(config.uuid, query->guid, sizeof(config.uuid));

      config.n_mux_regs = query->n_mux_regs;
      config.mux_regs_ptr = (uintptr_t) query->mux_regs;

      config.n_boolean_regs = query->n_b_counter_regs;
      config.boolean_regs_ptr = (uintptr_t) query->b_counter_regs;

      config.n_flex_regs = query->n_flex_regs;
      config.flex_regs_ptr = (uintptr_t) query->flex_regs;

      ret = perf->ioctl(fd, DRM_IOCTL_I915_PERF_ADD_CONFIG, &config);
      if (ret < 0) {
         DBG("Failed to load \"%s\" (%s) metrics set in kernel: %s\n",
             query->name, query->guid, strerror(errno));
         continue;
      }

      register_oa_config(perf, query, ret);
      DBG("metric set: %s (added)\n", query->guid);
   }
}

static void
compute_topology_builtins(struct gen_perf *perf,
                          const struct gen_device_info *devinfo)
{
   perf->sys_vars.slice_mask = devinfo->slice_masks;
   perf->sys_vars.n_eu_slices = devinfo->num_slices;

   for (int i = 0; i < sizeof(devinfo->subslice_masks[i]); i++) {
      perf->sys_vars.n_eu_sub_slices +=
         __builtin_popcount(devinfo->subslice_masks[i]);
   }

   for (int i = 0; i < sizeof(devinfo->eu_masks); i++)
      perf->sys_vars.n_eus += __builtin_popcount(devinfo->eu_masks[i]);

   perf->sys_vars.eu_threads_count =
      perf->sys_vars.n_eus * devinfo->num_thread_per_eu;

   /* The subslice mask builtin contains bits for all slices. Prior to Gen11
    * it had groups of 3bits for each slice, on Gen11 it's 8bits for each
    * slice.
    *
    * Ideally equations would be updated to have a slice/subslice query
    * function/operator.
    */
   perf->sys_vars.subslice_mask = 0;

   int bits_per_subslice = devinfo->gen == 11 ? 8 : 3;

   for (int s = 0; s < util_last_bit(devinfo->slice_masks); s++) {
      for (int ss = 0; ss < (devinfo->subslice_slice_stride * 8); ss++) {
         if (gen_device_info_subslice_available(devinfo, s, ss))
            perf->sys_vars.subslice_mask |= 1ULL << (s * bits_per_subslice + ss);
      }
   }
}

static bool
init_oa_sys_vars(struct gen_perf *perf, const struct gen_device_info *devinfo)
{
   uint64_t min_freq_mhz = 0, max_freq_mhz = 0;

   if (!read_sysfs_drm_device_file_uint64(perf, "gt_min_freq_mhz", &min_freq_mhz))
      return false;

   if (!read_sysfs_drm_device_file_uint64(perf,  "gt_max_freq_mhz", &max_freq_mhz))
      return false;

   memset(&perf->sys_vars, 0, sizeof(perf->sys_vars));
   perf->sys_vars.gt_min_freq = min_freq_mhz * 1000000;
   perf->sys_vars.gt_max_freq = max_freq_mhz * 1000000;
   perf->sys_vars.timestamp_frequency = devinfo->timestamp_frequency;
   perf->sys_vars.revision = devinfo->revision;
   compute_topology_builtins(perf, devinfo);

   return true;
}

typedef void (*perf_register_oa_queries_t)(struct gen_perf *);

static perf_register_oa_queries_t
get_register_queries_function(const struct gen_device_info *devinfo)
{
   if (devinfo->is_haswell)
      return gen_oa_register_queries_hsw;
   if (devinfo->is_cherryview)
      return gen_oa_register_queries_chv;
   if (devinfo->is_broadwell)
      return gen_oa_register_queries_bdw;
   if (devinfo->is_broxton)
      return gen_oa_register_queries_bxt;
   if (devinfo->is_skylake) {
      if (devinfo->gt == 2)
         return gen_oa_register_queries_sklgt2;
      if (devinfo->gt == 3)
         return gen_oa_register_queries_sklgt3;
      if (devinfo->gt == 4)
         return gen_oa_register_queries_sklgt4;
   }
   if (devinfo->is_kabylake) {
      if (devinfo->gt == 2)
         return gen_oa_register_queries_kblgt2;
      if (devinfo->gt == 3)
         return gen_oa_register_queries_kblgt3;
   }
   if (devinfo->is_geminilake)
      return gen_oa_register_queries_glk;
   if (devinfo->is_coffeelake) {
      if (devinfo->gt == 2)
         return gen_oa_register_queries_cflgt2;
      if (devinfo->gt == 3)
         return gen_oa_register_queries_cflgt3;
   }
   if (devinfo->is_cannonlake)
      return gen_oa_register_queries_cnl;

   return NULL;
}

bool
gen_perf_load_oa_metrics(struct gen_perf *perf, int fd,
                         const struct gen_device_info *devinfo)
{
   perf_register_oa_queries_t oa_register = get_register_queries_function(devinfo);
   bool i915_perf_oa_available = false;
   struct stat sb;

   /* The existence of this sysctl parameter implies the kernel supports
    * the i915 perf interface.
    */
   if (stat("/proc/sys/dev/i915/perf_stream_paranoid", &sb) == 0) {

      /* If _paranoid == 1 then on Gen8+ we won't be able to access OA
       * metrics unless running as root.
       */
      if (devinfo->is_haswell)
         i915_perf_oa_available = true;
      else {
         uint64_t paranoid = 1;

         read_file_uint64("/proc/sys/dev/i915/perf_stream_paranoid", &paranoid);

         if (paranoid == 0 || geteuid() == 0)
            i915_perf_oa_available = true;
      }
   }

   if (!i915_perf_oa_available ||
       !oa_register ||
       !get_sysfs_dev_dir(perf, fd) ||
       !init_oa_sys_vars(perf, devinfo))
      return false;

   perf->oa_metrics_table =
      _mesa_hash_table_create(perf, _mesa_key_hash_string,
                              _mesa_key_string_equal);

   /* Index all the metric sets mesa knows about before looking to see what
    * the kernel is advertising.
    */
   oa_register(perf);

   if (likely((INTEL_DEBUG & DEBUG_NO_OACONFIG) == 0) &&
       kernel_has_dynamic_config_support(perf, fd))
      init_oa_configs(perf, fd);
   else
      enumerate_sysfs_metrics(perf);

   return true;
}
