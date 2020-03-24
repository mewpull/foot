#include "shm.h"

#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/mman.h>
#include <linux/memfd.h>
#include <fcntl.h>

#include <pixman.h>

#include <fcft/stride.h>
#include <tllist.h>

#define LOG_MODULE "shm"
#define LOG_ENABLE_DBG 0
#include "log.h"

#define TIME_SCROLL 0

/*
 * Maximum memfd size allowed.
 *
 * On 64-bit, we could in theory use up to 2GB (wk_shm_create_pool()
 * is limited to int32_t), since we never mmap() the entire region.
 *
 * The compositor is different matter - it needs to mmap() the entire
 * range, and *keep* the mapping for as long as is has buffers
 * referencing it (thus - always). And if we open multiple terminals,
 * then the required address space multiples...
 *
 * That said, 128TB (the total amount of available user address space
 * on 64-bit) is *a lot*; we can fit 67108864 2GB memfds into
 * that. But, let's be conservative for now.
 *
 * On 32-bit the available address space is too small and SHM
 * scrolling is disabled.
 */
static const off_t max_pool_size = 256 * 1024 * 1024;

static tll(struct buffer) buffers;

static bool can_punch_hole = false;
static bool can_punch_hole_initialized = false;

static void
buffer_destroy_dont_close(struct buffer *buf)
{
    if (buf->pix != NULL)
        pixman_image_unref(buf->pix);
    if (buf->wl_buf != NULL)
        wl_buffer_destroy(buf->wl_buf);
    if (buf->real_mmapped != MAP_FAILED)
        munmap(buf->real_mmapped, buf->mmap_size);

    buf->pix = NULL;
    buf->wl_buf = NULL;
    buf->real_mmapped = NULL;
    buf->mmapped = NULL;
}

static void
buffer_destroy(struct buffer *buf)
{
    buffer_destroy_dont_close(buf);
    if (buf->fd >= 0)
        close(buf->fd);
    buf->fd = -1;
}

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct buffer *buffer = data;
    LOG_DBG("release: cookie=%lx (buf=%p)", buffer->cookie, buffer);
    assert(buffer->wl_buf == wl_buffer);
    assert(buffer->busy);
    buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = &buffer_release,
};

static size_t
page_size(void)
{
    static size_t size = 0;
    if (size == 0) {
        size = sysconf(_SC_PAGE_SIZE);
        if (size < 0) {
            LOG_ERRNO("failed to get page size");
            size = 4096;
        }
    }
    assert(size > 0);
    return size;
}

static bool
instantiate_offset(struct wl_shm *shm, struct buffer *buf, off_t new_offset)
{
    assert(buf->fd >= 0);
    assert(buf->mmapped == NULL);
    assert(buf->real_mmapped == NULL);
    assert(buf->wl_buf == NULL);
    assert(buf->pix == NULL);
    assert(new_offset + buf->size <= max_pool_size);

    void *real_mmapped = MAP_FAILED;
    void *mmapped = MAP_FAILED;
    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *wl_buf = NULL;
    pixman_image_t *pix = NULL;

    /* mmap offset must be page aligned */
    off_t aligned_offset = new_offset & ~(page_size() - 1);
    size_t page_offset = new_offset & (page_size() - 1);
    size_t mmap_size = buf->size + page_offset;

    assert(aligned_offset <= new_offset);
    assert(mmap_size >= buf->size);

    LOG_DBG("size=%zx, offset=%zx, size-aligned=%zx, offset-aligned=%zx",
            buf->size, buf->offset, mmap_size, aligned_offset);

    real_mmapped = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_UNINITIALIZED, buf->fd, aligned_offset);
    if (real_mmapped == MAP_FAILED) {
        LOG_ERRNO("failed to mmap SHM backing memory file");
        goto err;
    }
    mmapped = real_mmapped + page_offset;

    pool = wl_shm_create_pool(shm, buf->fd, new_offset + buf->size);
    if (pool == NULL) {
        LOG_ERR("failed to create SHM pool");
        goto err;
    }

    wl_buf = wl_shm_pool_create_buffer(
        pool, new_offset, buf->width, buf->height, buf->stride, WL_SHM_FORMAT_ARGB8888);
    if (wl_buf == NULL) {
        LOG_ERR("failed to create SHM buffer");
        goto err;
    }

    /* We use the entire pool for our single buffer */
    wl_shm_pool_destroy(pool); pool = NULL;

    /* One pixman image for each worker thread (do we really need multiple?) */
    pix = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, buf->width, buf->height, (uint32_t *)mmapped, buf->stride);
    if (pix == NULL) {
        LOG_ERR("failed to create pixman image");
        goto err;
    }

    buf->offset = new_offset;
    buf->real_mmapped = real_mmapped;
    buf->mmapped = mmapped;
    buf->mmap_size = mmap_size;
    buf->wl_buf = wl_buf;
    buf->pix = pix;

    wl_buffer_add_listener(wl_buf, &buffer_listener, buf);
    return true;

