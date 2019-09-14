/*
 * Copyright 2019 Collabora, Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors (Collabora):
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */
#include <stdio.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <pthread.h>
#include "drm-uapi/panfrost_drm.h"

#include "pan_bo.h"
#include "pan_screen.h"
#include "pan_util.h"
#include "pandecode/decode.h"

#include "os/os_mman.h"

#include "util/u_inlines.h"
#include "util/u_math.h"

/* This file implements a userspace BO cache. Allocating and freeing
 * GPU-visible buffers is very expensive, and even the extra kernel roundtrips
 * adds more work than we would like at this point. So caching BOs in userspace
 * solves both of these problems and does not require kernel updates.
 *
 * Cached BOs are sorted into a bucket based on rounding their size down to the
 * nearest power-of-two. Each bucket contains a linked list of free panfrost_bo
 * objects. Putting a BO into the cache is accomplished by adding it to the
 * corresponding bucket. Getting a BO from the cache consists of finding the
 * appropriate bucket and sorting. A cache eviction is a kernel-level free of a
 * BO and removing it from the bucket. We special case evicting all BOs from
 * the cache, since that's what helpful in practice and avoids extra logic
 * around the linked list.
 */

static struct panfrost_bo *
panfrost_bo_alloc(struct panfrost_screen *screen, size_t size,
                  uint32_t flags)
{
        struct drm_panfrost_create_bo create_bo = { .size = size };
        struct panfrost_bo *bo;
        int ret;

        if (screen->kernel_version->version_major > 1 ||
            screen->kernel_version->version_minor >= 1) {
                if (flags & PAN_BO_GROWABLE)
                        create_bo.flags |= PANFROST_BO_HEAP;
                if (!(flags & PAN_BO_EXECUTE))
                        create_bo.flags |= PANFROST_BO_NOEXEC;
        }

        ret = drmIoctl(screen->fd, DRM_IOCTL_PANFROST_CREATE_BO, &create_bo);
        if (ret) {
                fprintf(stderr, "DRM_IOCTL_PANFROST_CREATE_BO failed: %m\n");
                return NULL;
        }

        bo = rzalloc(screen, struct panfrost_bo);
        assert(bo);
        bo->size = create_bo.size;
        bo->gpu = create_bo.offset;
        bo->gem_handle = create_bo.handle;
        bo->flags = flags;
        bo->screen = screen;
        return bo;
}

static void
panfrost_bo_free(struct panfrost_bo *bo)
{
        struct drm_gem_close gem_close = { .handle = bo->gem_handle };
        int ret;

        ret = drmIoctl(bo->screen->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
        if (ret) {
                fprintf(stderr, "DRM_IOCTL_GEM_CLOSE failed: %m\n");
                assert(0);
        }

        ralloc_free(bo);
}

/* Helper to calculate the bucket index of a BO */

static unsigned
pan_bucket_index(unsigned size)
{
        /* Round down to POT to compute a bucket index */

        unsigned bucket_index = util_logbase2(size);

        /* Clamp the bucket index; all huge allocations will be
         * sorted into the largest bucket */

        bucket_index = MIN2(bucket_index, MAX_BO_CACHE_BUCKET);

        /* The minimum bucket size must equal the minimum allocation
         * size; the maximum we clamped */

        assert(bucket_index >= MIN_BO_CACHE_BUCKET);
        assert(bucket_index <= MAX_BO_CACHE_BUCKET);

        /* Reindex from 0 */
        return (bucket_index - MIN_BO_CACHE_BUCKET);
}

static struct list_head *
pan_bucket(struct panfrost_screen *screen, unsigned size)
{
        return &screen->bo_cache[pan_bucket_index(size)];
}

/* Tries to fetch a BO of sufficient size with the appropriate flags from the
 * BO cache. If it succeeds, it returns that BO and removes the BO from the
 * cache. If it fails, it returns NULL signaling the caller to allocate a new
 * BO. */

static struct panfrost_bo *
panfrost_bo_cache_fetch(
                struct panfrost_screen *screen,
                size_t size, uint32_t flags)
{
        pthread_mutex_lock(&screen->bo_cache_lock);
        struct list_head *bucket = pan_bucket(screen, size);
        struct panfrost_bo *bo = NULL;

        /* Iterate the bucket looking for something suitable */
        list_for_each_entry_safe(struct panfrost_bo, entry, bucket, link) {
                if (entry->size >= size &&
                    entry->flags == flags) {
                        int ret;
                        struct drm_panfrost_madvise madv;

                        /* This one works, splice it out of the cache */
                        list_del(&entry->link);

                        madv.handle = entry->gem_handle;
                        madv.madv = PANFROST_MADV_WILLNEED;
                        madv.retained = 0;

                        ret = drmIoctl(screen->fd, DRM_IOCTL_PANFROST_MADVISE, &madv);
                        if (!ret && !madv.retained) {
                                panfrost_bo_free(entry);
                                continue;
                        }
                        /* Let's go! */
                        bo = entry;
                        break;
                }
        }
        pthread_mutex_unlock(&screen->bo_cache_lock);

        return bo;
}

/* Tries to add a BO to the cache. Returns if it was
 * successful */

static bool
panfrost_bo_cache_put(struct panfrost_bo *bo)
{
        struct panfrost_screen *screen = bo->screen;

        if (bo->flags & PAN_BO_DONT_REUSE)
                return false;

        pthread_mutex_lock(&screen->bo_cache_lock);
        struct list_head *bucket = pan_bucket(screen, bo->size);
        struct drm_panfrost_madvise madv;

        madv.handle = bo->gem_handle;
        madv.madv = PANFROST_MADV_DONTNEED;
	madv.retained = 0;

        drmIoctl(screen->fd, DRM_IOCTL_PANFROST_MADVISE, &madv);

        /* Add us to the bucket */
        list_addtail(&bo->link, bucket);
        pthread_mutex_unlock(&screen->bo_cache_lock);

        return true;
}

/* Evicts all BOs from the cache. Called during context
 * destroy or during low-memory situations (to free up
 * memory that may be unused by us just sitting in our
 * cache, but still reserved from the perspective of the
 * OS) */

void
panfrost_bo_cache_evict_all(
                struct panfrost_screen *screen)
{
        pthread_mutex_lock(&screen->bo_cache_lock);
        for (unsigned i = 0; i < ARRAY_SIZE(screen->bo_cache); ++i) {
                struct list_head *bucket = &screen->bo_cache[i];

                list_for_each_entry_safe(struct panfrost_bo, entry, bucket, link) {
                        list_del(&entry->link);
                        panfrost_bo_free(entry);
                }
        }
        pthread_mutex_unlock(&screen->bo_cache_lock);
}

void
panfrost_bo_mmap(struct panfrost_bo *bo)
{
        struct drm_panfrost_mmap_bo mmap_bo = { .handle = bo->gem_handle };
        int ret;

        if (bo->cpu)
                return;

        ret = drmIoctl(bo->screen->fd, DRM_IOCTL_PANFROST_MMAP_BO, &mmap_bo);
        if (ret) {
                fprintf(stderr, "DRM_IOCTL_PANFROST_MMAP_BO failed: %m\n");
                assert(0);
        }

        bo->cpu = os_mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                          bo->screen->fd, mmap_bo.offset);
        if (bo->cpu == MAP_FAILED) {
                fprintf(stderr, "mmap failed: %p %m\n", bo->cpu);
                assert(0);
        }

        /* Record the mmap if we're tracing */
        if (pan_debug & PAN_DBG_TRACE)
                pandecode_inject_mmap(bo->gpu, bo->cpu, bo->size, NULL);
}

