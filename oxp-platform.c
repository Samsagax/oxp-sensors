// SPDX-License-Identifier: GPL-2.0+
/*
 * Platform driver for OXP Handhelds that expose fan readings and controls
 * via hwmon sysfs.
 *
 * All boards have the same DMI strings and they are told appart by the
 * boot cpu vendor (Intel/AMD).
 * Fan control is provided via pwm interface in the range [0-255]. Intel
 * boards have that range but AMD ones use [0-100] as range, the written
 * value is scaled to accomodate for that.
 * Intel boards do not provide fan RPM reading but can be infered by PWM
 * value set if requested via module parameter (don't report incorrect
 * valuesby default).
 * PWM control is disabled by default, can be enabled via module parameter.
 *
 * Copyright (C) 2022 Joaquín I. Aramendía <samsagax@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/dev_printk.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <asm/processor.h>

static bool fan_control;
module_param_hw(fan_control, bool, other, 0644);
MODULE_PARM_DESC(fan_control, "Enable fan control");

static bool fan_input_intel;
module_param_hw(fan_input_intel, bool, other, 0644);
MODULE_PARM_DESC(fan_input_intel, "Emulate fan reading on intel boards");

#define ACPI_LOCK_DELAY_MS	500

/* Handle ACPI lock mechanism */
struct lock_data {
	u32 mutex;
	bool (*lock)(struct lock_data *data);
	bool (*unlock)(struct lock_data *data);
};

static bool lock_global_acpi_lock(struct lock_data *data)
{
	return ACPI_SUCCESS(acpi_acquire_global_lock(ACPI_LOCK_DELAY_MS,
								 &data->mutex));
}

static bool unlock_global_acpi_lock(struct lock_data *data)
{
	return ACPI_SUCCESS(acpi_release_global_lock(data->mutex));
}

#define MAX_IDENTICAL_BOARD_VARIATIONS	2

enum board_family {
	family_unknown,
	family_mini_amd,
	family_mini_intel,
};

enum oxp_sensor_type {
	oxp_sensor_fan = 0,
	oxp_sensor_pwm,
	oxp_sensor_number,
};

struct oxp_ec_sensor_addr {
	enum hwmon_sensor_types type;
	u8 reg;
	short size;
	union {
		struct {
			u8 enable;
			u8 val_enable;
			u8 val_disable;
		};
		struct {
		  int max_speed;
		};
	};
};


/* AMD board EC addresses */
static const struct oxp_ec_sensor_addr amd_sensors[] = {
	[oxp_sensor_fan] = {
		.type = hwmon_fan,
		.reg = 0x76,
		.size = 2,
		.max_speed = 5000,
	},
	[oxp_sensor_pwm] = {
		.type = hwmon_pwm,
		.reg = 0x4B,
		.size = 1,
		.enable = 0x4A,
		.val_enable = 0x01,
		.val_disable = 0x00,
	},
	{},
};

/* Intel board EC addresses */
static const struct oxp_ec_sensor_addr intel_sensors[] = {
	[oxp_sensor_fan] = {
		.type = hwmon_fan,
		.reg = 0xC5,	/* PWM address if we are emulating RPM */
		.size = 1,
		.max_speed = 4700,
	},
	[oxp_sensor_pwm] = {
		.type = hwmon_pwm,
		.reg = 0xC5,
		.size = 1,
		.enable = 0xcA,
		.val_enable = 0x88,
		.val_disable = 0xC4,
	},
	{}
};


struct ec_board_info {
	const char *board_names[MAX_IDENTICAL_BOARD_VARIATIONS];
	enum board_family family;
	const struct oxp_ec_sensor_addr *sensors;
};

static const struct ec_board_info board_info[] = {
	{
		.board_names = {"ONE XPLAYER", "ONEXPLAYER mini A07"},
		.family = family_mini_amd,
		.sensors = amd_sensors,
	},
	{
		.board_names = {"ONE XPLAYER"},
		.family = family_mini_intel,
		.sensors = intel_sensors,
	},
	{}
};

struct oxp_status {
	struct ec_board_info board;
	struct lock_data lock_data;
};