err:
    if (pix != NULL)
        pixman_image_unref(pix);
    if (wl_buf != NULL)
        wl_buffer_destroy(wl_buf);
    if (pool != NULL)
        wl_shm_pool_destroy(pool);
    if (real_mmapped != MAP_FAILED)
        munmap(real_mmapped, mmap_size);

    abort();
    return false;
}

struct buffer *
shm_get_buffer(struct wl_shm *shm, int width, int height, unsigned long cookie)
{
    /* Purge buffers marked for purging */
    tll_foreach(buffers, it) {
        if (it->item.cookie != cookie)
            continue;

        if (!it->item.purge)
            continue;

        assert(!it->item.busy);

        LOG_DBG("cookie=%lx: purging buffer %p (width=%d, height=%d): %zu KB",
                cookie, &it->item, it->item.width, it->item.height,
                it->item.size / 1024);

        buffer_destroy(&it->item);
        tll_remove(buffers, it);
    }

    tll_foreach(buffers, it) {
        if (it->item.width != width)
            continue;
        if (it->item.height != height)
            continue;
        if (it->item.cookie != cookie)
            continue;

        if (!it->item.busy) {
            LOG_DBG("cookie=%lx: re-using buffer from cache (buf=%p)",
                    cookie, &it->item);
            it->item.busy = true;
            it->item.purge = false;
            return &it->item;
        }
    }

    /* Purge old buffers associated with this cookie */
    tll_foreach(buffers, it) {
        if (it->item.cookie != cookie)
            continue;

        if (it->item.busy)
            continue;

        if (it->item.width == width && it->item.height == height)
            continue;

        LOG_DBG("cookie=%lx: marking buffer %p for purging", cookie, &it->item);
        it->item.purge = true;
    }

    /*
     * No existing buffer available. Create a new one by:
     *
     * 1. open a memory backed "file" with memfd_create()
     * 2. mmap() the memory file, to be used by the pixman image
     * 3. create a wayland shm buffer for the same memory file
     *
     * The pixman image and the wayland buffer are now sharing memory.
     */

    int pool_fd = -1;
    const int stride = stride_for_format_and_width(PIXMAN_a8r8g8b8, width);
    const size_t size = stride * height;

    LOG_DBG("cookie=%lx: allocating new buffer: %zu KB", cookie, size / 1024);

    /* Backing memory for SHM */
    pool_fd = memfd_create("foot-wayland-shm-buffer-pool", MFD_CLOEXEC);
    if (pool_fd == -1) {
        LOG_ERRNO("failed to create SHM backing memory file");
        goto err;
    }

    /*
     * If we can figure our if we can punch holes *before* this, we
     * could set the initial offset to somewhere in the middle of the
     * avaiable address space. This would allow both backward and
     * forward scrolling without immediately needing to wrap.
     */
    //off_t initial_offset = (max_pool_size / 4) & ~(page_size() - 1);
    off_t initial_offset = 0;
    off_t memfd_size = initial_offset + size;

    if (ftruncate(pool_fd, memfd_size) == -1) {
        LOG_ERRNO("failed to truncate SHM pool");
        goto err;
    }

    if (!can_punch_hole_initialized) {
        can_punch_hole_initialized = true;
        can_punch_hole = fallocate(
            pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 1) == 0;

        if (!can_punch_hole) {
            LOG_WARN(
                "fallocate(FALLOC_FL_PUNCH_HOLE) not "
                "supported (%s): expect lower performance", strerror(errno));
        }
    }

    /* Push to list of available buffers, but marked as 'busy' */
    tll_push_back(
        buffers,
        ((struct buffer){
            .cookie = cookie,
            .width = width,
            .height = height,
            .stride = stride,
            .busy = true,
            .size = size,
            .fd = pool_fd,
            .mmap_size = size,
            .offset = 0}
            )
        );

    struct buffer *ret = &tll_back(buffers);
    if (!instantiate_offset(shm, ret, initial_offset))
        goto err;
    return ret;

err:
    if (pool_fd != -1)
        close(pool_fd);

    /* We don't handle this */
    abort();
    return NULL;
}

