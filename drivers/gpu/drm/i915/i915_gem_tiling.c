/*
 * Copyright © 2008 Intel Corporation
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
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <linux/string.h>
#include <linux/bitops.h>
#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"

/**
 * DOC: buffer object tiling
 *
 * i915_gem_set_tiling_ioctl() and i915_gem_get_tiling_ioctl() is the userspace
 * interface to declare fence register requirements.
 *
 * In principle GEM doesn't care at all about the internal data layout of an
 * object, and hence it also doesn't care about tiling or swizzling. There's two
 * exceptions:
 *
 * - For X and Y tiling the hardware provides detilers for CPU access, so called
 *   fences. Since there's only a limited amount of them the kernel must manage
 *   these, and therefore userspace must tell the kernel the object tiling if it
 *   wants to use fences for detiling.
 * - On gen3 and gen4 platforms have a swizzling pattern for tiled objects which
 *   depends upon the physical page frame number. When swapping such objects the
 *   page frame number might change and the kernel must be able to fix this up
 *   and hence now the tiling. Note that on a subset of platforms with
 *   asymmetric memory channel population the swizzling pattern changes in an
 *   unknown way, and for those the kernel simply forbids swapping completely.
 *
 * Since neither of this applies for new tiling layouts on modern platforms like
 * W, Ys and Yf tiling GEM only allows object tiling to be set to X or Y tiled.
 * Anything else can be handled in userspace entirely without the kernel's
 * invovlement.
 */

/**
 * i915_gem_fence_size - required global GTT size for a fence
 * @i915: i915 device
 * @size: object size
 * @tiling: tiling mode
 * @stride: tiling stride
 *
 * Return the required global GTT size for a fence (view of a tiled object),
 * taking into account potential fence register mapping.
 */
u32 i915_gem_fence_size(struct drm_i915_private *i915,
			u32 size, unsigned int tiling, unsigned int stride)
{
	u32 ggtt_size;

	GEM_BUG_ON(!size);

	if (tiling == I915_TILING_NONE)
		return size;

	GEM_BUG_ON(!stride);

	if (INTEL_GEN(i915) >= 4) {
		stride *= i915_gem_tile_height(tiling);
		GEM_BUG_ON(stride & 4095);
		return roundup(size, stride);
	}

	/* Previous chips need a power-of-two fence region when tiling */
	if (IS_GEN3(i915))
		ggtt_size = 1024*1024;
	else
		ggtt_size = 512*1024;

	while (ggtt_size < size)
		ggtt_size <<= 1;

	return ggtt_size;
}

/**
 * i915_gem_fence_alignment - required global GTT alignment for a fence
 * @i915: i915 device
 * @size: object size
 * @tiling: tiling mode
 * @stride: tiling stride
 *
 * Return the required global GTT alignment for a fence (a view of a tiled
 * object), taking into account potential fence register mapping.
 */
u32 i915_gem_fence_alignment(struct drm_i915_private *i915, u32 size,
			     unsigned int tiling, unsigned int stride)
{
	GEM_BUG_ON(!size);

	/*
	 * Minimum alignment is 4k (GTT page size), but might be greater
	 * if a fence register is needed for the object.
	 */
	if (INTEL_GEN(i915) >= 4 || tiling == I915_TILING_NONE)
		return 4096;

	/*
	 * Previous chips need to be aligned to the size of the smallest
	 * fence register that can contain the object.
	 */
	return i915_gem_fence_size(i915, size, tiling, stride);
}

/* Check pitch constriants for all chips & tiling formats */
static bool
i915_tiling_ok(struct drm_i915_private *dev_priv,
	       int stride, int size, int tiling_mode)
{
	int tile_width;

	/* Linear is always fine */
	if (tiling_mode == I915_TILING_NONE)
		return true;

	if (tiling_mode > I915_TILING_LAST)
		return false;

	if (IS_GEN2(dev_priv) ||
	    (tiling_mode == I915_TILING_Y && HAS_128_BYTE_Y_TILING(dev_priv)))
		tile_width = 128;
	else
		tile_width = 512;

	/* check maximum stride & object size */
	/* i965+ stores the end address of the gtt mapping in the fence
	 * reg, so dont bother to check the size */
	if (INTEL_GEN(dev_priv) >= 7) {
		if (stride / 128 > GEN7_FENCE_MAX_PITCH_VAL)
			return false;
	} else if (INTEL_GEN(dev_priv) >= 4) {
		if (stride / 128 > I965_FENCE_MAX_PITCH_VAL)
			return false;
	} else {
		if (stride > 8192)
			return false;

		if (IS_GEN3(dev_priv)) {
			if (size > I830_FENCE_MAX_SIZE_VAL << 20)
				return false;
		} else {
			if (size > I830_FENCE_MAX_SIZE_VAL << 19)
				return false;
		}
	}

	if (stride < tile_width)
		return false;

	/* 965+ just needs multiples of tile width */
	if (INTEL_GEN(dev_priv) >= 4) {
		if (stride & (tile_width - 1))
			return false;
		return true;
	}

	/* Pre-965 needs power of two tile widths */
	if (stride & (stride - 1))
		return false;

	return true;
}