/* Helper functions */
static int read_from_ec(u8 reg, int size, long *val)
{
	int i;
	int ret;
	u8 buffer;

	*val = 0;
	for (i = 0; i < size; i++) {
		ret = ec_read(reg + i, &buffer);
		if (ret)
			return ret;
		(*val) <<= i*8;
		*val += buffer;
	}
	return ret;
}

static int write_to_ec(const struct device *dev, u8 reg, u8 value)
{
	struct oxp_status *state = dev_get_drvdata(dev);
	int ret = -1;

	if (!state->lock_data.lock(&state->lock_data)) {
		dev_warn(dev, "Failed to acquire mutex");
		return -EBUSY;
	}

	ret = ec_write(reg, value);

	if (!state->lock_data.unlock(&state->lock_data))
		dev_err(dev, "Failed to release mutex");

	return ret;
}

/* Callbacks for hwmon interface */
static umode_t oxp_ec_hwmon_is_visible(const void *drvdata,
					enum hwmon_sensor_types type, u32 attr, int channel)
{
	const struct oxp_status *state = drvdata;

	switch (type) {
		case hwmon_fan:
			if (!fan_input_intel && state->board.family == family_mini_intel) {
				return 0;
			}
			return S_IRUGO;
		case hwmon_pwm:
			return S_IRUGO | S_IWUSR;
		default:
			return 0;
	}
	return 0;
}

static int oxp_platform_read(struct device *dev, enum hwmon_sensor_types type,
		u32 attr, int channel, long *val)
{
	int ret = -1;
	const struct oxp_status *state = dev_get_drvdata(dev);
	const struct ec_board_info *board = &state->board;

	switch(type) {
		case hwmon_fan:
			switch(attr) {
				case hwmon_fan_input:
					if (board->family == family_mini_intel && !fan_input_intel) {
						return -EINVAL;
					}
					ret = read_from_ec(board->sensors[oxp_sensor_fan].reg,
							board->sensors[oxp_sensor_fan].size, val);
					if (board->family == family_mini_intel) {
						*val = (*val * board->sensors[oxp_sensor_fan].max_speed) / 255;
					}
					break;
				case hwmon_fan_max:
					ret = 0;
					*val = board->sensors[oxp_sensor_fan].max_speed;
					break;
				case hwmon_fan_min:
					ret = 0;
					*val = 0;
					break;
				default:
					dev_dbg(dev, "Unknown attribute for type %d: %d\n", type, attr);
			}
			return ret;
		case hwmon_pwm:
			switch(attr) {
				case hwmon_pwm_input:
					ret = read_from_ec(board->sensors[oxp_sensor_pwm].reg,
							board->sensors[oxp_sensor_pwm].size, val);
					if (board->family == family_mini_amd) {
						*val = (*val * 255) / 100;
					}
					break;
				case hwmon_pwm_enable:
					ret = read_from_ec(board->sensors[oxp_sensor_pwm].enable, 1, val);
					break;
				default:
					dev_dbg(dev, "Unknown attribute for type %d: %d\n", type, attr);
			}
			return ret;
		default:
			dev_dbg(dev, "Unknown sensor type %d.\n", type);
			return -1;
	}
}

static int oxp_pwm_enable(const struct device *dev)
{
	int ret = -1;
	struct oxp_status *state = dev_get_drvdata(dev);
	const struct ec_board_info *board = &state->board;

	if (!fan_control)
		return -EINVAL;

	ret = write_to_ec(dev, board->sensors[oxp_sensor_pwm].enable,
		board->sensors[oxp_sensor_pwm].val_enable);

	return ret;
}

static int oxp_pwm_disable(const struct device *dev)
{
	int ret = -1;
	struct oxp_status *state = dev_get_drvdata(dev);
	const struct ec_board_info *board = &state->board;

	if (!fan_control)
		return -EINVAL;

	ret = write_to_ec(dev, board->sensors[oxp_sensor_pwm].enable,
		board->sensors[oxp_sensor_pwm].val_disable);

	return ret;
}