static void
panfrost_bo_munmap(struct panfrost_bo *bo)
{
        if (!bo->cpu)
                return;

        if (os_munmap((void *) (uintptr_t)bo->cpu, bo->size)) {
                perror("munmap");
                abort();
        }

        bo->cpu = NULL;
}

struct panfrost_bo *
panfrost_bo_create(struct panfrost_screen *screen, size_t size,
                   uint32_t flags)
{
        struct panfrost_bo *bo;

        /* Kernel will fail (confusingly) with EPERM otherwise */
        assert(size > 0);

        /* To maximize BO cache usage, don't allocate tiny BOs */
        size = MAX2(size, 4096);

        /* GROWABLE BOs cannot be mmapped */
        if (flags & PAN_BO_GROWABLE)
                assert(flags & PAN_BO_INVISIBLE);

        /* Before creating a BO, we first want to check the cache, otherwise,
         * the cache misses and we need to allocate a BO fresh from the kernel
         */
        bo = panfrost_bo_cache_fetch(screen, size, flags);
        if (!bo)
                bo = panfrost_bo_alloc(screen, size, flags);

        if (!bo)
                fprintf(stderr, "BO creation failed\n");

        assert(bo);

        /* Only mmap now if we know we need to. For CPU-invisible buffers, we
         * never map since we don't care about their contents; they're purely
         * for GPU-internal use. But we do trace them anyway. */

        if (!(flags & (PAN_BO_INVISIBLE | PAN_BO_DELAY_MMAP)))
                panfrost_bo_mmap(bo);
        else if (flags & PAN_BO_INVISIBLE) {
                if (pan_debug & PAN_DBG_TRACE)
                        pandecode_inject_mmap(bo->gpu, NULL, bo->size, NULL);
        }

        pipe_reference_init(&bo->reference, 1);
        return bo;
}

void
panfrost_bo_reference(struct panfrost_bo *bo)
{
        if (bo)
                pipe_reference(NULL, &bo->reference);
}

void
panfrost_bo_unreference(struct panfrost_bo *bo)
{
        if (!bo)
                return;

        if (!pipe_reference(&bo->reference, NULL))
                return;

        /* When the reference count goes to zero, we need to cleanup */
        panfrost_bo_munmap(bo);

        /* Rather than freeing the BO now, we'll cache the BO for later
         * allocations if we're allowed to.
         */
        if (panfrost_bo_cache_put(bo))
                return;

        panfrost_bo_free(bo);
}

struct panfrost_bo *
panfrost_bo_import(struct panfrost_screen *screen, int fd)
{
        struct panfrost_bo *bo = rzalloc(screen, struct panfrost_bo);
        struct drm_panfrost_get_bo_offset get_bo_offset = {0,};
        ASSERTED int ret;
        unsigned gem_handle;

        ret = drmPrimeFDToHandle(screen->fd, fd, &gem_handle);
        assert(!ret);

        get_bo_offset.handle = gem_handle;
        ret = drmIoctl(screen->fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &get_bo_offset);
        assert(!ret);

        bo->screen = screen;
        bo->gem_handle = gem_handle;
        bo->gpu = (mali_ptr) get_bo_offset.offset;
        bo->size = lseek(fd, 0, SEEK_END);
        bo->flags |= PAN_BO_DONT_REUSE;
        assert(bo->size > 0);
        pipe_reference_init(&bo->reference, 1);

        // TODO map and unmap on demand?
        panfrost_bo_mmap(bo);
        return bo;
}

int
panfrost_bo_export(struct panfrost_bo *bo)
{
        struct drm_prime_handle args = {
                .handle = bo->gem_handle,
                .flags = DRM_CLOEXEC,
        };

        int ret = drmIoctl(bo->screen->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
        if (ret == -1)
                return -1;

        bo->flags |= PAN_BO_DONT_REUSE;
        return args.fd;
}

