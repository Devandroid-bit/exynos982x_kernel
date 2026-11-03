#include <linux/input/input_booster.h>
#include <linux/exynos-ucc.h>
#include <linux/ems_service.h>

static struct pm_qos_request cluster2_qos;
static struct pm_qos_request cluster1_qos;
static struct pm_qos_request cluster0_qos;
static struct pm_qos_request mif_qos;
static struct pm_qos_request int_qos;
static struct kpp kpp_ta;
static struct kpp kpp_fg;
static struct ucc_req ucc_req = {
	.name = "input",
};

static DEFINE_MUTEX(input_lock);
bool current_hmp_boost = INIT_ZERO;
bool current_ucc_boost = INIT_ZERO;

struct inform {
	void *qos;
	void (*set_func)(int);
	int release_value;
};
struct inform informations[MAX_RES_COUNT];

void set_ucc(int enable)
{
	mutex_lock(&input_lock);

	if (enable != current_ucc_boost) {
		pr_booster("[Input Booster2] ******      set_ucc : %d ( %s )\n", enable, __FUNCTION__);
		if (enable) {
			ucc_add_request(&ucc_req, enable);
		} else {
			ucc_remove_request(&ucc_req);
		}
		current_ucc_boost = enable;
	}

	mutex_unlock(&input_lock);
}

void set_hmp(int enable)
{
	mutex_lock(&input_lock);

	if (enable != current_hmp_boost) {
		pr_booster("[Input Booster2] ******      set_ehmp : %d ( %s )\n", enable, __FUNCTION__);
		if (enable) {
			kpp_request(STUNE_TOPAPP, &kpp_ta, enable);
			kpp_request(STUNE_FOREGROUND, &kpp_fg, enable);
		} else {
			kpp_request(STUNE_TOPAPP, &kpp_ta, 0);
			kpp_request(STUNE_FOREGROUND, &kpp_fg, 0);
		}
		current_hmp_boost = enable;
	}

	mutex_unlock(&input_lock);
}

static void ib_update_resource(int res_id, int value)
{
	if (res_id < 0 || res_id >= MAX_RES_COUNT)
		return;

	if (informations[res_id].qos)
		pm_qos_update_request(informations[res_id].qos, value);
	else if (informations[res_id].set_func)
		informations[res_id].set_func(value);
}

void ib_set_booster(long *qos_values)
{
	int res_type = 0;
	int cur_res_idx;
	long value = -1;

	for (res_type = 0; res_type < allowed_res_count; res_type++) {

		cur_res_idx = allowed_resources[res_type];
		if (cur_res_idx < 0 || cur_res_idx >= MAX_RES_COUNT)
			continue;

		value = qos_values[cur_res_idx];
		if (value <= 0)
			continue;

		ib_update_resource(cur_res_idx, (int)value);
	}
}

void ib_release_booster(long *rel_flags)
{
	int res_type = 0;
	int cur_res_idx;
	long flag = -1;

	for (res_type = 0; res_type < allowed_res_count; res_type++) {

		cur_res_idx = allowed_resources[res_type];
		if (cur_res_idx < 0 || cur_res_idx >= MAX_RES_COUNT)
			continue;

		flag = rel_flags[cur_res_idx];
		if (flag <= 0)
			continue;

		ib_update_resource(cur_res_idx,
				   informations[cur_res_idx].release_value);
	}
}

int input_booster_init_vendor(void)
{
	int i = 0;

	pm_qos_add_request(&cluster2_qos, PM_QOS_CLUSTER2_FREQ_MIN, PM_QOS_CLUSTER2_FREQ_MIN_DEFAULT_VALUE);
	pm_qos_add_request(&cluster1_qos, PM_QOS_CLUSTER1_FREQ_MIN, PM_QOS_CLUSTER1_FREQ_MIN_DEFAULT_VALUE);
	pm_qos_add_request(&cluster0_qos, PM_QOS_CLUSTER0_FREQ_MIN, PM_QOS_CLUSTER0_FREQ_MIN_DEFAULT_VALUE);
	pm_qos_add_request(&mif_qos, PM_QOS_BUS_THROUGHPUT, PM_QOS_BUS_THROUGHPUT_DEFAULT_VALUE);
	pm_qos_add_request(&int_qos, PM_QOS_DEVICE_THROUGHPUT, PM_QOS_DEVICE_THROUGHPUT_DEFAULT_VALUE);

	informations[i++].qos = &cluster2_qos;
	informations[i++].qos = &cluster1_qos;
	informations[i++].qos = &cluster0_qos;
	informations[i++].qos = &mif_qos;
	informations[i++].qos = &int_qos;
	informations[i++].set_func = set_hmp;
	informations[i++].set_func = set_ucc;

	for (i = 0; i < MAX_RES_COUNT; i++)
		informations[i].release_value = release_val[i];

	return 1;
}

void input_booster_exit_vendor(void)
{
	pm_qos_remove_request(&cluster2_qos);
	pm_qos_remove_request(&cluster1_qos);
	pm_qos_remove_request(&cluster0_qos);
	pm_qos_remove_request(&mif_qos);
	pm_qos_remove_request(&int_qos);
}
