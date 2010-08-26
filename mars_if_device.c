// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

/* Interface to a Linux device.
 * 1 Input, 0 Outputs.
 */

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include <linux/major.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>

#include "mars.h"

///////////////////////// own type definitions ////////////////////////

#include "mars_if_device.h"

///////////////////////// own static definitions ////////////////////////

static int device_minor = 0;

//////////////// object / aspect constructors / destructors ///////////////

///////////////////////// linux operations ////////////////////////

/* callback
 */
static void _if_device_endio(struct generic_callback *cb)
{
	struct mars_ref_object *mref = cb->cb_private;
	struct bio *bio = mref->orig_bio;
	int error;
	if (unlikely(!bio)) {
		MARS_FAT("callback with no bio called. something is very wrong here!\n");
		return;
	}
	error = cb->cb_error;
	if (unlikely(error < 0)) {
		MARS_ERR("NYI: error=%d RETRY LOGIC %u\n", error, bio->bi_size);
	}
	if (likely(error > 0)) { // bio conventions are slightly different...
		error = 0;
		bio->bi_size = 0;
	}
	bio_endio(bio, error);
}

/* accept a linux bio, wrap it into mref and call buf_io() on it.
 */
static int if_device_make_request(struct request_queue *q, struct bio *bio)
{
	struct if_device_input *input;
	struct if_device_brick *brick;
	struct mars_ref_object *mref = NULL;
	struct if_device_mars_ref_aspect *mref_a;
	struct generic_callback *cb;
	int rw = bio->bi_rw & 1;
        int error = -ENOSYS;

	MARS_DBG("make_request(%d)\n", bio->bi_size);

	input = q->queuedata;
        if (unlikely(!input))
                goto err;

	brick = input->brick;
        if (unlikely(!brick))
                goto err;

	/* THIS IS PROVISIONARY
	 */
	while (unlikely(!brick->is_active)) {
		msleep(100);
	}

	error = -ENOMEM;
	mref = if_device_alloc_mars_ref(&brick->hidden_output, &input->mref_object_layout);
	if (unlikely(!mref))
		goto err;

	mref_a = if_device_mars_ref_get_aspect(&brick->hidden_output, mref);
	if (unlikely(!mref_a))
		goto err;
	cb = &mref_a->cb;
	cb->cb_fn = _if_device_endio;
	cb->cb_private = mref;
	cb->cb_error = 0;
	cb->cb_prev = NULL;
	mref->ref_cb = cb;

	mars_ref_attach_bio(mref, bio);

	GENERIC_INPUT_CALL(input, mars_ref_io, mref, rw);

	GENERIC_INPUT_CALL(input, mars_ref_put, mref);

	return 0;

err:
	MARS_ERR("cannot submit request, status=%d\n", error);
	if (!mref)
		bio_endio(bio, error);
	return error;
}

static int if_device_open(struct block_device *bdev, fmode_t mode)
{
	struct if_device_input *input = bdev->bd_disk->private_data;
	(void)input;
	MARS_DBG("if_device_open()\n");
	return 0;
}

static int if_device_release(struct gendisk *gd, fmode_t mode)
{
	MARS_DBG("if_device_close()\n");
	return 0;
}

static const struct block_device_operations if_device_blkdev_ops = {
	.owner =   THIS_MODULE,
	.open =    if_device_open,
	.release = if_device_release,

};

////////////////// own brick / input / output operations //////////////////

static void if_device_unplug(struct request_queue *q)
{
	//struct if_device_input *input = q->queuedata;
	MARS_DBG("UNPLUG\n");
	queue_flag_clear_unlocked(QUEUE_FLAG_PLUGGED, q);
	//blk_run_address_space(lo->lo_backing_file->f_mapping);
}


//////////////// object / aspect constructors / destructors ///////////////

static int if_device_mars_ref_aspect_init_fn(struct generic_aspect *_ini, void *_init_data)
{
	return 0;
}

static void if_device_mars_ref_aspect_exit_fn(struct generic_aspect *_ini, void *_init_data)
{
}

MARS_MAKE_STATICS(if_device);

//////////////////////// contructors / destructors ////////////////////////

static int if_device_brick_construct(struct if_device_brick *brick)
{
	struct if_device_output *hidden = &brick->hidden_output;
	_if_device_output_init(brick, hidden, "internal");
	return 0;
}

static int if_device_brick_destruct(struct if_device_brick *brick)
{
	return 0;
}