void
shm_fini(void)
{
    tll_foreach(buffers, it) {
        buffer_destroy(&it->item);
        tll_remove(buffers, it);
    }
}

bool
shm_can_scroll(void)
{
#if defined(__i386__)
    /* Not enough virtual address space in 32-bit */
    return false;
#else
    return can_punch_hole;
#endif
}

static bool
wrap_buffer(struct wl_shm *shm, struct buffer *buf, off_t new_offset)
{
    off_t aligned_offset = new_offset & ~(page_size() - 1);
    size_t page_offset = new_offset & (page_size() - 1);
    size_t mmap_size = buf->size + page_offset;

    assert(aligned_offset <= new_offset);
    assert(mmap_size >= buf->size);

    uint8_t *m = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_UNINITIALIZED, buf->fd, aligned_offset);
    if (m == MAP_FAILED) {
        LOG_ERRNO("failed to mmap");
        return false;
    }

    memcpy(m + page_offset, buf->mmapped, buf->size);
    munmap(m, mmap_size);

    /* Re-instantiate pixman+wl_buffer+raw pointersw */
    buffer_destroy_dont_close(buf);
    return instantiate_offset(shm, buf, new_offset);
}

static bool
shm_scroll_forward(struct wl_shm *shm, struct buffer *buf, int rows,
                   int top_margin, int top_keep_rows,
                   int bottom_margin, int bottom_keep_rows)
{
    assert(can_punch_hole);
    assert(buf->busy);
    assert(buf->pix);
    assert(buf->wl_buf);
    assert(buf->real_mmapped);
    assert(buf->fd >= 0);

    if (!can_punch_hole)
        return false;

    LOG_DBG("scrolling %d rows (%d bytes)", rows, rows * buf->stride);

    assert(rows > 0);
    assert(rows * buf->stride < buf->size);

    if (buf->offset + rows * buf->stride + buf->size > max_pool_size) {
        LOG_INFO("memfd offset wrap around");
        assert(buf->offset > buf->size);

        /*
         * Wrap around by moving the offset to the beginning of the
         * memfd. The ftruncate() we do below takes care of trimming
         * down the size.
         */
        if (!wrap_buffer(shm, buf, 0))
            goto err;
    }

    off_t new_offset = buf->offset + rows * buf->stride;
    assert(new_offset + buf->size <= max_pool_size);

#if TIME_SCROLL
    struct timeval time0;
    gettimeofday(&time0, NULL);
#endif

    /* Increase file size */
    if (ftruncate(buf->fd, new_offset + buf->size) < 0) {
        LOG_ERRNO("failed to resize memfd from %zu -> %zu",
                  buf->offset + buf->size, new_offset + buf->size);
        goto err;
    }

#if TIME_SCROLL
    struct timeval time1;
    gettimeofday(&time1, NULL);

    struct timeval tot;
    timersub(&time1, &time0, &tot);
    LOG_INFO("ftruncate: %lds %ldus", tot.tv_sec, tot.tv_usec);

    struct timeval time2 = time1;
#endif

    if (top_keep_rows > 0) {
        /* Copy current 'top' region to its new location */
        memmove(
            (uint8_t *)buf->mmapped + (top_margin + rows) * buf->stride,
            (uint8_t *)buf->mmapped + (top_margin + 0) * buf->stride,
            top_keep_rows * buf->stride);

#if TIME_SCROLL
        gettimeofday(&time2, NULL);
        timersub(&time2, &time1, &tot);
        LOG_INFO("memmove (top region): %lds %ldus", tot.tv_sec, tot.tv_usec);
#endif
    }

    /* Destroy old objects (they point to the old offset) */
    buffer_destroy_dont_close(buf);

    /* Free unused memory */
    if (new_offset > 0 &&
        fallocate(buf->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, new_offset) < 0)
    {
        LOG_ERRNO("fallocate(FALLOC_FL_PUNCH_HOLE, 0, %lu) failed", new_offset);
        goto err;
    }


#if TIME_SCROLL
    struct timeval time3;
    gettimeofday(&time3, NULL);
    timersub(&time3, &time2, &tot);
    LOG_INFO("PUNCH HOLE: %lds %ldus", tot.tv_sec, tot.tv_usec);
#endif

    /* Re-instantiate pixman+wl_buffer+raw pointersw */
    bool ret = instantiate_offset(shm, buf, new_offset);

    if (ret && bottom_keep_rows > 0) {
        /* Copy 'bottom' region to its new location */
        memmove(
            (uint8_t *)buf->mmapped + buf->size - (bottom_margin + bottom_keep_rows) * buf->stride,
            (uint8_t *)buf->mmapped + buf->size - (bottom_margin + rows + bottom_keep_rows) * buf->stride,
            bottom_keep_rows * buf->stride);

#if TIME_SCROLL
        struct timeval time4;
        gettimeofday(&time4, NULL);

        timersub(&time4, &time3, &tot);
        LOG_INFO("memmove (bottom region): %lds %ldus", tot.tv_sec, tot.tv_usec);
#endif
    }

    return ret;

err:
    abort();
    return false;
}

