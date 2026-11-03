#define ITAG " [Evdev Booster] "
#include <linux/input/input_booster.h>

#include <linux/input.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/syscalls.h>

#if IS_ENABLED(CONFIG_SEC_INPUT_BOOSTER)
spinlock_t ib_ev_lock;
struct workqueue_struct *ev_unbound_wq;
static struct device *evbst_dev;

void input_booster(struct ib_event_data *ib_ev_data);

int evdev_ib_notify_callback(struct notifier_block *nb,
	unsigned long event_type, void *data)
{
	if (ib_init_succeed) {
		spin_lock(&ib_ev_lock);
		struct ib_event_data *ib_ev_data = data;

		switch (event_type) {
		case IB_EVENT_TOUCH_BOOSTER:
			input_booster(ib_ev_data);
			break;
		default:
			break;
		}
		spin_unlock(&ib_ev_lock);
	}

	return 0;
}

static struct notifier_block ib_event_notifier = {
	 .notifier_call = evdev_ib_notify_callback,
	 .priority = 1,
};

#if defined(CONFIG_SEC_INPUT_BOOSTER_HANDLER)
struct workqueue_struct *ib_unbound_highwq;

struct ib_queued_event {
	struct work_struct work;
	int evt_cnt;
	struct input_value vals[];
};

int ib_notifier_register(struct notifier_block *nb) {
	/* nothing to do here */
	return 0;
}

int ib_notifier_unregister(struct notifier_block *nb) {
	/* nothing to do here */
	return 0;
}

static void evdev_ib_trigger(struct work_struct *work)
{
	struct ib_queued_event *ib_work =
		container_of(work, struct ib_queued_event, work);
	struct ib_event_data ib_data = {0, };

	spin_lock(&ib_ev_lock);
	ib_data.evt_cnt = ib_work->evt_cnt;
	ib_data.vals = ib_work->vals;
	input_booster(&ib_data);
	spin_unlock(&ib_ev_lock);

	kfree(ib_work);
}

static void sec_input_boost_events(struct input_handle *handle,
			 const struct input_value *vals, unsigned int count)
{
	struct ib_queued_event *ib_work;
	unsigned int alloc_size;

	if (!count || !vals || !ib_unbound_highwq)
		return;

	if (count > MAX_EVENT_COUNT) {
		pr_warn_ratelimited(ITAG"event count(%u) exceeds max(%d), dropped\n",
					count, MAX_EVENT_COUNT);
		return;
	}

	alloc_size = sizeof(*ib_work) + count * sizeof(struct input_value);
	ib_work = kmalloc(alloc_size, GFP_ATOMIC);
	if (!ib_work)
		return;

	INIT_WORK(&ib_work->work, evdev_ib_trigger);
	ib_work->evt_cnt = count;
	memcpy(ib_work->vals, vals, count * sizeof(struct input_value));

	if (!queue_work(ib_unbound_highwq, &ib_work->work))
		kfree(ib_work);
}

static void sec_input_boost_event(struct input_handle *handle,
			unsigned int type, unsigned int code, int value)
{
	struct input_value vals[] = { { type, code, value } };

	sec_input_boost_events(handle, vals, 1);
}

static int sec_input_boost_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	/*
		dev_name(&dev->dev) : eventX
		dev->name : input driver name (like touchsreen)
	*/
	handle->dev = input_get_device(dev);
	handle->name = "sec_input_booster";
	handle->handler = handler;

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	input_put_device(handle->dev);
	kfree(handle);
	return error;
}

static void sec_input_boost_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	input_put_device(handle->dev);
	kfree(handle);
}

static const struct input_device_id sec_input_boost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y)
		},
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y)
		},
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler ib_input_handler = {
	.event		= sec_input_boost_event,
	.events		= sec_input_boost_events,
	.connect	= sec_input_boost_connect,
	.disconnect	= sec_input_boost_disconnect,
	.name		= "sec_input_booster",
	.id_table	= sec_input_boost_ids,
};

static int init_input_handler(void)
{
	int err;

	ib_unbound_highwq
		= alloc_ordered_workqueue("ib_unbound_highwq", WQ_HIGHPRI);
	if (!ib_unbound_highwq)
		return -ENOMEM;

	err = input_register_handler(&ib_input_handler);
	if (err) {
		destroy_workqueue(ib_unbound_highwq);
		ib_unbound_highwq = NULL;
	}

	return err;
}
#endif

