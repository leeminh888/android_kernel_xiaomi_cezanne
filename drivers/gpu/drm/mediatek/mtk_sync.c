/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/seq_file.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <linux/sync_file.h>

#include "mtk_sync.h"

/* ---------------------------------------------------------------- */

static struct dma_fence_ops mtk_sync_timeline_fence_ops;

static inline struct sync_pt *fence_to_sync_pt(struct dma_fence *fence)
{
	if (fence->ops != &mtk_sync_timeline_fence_ops)
		return NULL;
	return container_of(fence, struct sync_pt, base);
}

static void mtk_sync_timeline_free(struct kref *kref)
{
	struct sync_timeline *obj =
		container_of(kref, struct sync_timeline, kref);

	sync_timeline_debug_remove(obj);

	kfree(obj);
}

static void mtk_sync_timeline_get(struct sync_timeline *obj)
{
	kref_get(&obj->kref);
}

static void mtk_sync_timeline_put(struct sync_timeline *obj)
{
	kref_put(&obj->kref, mtk_sync_timeline_free);
}

/**
 * mtk_sync_pt_create() - creates a sync pt
 * @parent:	fence's parent sync_timeline
 * @size:	size to allocate for this pt
 * @inc:	value of the fence
 *
 * Creates a new sync_pt as a child of @parent.  @size bytes will be
 * allocated allowing for implementation specific data to be kept after
 * the generic sync_timeline struct. Returns the sync_pt object or
 * NULL in case of error.
 */
static struct sync_pt *mtk_sync_pt_create(struct sync_timeline *obj,
					  unsigned int value)
{
	struct sync_pt *pt;

	pt = kzalloc(sizeof(*pt), GFP_KERNEL);
	if (!pt)
		return NULL;

	mtk_sync_timeline_get(obj);
	dma_fence_init(&pt->base, &mtk_sync_timeline_fence_ops, &obj->lock,
		   obj->context, value);
	INIT_LIST_HEAD(&pt->link);

	spin_lock_irq(&obj->lock);
	if (!dma_fence_is_signaled_locked(&pt->base)) {
		struct rb_node **p = &obj->pt_tree.rb_node;
		struct rb_node *parent = NULL;

		while (*p) {
			struct sync_pt *other;
			int cmp;

			parent = *p;
			other = rb_entry(parent, typeof(*pt), node);
			cmp = value - other->base.seqno;
			if (cmp > 0) {
				p = &parent->rb_right;
			} else if (cmp < 0) {
				p = &parent->rb_left;
			} else {
				if (dma_fence_get_rcu(&other->base)) {
					dma_fence_put(&pt->base);
					pt = other;
					goto unlock;
				}
				p = &parent->rb_left;
			}
		}
		rb_link_node(&pt->node, parent, p);
		rb_insert_color(&pt->node, &obj->pt_tree);

		parent = rb_next(&pt->node);
		list_add_tail(
			&pt->link,
			parent ? &rb_entry(parent, typeof(*pt), node)->link
			       : &obj->pt_list);
	}
unlock:
	spin_unlock_irq(&obj->lock);

	return pt;
}

static const char *
mtk_sync_timeline_fence_get_driver_name(struct dma_fence *fence)
{
	return "mtk_sync";
}

static const char *
mtk_sync_timeline_fence_get_timeline_name(struct dma_fence *fence)
{
	struct sync_timeline *parent = dma_fence_parent(fence);

	return parent->name;
}

static void mtk_sync_timeline_fence_release(struct dma_fence *fence)
{
	struct sync_pt *pt = fence_to_sync_pt(fence);
	struct sync_timeline *parent = dma_fence_parent(fence);

	if (!list_empty(&pt->link)) {
		unsigned long flags;

		spin_lock_irqsave(fence->lock, flags);
		if (!list_empty(&pt->link)) {
			list_del(&pt->link);
			rb_erase(&pt->node, &parent->pt_tree);
		}
		spin_unlock_irqrestore(fence->lock, flags);
	}

	mtk_sync_timeline_put(parent);
	dma_fence_free(fence);
}

static bool mtk_sync_timeline_fence_signaled(struct dma_fence *fence)
{
	struct sync_timeline *parent = dma_fence_parent(fence);

	return !__dma_fence_is_later(fence->seqno, parent->value);
}

static bool mtk_sync_timeline_fence_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static void mtk_sync_timeline_fence_value_str(
					      struct dma_fence *fence,
					      char *str,
					      int size)
{
	snprintf(str, size, "%d", fence->seqno);
}

static void mtk_sync_timeline_fence_timeline_value_str(struct dma_fence *fence,
						       char *str, int size)
{
	struct sync_timeline *parent = dma_fence_parent(fence);

	snprintf(str, size, "%d", parent->value);
}

static struct dma_fence_ops mtk_sync_timeline_fence_ops = {
	.get_driver_name = mtk_sync_timeline_fence_get_driver_name,
	.get_timeline_name = mtk_sync_timeline_fence_get_timeline_name,
	.enable_signaling = mtk_sync_timeline_fence_enable_signaling,
	.signaled = mtk_sync_timeline_fence_signaled,
	.wait = dma_fence_default_wait,
	.release = mtk_sync_timeline_fence_release,
	.fence_value_str = mtk_sync_timeline_fence_value_str,
	.timeline_value_str = mtk_sync_timeline_fence_timeline_value_str,
};

/* ---------------------------------------------------------------- */

/**
 * mtk_sync_timeline_signal() - signal a status change on a sync_timeline
 * @obj:	sync_timeline to signal
 * @inc:	num to increment on timeline->value
 *
 * A sync implementation should call this any time one of it's fences
 * has signaled or has an error condition.
 */
static void mtk_sync_timeline_signal(struct sync_timeline *obj,
				     unsigned int inc)
{
	struct sync_pt *pt, *next;

	spin_lock_irq(&obj->lock);

	obj->value += inc;

	list_for_each_entry_safe(pt, next, &obj->pt_list, link) {
		if (!mtk_sync_timeline_fence_signaled(&pt->base))
			break;

		list_del_init(&pt->link);
		rb_erase(&pt->node, &obj->pt_tree);

		/*
		 * A signal callback may release the last reference to this
		 * fence, causing it to be freed. That operation has to be
		 * last to avoid a use after free inside this loop, and must
		 * be after we remove the fence from the timeline in order to
		 * prevent deadlocking on timeline->lock inside
		 * timeline_fence_release().
		 */
		dma_fence_signal_locked(&pt->base);
	}

	spin_unlock_irq(&obj->lock);
}

struct sync_timeline *mtk_sync_timeline_create(const char *name)
{
	return sync_timeline_create(name);
}

void mtk_sync_timeline_destroy(struct sync_timeline *obj)
{
	mtk_sync_timeline_put(obj);
}

void mtk_sync_timeline_inc(struct sync_timeline *obj, u32 value)
{
	mtk_sync_timeline_signal(obj, value);
}

int mtk_sync_fence_create(struct sync_timeline *obj, struct fence_data *data)
{
	int fd = get_unused_fd_flags(O_CLOEXEC);
	int err;
	struct sync_pt *pt;
	struct sync_file *sync_file;

	if (fd < 0)
		return fd;

	pt = mtk_sync_pt_create(obj, data->value);
	if (!pt) {
		err = -ENOMEM;
		goto err;
	}

	sync_file = sync_file_create(&pt->base);
	dma_fence_put(&pt->base);
	if (!sync_file) {
		err = -ENOMEM;
		goto err;
	}

	data->fence = fd;

	fd_install(fd, sync_file->file);

	return 0;

err:
	put_unused_fd(fd);
	return err;
}