static bool i915_vma_fence_prepare(struct i915_vma *vma,
				   int tiling_mode, unsigned int stride)
{
	struct drm_i915_private *i915 = vma->vm->i915;
	u32 size, alignment;

	if (!i915_vma_is_map_and_fenceable(vma))
		return true;

	size = i915_gem_fence_size(i915, vma->size, tiling_mode, stride);
	if (vma->node.size < size)
		return false;

	alignment = i915_gem_fence_alignment(i915, vma->size, tiling_mode, stride);
	if (vma->node.start & (alignment - 1))
		return false;

	return true;
}

/* Make the current GTT allocation valid for the change in tiling. */
static int
i915_gem_object_fence_prepare(struct drm_i915_gem_object *obj,
			      int tiling_mode, unsigned int stride)
{
	struct i915_vma *vma;
	int ret;

	if (tiling_mode == I915_TILING_NONE)
		return 0;

	list_for_each_entry(vma, &obj->vma_list, obj_link) {
		if (!i915_vma_is_ggtt(vma))
			break;

		if (i915_vma_fence_prepare(vma, tiling_mode, stride))
			continue;

		ret = i915_vma_unbind(vma);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * i915_gem_set_tiling_ioctl - IOCTL handler to set tiling mode
 * @dev: DRM device
 * @data: data pointer for the ioctl
 * @file: DRM file for the ioctl call
 *
 * Sets the tiling mode of an object, returning the required swizzling of
 * bit 6 of addresses in the object.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int
i915_gem_set_tiling_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	struct drm_i915_gem_set_tiling *args = data;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_object *obj;
	int err = 0;

	/* Make sure we don't cross-contaminate obj->tiling_and_stride */
	BUILD_BUG_ON(I915_TILING_LAST & STRIDE_MASK);

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	if (!i915_tiling_ok(dev_priv,
			    args->stride, obj->base.size, args->tiling_mode)) {
		i915_gem_object_put(obj);
		return -EINVAL;
	}

	mutex_lock(&dev->struct_mutex);
	if (obj->pin_display || obj->framebuffer_references) {
		err = -EBUSY;
		goto err;
	}

	if (args->tiling_mode == I915_TILING_NONE) {
		args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		args->stride = 0;
	} else {
		if (args->tiling_mode == I915_TILING_X)
			args->swizzle_mode = dev_priv->mm.bit_6_swizzle_x;
		else
			args->swizzle_mode = dev_priv->mm.bit_6_swizzle_y;

		/* Hide bit 17 swizzling from the user.  This prevents old Mesa
		 * from aborting the application on sw fallbacks to bit 17,
		 * and we use the pread/pwrite bit17 paths to swizzle for it.
		 * If there was a user that was relying on the swizzle
		 * information for drm_intel_bo_map()ed reads/writes this would
		 * break it, but we don't have any of those.
		 */
		if (args->swizzle_mode == I915_BIT_6_SWIZZLE_9_17)
			args->swizzle_mode = I915_BIT_6_SWIZZLE_9;
		if (args->swizzle_mode == I915_BIT_6_SWIZZLE_9_10_17)
			args->swizzle_mode = I915_BIT_6_SWIZZLE_9_10;

		/* If we can't handle the swizzling, make it untiled. */
		if (args->swizzle_mode == I915_BIT_6_SWIZZLE_UNKNOWN) {
			args->tiling_mode = I915_TILING_NONE;
			args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
			args->stride = 0;
		}
	}

	if (args->tiling_mode != i915_gem_object_get_tiling(obj) ||
	    args->stride != i915_gem_object_get_stride(obj)) {
		/* We need to rebind the object if its current allocation
		 * no longer meets the alignment restrictions for its new
		 * tiling mode. Otherwise we can just leave it alone, but
		 * need to ensure that any fence register is updated before
		 * the next fenced (either through the GTT or by the BLT unit
		 * on older GPUs) access.
		 *
		 * After updating the tiling parameters, we then flag whether
		 * we need to update an associated fence register. Note this
		 * has to also include the unfenced register the GPU uses
		 * whilst executing a fenced command for an untiled object.
		 */

		err = i915_gem_object_fence_prepare(obj,
						    args->tiling_mode,
						    args->stride);
		if (!err) {
			struct i915_vma *vma;

			mutex_lock(&obj->mm.lock);
			if (obj->mm.pages &&
			    obj->mm.madv == I915_MADV_WILLNEED &&
			    dev_priv->quirks & QUIRK_PIN_SWIZZLED_PAGES) {
				if (args->tiling_mode == I915_TILING_NONE) {
					GEM_BUG_ON(!obj->mm.quirked);
					__i915_gem_object_unpin_pages(obj);
					obj->mm.quirked = false;
				}
				if (!i915_gem_object_is_tiled(obj)) {
					GEM_BUG_ON(!obj->mm.quirked);
					__i915_gem_object_pin_pages(obj);
					obj->mm.quirked = true;
				}
			}
			mutex_unlock(&obj->mm.lock);

			list_for_each_entry(vma, &obj->vma_list, obj_link) {
				if (!i915_vma_is_ggtt(vma))
					break;

				vma->fence_size = i915_gem_fence_size(dev_priv, vma->size,
								      args->tiling_mode,
								      args->stride);
				vma->fence_alignment = i915_gem_fence_alignment(dev_priv, vma->size,
										args->tiling_mode,
										args->stride);

				if (vma->fence)
					vma->fence->dirty = true;
			}
			obj->tiling_and_stride =
				args->stride | args->tiling_mode;

			/* Force the fence to be reacquired for GTT access */
			i915_gem_release_mmap(obj);
		}
	}
	/* we have to maintain this existing ABI... */
	args->stride = i915_gem_object_get_stride(obj);
	args->tiling_mode = i915_gem_object_get_tiling(obj);

	/* Try to preallocate memory required to save swizzling on put-pages */
	if (i915_gem_object_needs_bit17_swizzle(obj)) {
		if (obj->bit_17 == NULL) {
			obj->bit_17 = kcalloc(BITS_TO_LONGS(obj->base.size >> PAGE_SHIFT),
					      sizeof(long), GFP_KERNEL);
		}
	} else {
		kfree(obj->bit_17);
		obj->bit_17 = NULL;
	}

err:
	i915_gem_object_put(obj);
	mutex_unlock(&dev->struct_mutex);

	return err;
}

/**
 * i915_gem_get_tiling_ioctl - IOCTL handler to get tiling mode
 * @dev: DRM device
 * @data: data pointer for the ioctl
 * @file: DRM file for the ioctl call
 *
 * Returns the current tiling mode and required bit 6 swizzling for the object.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int
i915_gem_get_tiling_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	struct drm_i915_gem_get_tiling *args = data;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_object *obj;
	int err = -ENOENT;

	rcu_read_lock();
	obj = i915_gem_object_lookup_rcu(file, args->handle);
	if (obj) {
		args->tiling_mode =
			READ_ONCE(obj->tiling_and_stride) & TILING_MASK;
		err = 0;
	}
	rcu_read_unlock();
	if (unlikely(err))
		return err;

	switch (args->tiling_mode) {
	case I915_TILING_X:
		args->swizzle_mode = dev_priv->mm.bit_6_swizzle_x;
		break;
	case I915_TILING_Y:
		args->swizzle_mode = dev_priv->mm.bit_6_swizzle_y;
		break;
	default:
	case I915_TILING_NONE:
		args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		break;
	}

	/* Hide bit 17 from the user -- see comment in i915_gem_set_tiling */
	if (dev_priv->quirks & QUIRK_PIN_SWIZZLED_PAGES)
		args->phys_swizzle_mode = I915_BIT_6_SWIZZLE_UNKNOWN;
	else
		args->phys_swizzle_mode = args->swizzle_mode;
	if (args->swizzle_mode == I915_BIT_6_SWIZZLE_9_17)
		args->swizzle_mode = I915_BIT_6_SWIZZLE_9;
	if (args->swizzle_mode == I915_BIT_6_SWIZZLE_9_10_17)
		args->swizzle_mode = I915_BIT_6_SWIZZLE_9_10;

	return 0;
}