int chk_next_data(const struct input_value *vals, int next_idx, int input_type)
{
	int ret_val = 0;
	int next_type = -1;
	int next_code = -1;

	next_type = vals[next_idx].type;
	next_code = vals[next_idx].code;

	switch (input_type) {
	case BTN_TOUCH:
		if ((next_type == EV_ABS) && (next_code == ABS_PRESSURE))
			ret_val = 1;
		break;
	case EV_KEY:
		ret_val = 1;
		break;
	default:
		break;
	}

	return ret_val;
}

int chk_boost_on_off(const struct input_value *vals, int idx, int dev_type)
{
	int ret_val = -1;

	if (dev_type < 0)
		return ret_val;

	/* In case of SPEN or HOVER, it must be empty multi event
	 * Before starting input booster.
	 */
	if (dev_type == SPEN || dev_type == HOVER) {
		if (!evdev_mt_event[dev_type] && vals[idx].value)
			ret_val = 1;
		else if (evdev_mt_event[dev_type] && !vals[idx].value)
			ret_val = 0;
	} else if (dev_type == TOUCH || dev_type == MULTI_TOUCH) {
		if (vals[idx].value >= 0)
			ret_val = 1;
		else
			ret_val = 0;
	} else if (vals[idx].value > 0)
		ret_val = 1;
	else if (vals[idx].value <= 0)
		ret_val = 0;

	return ret_val;
}

/*
 * get_device_type : Define type of device for input_booster.
 * device_type : Current device that in which input events triggered.
 * keyId : Each device get given unique keyid using Type, Code, Slot values
 *  to identify which booster will be triggered.
 * cur_idx : Pointing current handling index from input booster.
 *  cur_idx will be updated when cur_idx is same as head.
 * vals : Event set to determine what booster would be triggered.
 * evt_cnt : Total count of event in this time.
 */