static int oxp_platform_write(struct device *dev, enum hwmon_sensor_types type,
		u32 attr, int channel, long val)
{
	int ret = -1;
	struct oxp_status *state = dev_get_drvdata(dev);
	const struct ec_board_info *board = &state->board;
	const struct oxp_ec_sensor_addr *sensor;

	switch(type) {
		case hwmon_pwm:
			if (!fan_control)
				return -EINVAL;
			switch(attr) {
				case hwmon_pwm_enable:
					if (val == 1) {
						ret = oxp_pwm_enable(dev);
					} else if (val == 0) {
						ret = oxp_pwm_disable(dev);
					} else {
						return -EINVAL;
					}
					return ret;
				case hwmon_pwm_input:
					if (val < 0 || val > 255)
						return -EINVAL;
					for (sensor = board->sensors; sensor->type; sensor++) {
						if (sensor->type == type) {
							if (!state->lock_data.lock(&state->lock_data)) {
								dev_warn(dev, "Failed to acquire mutex");
								return -EBUSY;
							}
							if (board->family == family_mini_amd) {
								val = (val * 100) / 255;
							}

							ret = ec_write(sensor->reg, val);

							if (!state->lock_data.unlock(&state->lock_data))
								dev_err(dev, "Failed to release mutex");
						}
					}
					return ret;
				default:
					dev_dbg(dev, "Unknown attribute for type %d: %d", type, attr);
					return ret;
			}
		default:
			dev_dbg(dev, "Unknown sensor type: %d", type);
	}
	return ret;
}

/* Known sensors in the OXP EC controllers */
static const struct hwmon_channel_info *oxp_platform_sensors[] = {
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT | HWMON_F_MAX | HWMON_F_MIN),
	HWMON_CHANNEL_INFO(pwm,
		HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops oxp_ec_hwmon_ops = {
	.is_visible = oxp_ec_hwmon_is_visible,
	.read = oxp_platform_read,
	.write = oxp_platform_write,
};

static const struct hwmon_chip_info oxp_ec_chip_info = {
	.ops = &oxp_ec_hwmon_ops,
	.info = oxp_platform_sensors,
};

/* Initialization logic */
static const struct ec_board_info * __init get_board_info(void)
{
	const char *dmi_board_vendor = dmi_get_system_info(DMI_BOARD_VENDOR);
	const char *dmi_board_name = dmi_get_system_info(DMI_BOARD_NAME);
	const struct ec_board_info *board;

	if (!dmi_board_vendor || !dmi_board_name ||
	    (strcasecmp(dmi_board_vendor, "ONE-NETBOOK TECHNOLOGY CO., LTD.") &&
	     strcasecmp(dmi_board_vendor, "ONE-NETBOOK")))
		return NULL;

	/* Match our known boards */
	/* Need to check for AMD/Intel here */
	for (board = board_info; board->sensors; board++) {
		if (match_string(board->board_names,
				 MAX_IDENTICAL_BOARD_VARIATIONS,
				 dmi_board_name) >= 0) {
			if (board->family == family_mini_intel &&
					boot_cpu_data.x86_vendor == X86_VENDOR_INTEL) {
				return board;
			} else if (board->family == family_mini_amd &&
					boot_cpu_data.x86_vendor == X86_VENDOR_AMD) {
				return board;
			}
		}
	}
	return NULL;
}

static int __init oxp_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hwdev;
	const struct ec_board_info *pboard_info;
	struct oxp_status *state;

	pboard_info = get_board_info();
	if (!pboard_info)
		return -ENODEV;

	state = devm_kzalloc(dev, sizeof(struct oxp_status), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->board = *pboard_info;

	state->lock_data.mutex = 0;
	state->lock_data.lock = lock_global_acpi_lock;
	state->lock_data.unlock = unlock_global_acpi_lock;

	hwdev = devm_hwmon_device_register_with_info(dev, "oxpec", state,
							&oxp_ec_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwdev);
}

static const struct acpi_device_id acpi_ec_ids[] = {
	/* Embedded Controller Device */
	{ "PNP0C09", 0 },
	{}
};

static struct platform_driver oxp_platform_driver = {
	.driver = {
		.name	= "oxp-platform",
		.acpi_match_table = acpi_ec_ids,
	},
};

MODULE_DEVICE_TABLE(acpi, acpi_ec_ids);
module_platform_driver_probe(oxp_platform_driver, oxp_platform_probe);

MODULE_AUTHOR("Joaquín I. Aramendía <samsagax@gmail.com>");
MODULE_DESCRIPTION(
	"Platform driver that handles ACPI EC of ONEXPLAYER Devices");
MODULE_LICENSE("GPL");