static int if_device_switch(struct if_device_brick *brick, bool state)
{
	struct if_device_input *input = brick->inputs[0];
	struct request_queue *q;
	struct gendisk *disk;
	int minor;
	struct mars_info info = {};
	unsigned long capacity;
	int status;

	//MARS_DBG("1\n");
	
	status = GENERIC_INPUT_CALL(input, mars_get_info, &info);
	if (status < 0) {
		MARS_ERR("cannot get device info, status=%d\n", status);
		return status;
	}
	capacity = info.current_size >> 9; // TODO: make this dynamic

	q = blk_alloc_queue(GFP_MARS);
	if (!q) {
		MARS_ERR("cannot allocate device request queue\n");
		return -ENOMEM;
	}
	q->queuedata = input;
	input->q = q;

	//MARS_DBG("2\n");
	disk = alloc_disk(1);
	if (!disk) {
		MARS_ERR("cannot allocate gendisk\n");
		return -ENOMEM;
	}

	//MARS_DBG("3\n");
	minor = device_minor++; //TODO: protect against races (e.g. atomic_t)
	disk->queue = q;
	disk->major = MARS_MAJOR; //TODO: make this dynamic for >256 devices
	disk->first_minor = minor;
	disk->fops = &if_device_blkdev_ops;
	sprintf(disk->disk_name, "mars%d", minor);
	MARS_DBG("created device name %s\n", disk->disk_name);
	disk->private_data = input;
	set_capacity(disk, capacity);

	blk_queue_make_request(q, if_device_make_request);
	blk_queue_max_segment_size(q, MARS_MAX_SEGMENT_SIZE);
	blk_queue_bounce_limit(q, BLK_BOUNCE_ANY);
	q->unplug_fn = if_device_unplug;
	spin_lock_init(&input->req_lock);
	q->queue_lock = &input->req_lock; // needed!
	//blk_queue_ordered(q, QUEUE_ORDERED_DRAIN, NULL);//???

	//MARS_DBG("4\n");
	input->bdev = bdget(MKDEV(disk->major, minor));
	/* we have no partitions. we contain only ourselves. */
	input->bdev->bd_contains = input->bdev;

#if 0 // ???
	q->backing_dev_info.congested_fn = mars_congested;
	q->backing_dev_info.congested_data = input;
#endif

#if 0 // ???
	blk_queue_merge_bvec(q, mars_merge_bvec);
#endif

	// point of no return
	//MARS_DBG("99999\n");
	add_disk(disk);
	input->disk = disk;
	//set_device_ro(input->bdev, 0); // TODO: implement modes
	brick->is_active = true;
	return 0;
}

static int if_device_input_construct(struct if_device_input *input)
{
	return 0;
}

static int if_device_input_destruct(struct if_device_input *input)
{
	if (input->bdev)
		bdput(input->bdev);
	if (input->disk) {
		del_gendisk(input->disk);
		//put_disk(input->disk);
	}
	if (input->q)
		blk_cleanup_queue(input->q);
	return 0;
}

static int if_device_output_construct(struct if_device_output *output)
{
	return 0;
}

///////////////////////// static structs ////////////////////////

static struct if_device_brick_ops if_device_brick_ops = {
	.brick_switch = if_device_switch,
};

static struct if_device_output_ops if_device_output_ops = {
	.make_object_layout = if_device_make_object_layout,
};

const struct if_device_input_type if_device_input_type = {
	.type_name = "if_device_input",
	.input_size = sizeof(struct if_device_input),
	.input_construct = &if_device_input_construct,
	.input_destruct = &if_device_input_destruct,
};

static const struct if_device_input_type *if_device_input_types[] = {
	&if_device_input_type,
};

const struct if_device_output_type if_device_output_type = {
	.type_name = "if_device_output",
	.output_size = sizeof(struct if_device_output),
	.master_ops = &if_device_output_ops,
	.output_construct = &if_device_output_construct,
	.aspect_types = if_device_aspect_types,
	.layout_code = {
		[BRICK_OBJ_MARS_REF] = LAYOUT_ALL,
	}
};
const struct if_device_brick_type if_device_brick_type = {
	.type_name = "if_device_brick",
	.brick_size = sizeof(struct if_device_brick),
	.max_inputs = 1,
	.max_outputs = 0,
	.master_ops = &if_device_brick_ops,
	.default_input_types = if_device_input_types,
	.brick_construct = &if_device_brick_construct,
	.brick_destruct = &if_device_brick_destruct,
};
EXPORT_SYMBOL_GPL(if_device_brick_type);

////////////////// module init stuff /////////////////////////

static void __exit exit_if_device(void)
{
	int status;
	printk(MARS_INFO "exit_if_device()\n");
	status = if_device_unregister_brick_type();
	unregister_blkdev(DRBD_MAJOR, "mars");
}

static int __init init_if_device(void)
{
	int status;

	(void)if_device_aspect_types; // not used, shut up gcc

	printk(MARS_INFO "init_if_device()\n");
	status = register_blkdev(DRBD_MAJOR, "mars");
	if (status)
		return status;
	status = if_device_register_brick_type();
	if (status)
		goto err_device;
	return status;
err_device:
	MARS_ERR("init_if_device() status=%d\n", status);
	exit_if_device();
	return status;
}

MODULE_DESCRIPTION("MARS if_device");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_if_device);
module_exit(exit_if_device);