int get_device_type(int *device_type, unsigned int *keyId,
					int *cur_idx, const struct input_value *vals, int evt_cnt)
{
	int i;
	int ret_val = -1;
	int dev_type = NONE_TYPE_DEVICE;
	int uniq_slot = 0;
	int next_idx = 0 ;
	int target_idx = 0;

	if (evt_cnt > MAX_EVENTS) {
		pr_warn_ratelimited(ITAG"Exceed max event number(%d)\n", evt_cnt);
		*cur_idx = evt_cnt;
		return ret_val;
	}

	/* Initializing device type before finding the proper device type. */
	*device_type = dev_type;

	for (i = *cur_idx; i < evt_cnt; i++) {
		pr_booster(" %s Type : %d, Code : %d, Value : %d, idx : %d\n",
			"Input Data || ", vals[i].type,
			vals[i].code, vals[i].value, i);

		if (vals[i].type == EV_SYN || vals[i].code == SYN_REPORT) {
			break;
		}

		if (vals[i].type == EV_KEY) {
			target_idx = i;
			switch (vals[i].code) {
			case BTN_TOUCH:
				next_idx = i + 1;
				if (next_idx >= evt_cnt)
					break;
				if (!chk_next_data(vals, next_idx, BTN_TOUCH))
					break;
				dev_type = SPEN;
				break;
			case BTN_TOOL_PEN:
				dev_type = HOVER;
				break;
			case KEY_BACK:
			case KEY_HOMEPAGE:
				dev_type = TOUCH_KEY;
				break;
			case KEY_VOLUMEUP:
			case KEY_VOLUMEDOWN:
			case KEY_POWER:
#if IS_ENABLED(CONFIG_SOC_S5E5515) // watch - lower key(back key) Code : 0x244(580)
			case KEY_APPSELECT:
#endif
				dev_type = KEY;
				break;
			default:
				break;
			}

		} else if (vals[i].type == EV_ABS) {
			target_idx = i;
			switch (vals[i].code) {
			case ABS_MT_TRACKING_ID:
				if (vals[i].value >= 0) {
					evdev_mt_slot++;
				} else {
					evdev_mt_slot--;
					if (evdev_mt_slot < 0) {
						evdev_mt_slot = 0;
						break;
					}
				}

				if (vals[i].value >= 0) {
					if (evdev_mt_slot == 1) {
						dev_type = TOUCH;
						uniq_slot = 1;
					} else if (evdev_mt_slot == 2) {
						dev_type = MULTI_TOUCH;
						uniq_slot = 2;
					}
				} else if (vals[i].value < 0) {
					if (evdev_mt_slot == 0) {
						dev_type = TOUCH;
						uniq_slot = 1;
					} else if (evdev_mt_slot == 1) {
						dev_type = MULTI_TOUCH;
						uniq_slot = 2;
					}
				}

				pr_booster("Touch Booster Trigger(%d), T(%d) C(%d) V(%d), Idx(%d), Cnt(%d)",
							evdev_mt_slot,
							vals[i].type, vals[i].code, vals[i].value,
							i, evt_cnt);

				break;
			}
		} else if (vals[i].type == EV_MSC &&
					vals[i].code == MSC_SCAN) {
			next_idx = i + 1;
			if (next_idx >= evt_cnt)
				break;
			if (!chk_next_data(vals, next_idx, EV_KEY))
				break;

			i = next_idx;
			target_idx = next_idx;
			switch (vals[next_idx].code) {
			case BTN_LEFT: /* Checking Touch Button Event */
			case BTN_RIGHT:
			case BTN_MIDDLE:
				dev_type = MOUSE;
				//Remain the last of CODE value as a uniq_slot to recognize BTN Type (LEFT, RIGHT, MIDDLE)
				uniq_slot = vals[next_idx].code;
				break;
			default: /* Checking Keyboard Event */
				dev_type = KEYBOARD;
				uniq_slot = vals[next_idx].code;

				pr_booster("KBD Booster Trigger(%d), Type(%d), Code(%d), Val(%d), Idx(%d), Cnt(%d)\n",
						vals[next_idx].code,
						vals[i].type, vals[i].code, vals[i].value,
						i, evt_cnt);
				break;
			}
		}

		if (dev_type != NONE_TYPE_DEVICE ) {
			*keyId = create_uniq_id(vals[i].type, vals[i].code, uniq_slot);
			ret_val = chk_boost_on_off(vals, target_idx, dev_type);
			pr_booster("Dev type Find(%d), KeyID(%d), enable(%d), Target(%d)\n",
				dev_type, *keyId, ret_val, target_idx);
			break;
		}

	}

	*cur_idx = i+1;

	*device_type = dev_type;
	return ret_val;
}

static void evdev_ib_boost_trigger(struct work_struct *work)
{
	struct t_ib_trigger *trigger =
		container_of(work, struct t_ib_trigger, ib_trigger_work);

	trigger_input_booster(work);
	kfree(trigger);
}

static bool queue_ib_boost_trigger(int keyId, int enable, int dev_type)
{
	struct t_ib_trigger *trigger;

	trigger = kmalloc(sizeof(*trigger), GFP_ATOMIC);
	if (!trigger)
		return false;

	trigger->key_id = keyId;
	trigger->event_type = enable;
	trigger->dev_type = dev_type;
	INIT_WORK(&trigger->ib_trigger_work, evdev_ib_boost_trigger);

	if (!queue_work(ev_unbound_wq, &trigger->ib_trigger_work)) {
		kfree(trigger);
		return false;
	}

	return true;
}