static bool
shm_scroll_reverse(struct wl_shm *shm, struct buffer *buf, int rows,
                   int top_margin, int top_keep_rows,
                   int bottom_margin, int bottom_keep_rows)
{
    assert(rows > 0);

    off_t diff = rows * buf->stride;
    if (diff > buf->offset) {
        LOG_INFO("memfd offset reverse wrap-around");

        /*
         * Wrap around by resizing the memfd and moving the offset to
         * the end of the file, taking care the new offset is aligned.
         */

        if (ftruncate(buf->fd, max_pool_size) < 0) {
            LOG_ERRNO("failed to resize memfd from %zu -> %zu",
                      buf->offset + buf->size, max_pool_size - buf->size);
            goto err;
        }

        if (!wrap_buffer(shm, buf, (max_pool_size - buf->size) & ~(page_size() - 1)))
            goto err;
    }

    off_t new_offset = buf->offset - diff;
    assert(new_offset <= max_pool_size);

#if TIME_SCROLL
    struct timeval time0;
    gettimeofday(&time0, NULL);

    struct timeval tot;
    struct timeval time1 = time0;
#endif

    if (bottom_keep_rows > 0) {
        /* Copy 'bottom' region to its new location */
        memmove(
            (uint8_t *)buf->mmapped + buf->size - (bottom_margin + rows + bottom_keep_rows) * buf->stride,
            (uint8_t *)buf->mmapped + buf->size - (bottom_margin + bottom_keep_rows) * buf->stride,
            bottom_keep_rows * buf->stride);

#if TIME_SCROLL
        gettimeofday(&time1, NULL);
        timersub(&time1, &time0, &tot);
        LOG_INFO("memmove (bottom region): %lds %ldus", tot.tv_sec, tot.tv_usec);
#endif
    }

    /* Destroy old objects (they point to the old offset) */
    buffer_destroy_dont_close(buf);

    if (ftruncate(buf->fd, new_offset + buf->size) < 0) {
        LOG_ERRNO("failed to resize memfd from %zu -> %zu",
                  buf->offset + buf->size, new_offset + buf->size);
        goto err;
    }

#if TIME_SCROLL
    struct timeval time2;
    gettimeofday(&time2, NULL);
    timersub(&time2, &time1, &tot);
    LOG_INFO("ftruncate: %lds %ldus", tot.tv_sec, tot.tv_usec);
#endif

    /* Re-instantiate pixman+wl_buffer+raw pointers */
    bool ret = instantiate_offset(shm, buf, new_offset);

    if (ret && top_keep_rows > 0) {
        /* Copy current 'top' region to its new location */
        memmove(
            (uint8_t *)buf->mmapped + (top_margin + 0) * buf->stride,
            (uint8_t *)buf->mmapped + (top_margin + rows) * buf->stride,
            top_keep_rows * buf->stride);

#if TIME_SCROLL
        struct timeval time3;
        gettimeofday(&time3, NULL);
        timersub(&time3, &time2, &tot);
        LOG_INFO("memmove (top region): %lds %ldus", tot.tv_sec, tot.tv_usec);
#endif
    }

    return ret;

err:
    abort();
    return false;
}

bool
shm_scroll(struct wl_shm *shm, struct buffer *buf, int rows,
           int top_margin, int top_keep_rows,
           int bottom_margin, int bottom_keep_rows)
{
    assert(rows != 0);
    return rows > 0
        ? shm_scroll_forward(shm, buf, rows, top_margin, top_keep_rows, bottom_margin, bottom_keep_rows)
        : shm_scroll_reverse(shm, buf, -rows, top_margin, top_keep_rows, bottom_margin, bottom_keep_rows);
}

    void
shm_purge(struct wl_shm *shm, unsigned long cookie)
{
    LOG_DBG("cookie=%lx: purging all buffers", cookie);

    /* Purge old buffers associated with this cookie */
    tll_foreach(buffers, it) {
        if (it->item.cookie != cookie)
            continue;

        assert(!it->item.busy);

        buffer_destroy(&it->item);
        tll_remove(buffers, it);
    }
}