// ********** Detect Events ********** //
//void input_booster(int head, int bufsize)
void input_booster(struct ib_event_data *ib_ev_data)
{
#if IS_ENABLED(CONFIG_SEC_INPUT_BOOSTER_QC) || \
	IS_ENABLED(CONFIG_SEC_INPUT_BOOSTER_SLSI) || \
	IS_ENABLED(CONFIG_SEC_INPUT_BOOSTER_MTK)
	int dev_type = 0;
	int keyId = 0;
	int cur_idx = -1;

	pr_booster("%s Triggered :: evt_cnt(%d), ib_init_succeed(%d)",
		__func__, ib_ev_data->evt_cnt, ib_init_succeed);

	if (!ib_init_succeed || ib_ev_data->evt_cnt == 0) {
		pr_err(ITAG"ev_cnt(%d) dev is Null OR dt_infor hasn't mem alloc\n", ib_ev_data->evt_cnt);
		return;
	}

	if (!ib_ev_data->vals || ib_ev_data->evt_cnt > MAX_EVENTS) {
		pr_warn_ratelimited(ITAG"invalid event batch, cnt(%d)\n",
					ib_ev_data->evt_cnt);
		return;
	}

	if (!ev_unbound_wq)
		return;

	cur_idx = 0;

	while (cur_idx < ib_ev_data->evt_cnt) {

		keyId = 0;

		int enable = get_device_type(&dev_type, &keyId, &cur_idx, ib_ev_data->vals, ib_ev_data->evt_cnt);
		if (enable < 0 || keyId == 0) {
			continue;
		}

		if (dev_type <= NONE_TYPE_DEVICE || dev_type >= MAX_DEVICE_TYPE_NUM) {
			continue;
		}

		if (enable == BOOSTER_ON) {
			evdev_mt_event[dev_type]++;
		} else {
			evdev_mt_event[dev_type]--;
		}

		pr_booster("Device_Type(%d), KeyId(%d), enable(%d)",
			dev_type, keyId, enable);

		if (!queue_ib_boost_trigger(keyId, enable, dev_type)) {
			if (enable == BOOSTER_ON)
				evdev_mt_event[dev_type]--;
			else
				evdev_mt_event[dev_type]++;
		}
	}
#endif
}

static void evbst_dev_release(struct device *dev)
{
	kfree(dev);
}

static int __init ev_boost_init(void)
{
	int err;

	pr_info(ITAG" Input Booster Module Init\n");
	input_booster_init();
	pr_info(ITAG" Input Booster Module Init End\n");
	spin_lock_init(&ib_ev_lock);
	ib_notifier_register(&ib_event_notifier);

	ev_unbound_wq =
		alloc_ordered_workqueue("ev_unbound_wq", WQ_HIGHPRI);
	if (!ev_unbound_wq) {
		pr_err(ITAG" ev_unbound_wq alloc fail\n");
		err = -ENOMEM;
		goto err_notifier;
	}

	evbst_dev = kzalloc(sizeof(*evbst_dev), GFP_KERNEL);
	if (!evbst_dev) {
		pr_err(ITAG" evbst_dev alloc fail\n");
		err = -ENOMEM;
		goto err_wq;
	}
	dev_set_name(evbst_dev, "evdev_booster_dev");
	evbst_dev->release = evbst_dev_release;

	err = device_register(evbst_dev);
	if (err) {
		pr_err(ITAG" evdev device register failed\n");
		put_device(evbst_dev);
		evbst_dev = NULL;
		goto err_wq;
	}

#if defined(CONFIG_SEC_INPUT_BOOSTER_HANDLER)
	err = init_input_handler();
	if (err) {
		pr_err(ITAG" input handler fail\n");
		goto err_dev;
	}
#endif

	return 0;

#if defined(CONFIG_SEC_INPUT_BOOSTER_HANDLER)
err_dev:
	device_unregister(evbst_dev);
	evbst_dev = NULL;
#endif
err_wq:
	destroy_workqueue(ev_unbound_wq);
	ev_unbound_wq = NULL;
err_notifier:
	ib_notifier_unregister(&ib_event_notifier);
	input_booster_exit();
	return err;
}

static void __exit ev_boost_exit(void)
{
#if defined(CONFIG_SEC_INPUT_BOOSTER_HANDLER)
	input_unregister_handler(&ib_input_handler);
	if (ib_unbound_highwq) {
		destroy_workqueue(ib_unbound_highwq);
		ib_unbound_highwq = NULL;
	}
#endif
	ib_notifier_unregister(&ib_event_notifier);
	if (ev_unbound_wq) {
		destroy_workqueue(ev_unbound_wq);
		ev_unbound_wq = NULL;
	}
	if (evbst_dev) {
		device_unregister(evbst_dev);
		evbst_dev = NULL;
	}
	input_booster_exit();
}

late_initcall(ev_boost_init);
module_exit(ev_boost_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hongc.shim");
MODULE_DESCRIPTION("EVDEV Booster in kernel");
#endif //--CONFIG_SEC_INPUT_BOOSTER
