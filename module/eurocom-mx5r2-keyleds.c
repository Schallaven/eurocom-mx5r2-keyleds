/*
 * eurocom-mx5r2-keyleds.c
 *
 * Copyright (C) 2017 by Sven Kochmann, available at Github:
 * <https://www.github.com/Schallaven/eurocom-mx5r2-keyleds/>
 * 
 * Partly based on a tutorial availabe at:
 * <http://derekmolloy.ie/writing-a-linux-kernel-module-part-2-a-character-device/>
 *
 * Partly based on clevo-xsm-wmi by Arnoud Willemsen
 * Copyright (C) 2014-2016 Arnoud Willemsen <mail@lynthium.com>
 *
 * clevo-xsm-wmi is based on tuxedo-wmi by TUXEDO Computers GmbH
 * Copyright (C) 2013-2015 TUXEDO Computers GmbH <tux@tuxedocomputers.com>
 * Custom build Linux Notebooks and Computers: www.tuxedocomputers.com
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the  GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is  distributed in the hope that it  will be useful, but
 * WITHOUT  ANY   WARRANTY;  without   even  the  implied   warranty  of
 * MERCHANTABILITY  or FITNESS FOR  A PARTICULAR  PURPOSE.  See  the GNU
 * General Public License for more details.
 *
 * You should  have received  a copy of  the GNU General  Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This kernel module implements the keyboard LEDs on a Eurocom MX5-R2. In
 * contrast to the previous implementations (clevo-xsm-wmi and tuxedo-wmi)
 * it does not implement anything else. Do one thing, and only one thing.
 *
 * Compatible models:
 * - Eurocom MX5-R2
 *
 */

/* This is the name and class of the keyboard interface, which will be available at /sys/class/<classname>/<interfacename> */
#define EUROCOM_MX5R2_DRIVER_NAME "mx5kbleds"

/* For kernel output; should be before the includes to overwrite the default for pr_fmt. */
#define pr_fmt(fmt) EUROCOM_MX5R2_DRIVER_NAME ": " fmt
#define __EUROCOM_PR(lvl, fmt, ...) do { pr_##lvl(fmt, ##__VA_ARGS__); } while (0)
#define EUROCOM_INFO(fmt, ...) __EUROCOM_PR(info, fmt, ##__VA_ARGS__)
#define EUROCOM_ERROR(fmt, ...) __EUROCOM_PR(err, fmt, ##__VA_ARGS__)
#define EUROCOM_DEBUG(fmt, ...) __EUROCOM_PR(debug, "[%s:%u] " fmt, __func__, __LINE__, ##__VA_ARGS__)

/* Includes */
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/rfkill.h>
#include <linux/stringify.h>
#include <linux/version.h>
#include <linux/workqueue.h>


/* Definitions 
 * ------------------------------------------------------------------------------------------ */

/* This is the GUID of the wmi-interface (PNP0C14) we want to implement */
#define EUROCOM_MX5R2_GUID "ABBC0F6D-8EA1-11D1-00A0-C90629100000"

/* This is the wmi command for controlling the keyboard LEDs; see dsdt for reference */
#define SET_KB_LED              0x67  /* 103 */

/* Global things for the driver */
static struct platform_driver mx5r2_led_driver = {
	.driver		= {
		.name	= "mx5kbleds",
        .owner  = THIS_MODULE,
	},
};

/* Global pointer for the platform device */
struct platform_device *mx5r2_led_device;
 

/* These are the regions of the keyboard */
enum keyboard_regions {
    KB_REGION_LEFT,
    KB_REGION_CENTER,
    KB_REGION_RIGHT,
    KB_REGION_UNKNOWN
};

/* These are the modes the internal firmware recognizes (0-7); see dsdt for reference */
#define NUMBER_OF_KEYBOARD_MODES 8
enum {
    KB_MODE_RANDOM_COLOR,
	KB_MODE_CUSTOM,
	KB_MODE_BREATHE,
	KB_MODE_CYCLE,
	KB_MODE_WAVE,
	KB_MODE_DANCE,
	KB_MODE_TEMPO,
	KB_MODE_FLASH
} keyboard_mode = KB_MODE_CUSTOM;

static char* keyboard_mode_description[] = {
    "random",
    "custom",
    "breathe",
    "cycle",
    "wave",
    "dance",
    "tempo",
    "flash"
};


/* Implementation of the wmbb-method of the WMI device 
 * ------------------------------------------------------------------------------------------ */   

/* Method for sending the command to the wmbb method of the PNP0C14-interface; see dsdt for details */
static int wmi_evaluate_wmbb_method(u32 method_id, u32 arg, u32 *retval) {
    /* These structures are expected by the wmbb methods for input and output */
	union acpi_object obj;
	struct acpi_buffer in  = { (acpi_size) sizeof(arg), &arg };
	struct acpi_buffer out = { sizeof(obj), &obj };
	
	acpi_status status;

	u32 tmp;

    EUROCOM_DEBUG("called %0#4x with arg %0#6x\n", method_id, arg);	

	status = wmi_evaluate_method(EUROCOM_MX5R2_GUID, 0x00, method_id, &in, &out);

	if (unlikely(ACPI_FAILURE(status))) {
		EUROCOM_ERROR("called %0#4x with arg %0#6x\n", method_id, arg);
		EUROCOM_ERROR("ACPI_FAILURE in wmi_evaluate_method. Returned: 0x%04X\n", status);
		goto exit;
	}

	if (obj.type == ACPI_TYPE_INTEGER)
			tmp = (u32) obj.integer.value;
	else
			tmp = 0;

    EUROCOM_DEBUG("called %0#4x with arg %0#6x. Returned: %0#6x\n", method_id, arg, tmp);

	if (likely(retval))
			*retval = tmp;

exit:
	if (unlikely(ACPI_FAILURE(status)))
		return -EIO;

	return 0;
}

/* Setting and getting the color of the keyboard 
 * ------------------------------------------------------------------------------------------ */
/* Global values to keep track of the values; initialize with default value (blue for all regions) */
struct {
    unsigned int left;
    unsigned int center;
    unsigned int right;
} keyboard_colors = { 0x0000FF, 0x0000FF, 0x0000FF };


/* Method for setting the color of a specific region of the keyboard by wmi */
static void set_region_color(unsigned char keyboard_region, unsigned char red, unsigned char green, unsigned char blue) {
    /* Prepare cmd for setting the color */
    unsigned int cmd = 0xF0000000;   

    /* Not a valid region? Do nothing */
    if (keyboard_region >= KB_REGION_UNKNOWN)
        return;

    /* Include region and colors */
    cmd |= keyboard_region << 24;
    cmd |= blue << 16;
    cmd |= red << 8;
    cmd |= green << 0;

    /* Save the color to global variable */
    switch (keyboard_region) {
        case KB_REGION_LEFT:
            keyboard_colors.left = red << 16 | green << 8 | blue << 0;
            break;
        case KB_REGION_CENTER:
            keyboard_colors.center = red << 16 | green << 8 | blue << 0;
            break;
        case KB_REGION_RIGHT:
            keyboard_colors.right = red << 16 | green << 8 | blue << 0;
            break;
        default:
            EUROCOM_DEBUG("Region not recognized: %d", keyboard_region);
            break;
    };

    /* Whenever the color is set, the mode will automatically be set to custom mode, so save this state here */
    keyboard_mode = KB_MODE_CUSTOM;
    
    /* Send to keyboard */
    wmi_evaluate_wmbb_method(SET_KB_LED, cmd, NULL);
};

/* Method for resetting the colors of the keyboards (useful when switching from a non-custom mode back to custom mode) */
static void reset_custom_colors(void) {
    set_region_color(KB_REGION_LEFT,   (keyboard_colors.left >> 16), (keyboard_colors.left >> 8) & 0xFF, keyboard_colors.left & 0xFF);
    set_region_color(KB_REGION_CENTER, (keyboard_colors.center >> 16), (keyboard_colors.center >> 8) & 0xFF, keyboard_colors.center & 0xFF);
    set_region_color(KB_REGION_RIGHT,  (keyboard_colors.right >> 16), (keyboard_colors.right >> 8) & 0xFF, keyboard_colors.right & 0xFF);
}

static ssize_t get_region_color_left(struct device *child, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d %d %d\n", (keyboard_colors.left >> 16), (keyboard_colors.left >> 8) & 0xFF, keyboard_colors.left & 0xFF);
}

static ssize_t set_region_color_left(struct device *child, struct device_attribute *attr, const char *buf, size_t size) {
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int i = 0;

    i = sscanf(buf, "%d %d %d", &red, &green, &blue);

    if ( i == 3 ) {
        if (keyboard_mode != KB_MODE_CUSTOM) reset_custom_colors();
        set_region_color(KB_REGION_LEFT, (unsigned char)red, (unsigned char)green, (unsigned char)blue);
    }
    
    return size;        
}

static ssize_t get_region_color_center(struct device *child, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d %d %d\n", (keyboard_colors.center >> 16), (keyboard_colors.center >> 8) & 0xFF, keyboard_colors.center & 0xFF);
}

static ssize_t set_region_color_center(struct device *child, struct device_attribute *attr, const char *buf, size_t size) {
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int i = 0;

    i = sscanf(buf, "%d %d %d", &red, &green, &blue);

    if ( i == 3 ) {
        if (keyboard_mode != KB_MODE_CUSTOM) reset_custom_colors();
        set_region_color(KB_REGION_CENTER, (unsigned char)red, (unsigned char)green, (unsigned char)blue);
    }
    
    return size;        
}

static ssize_t get_region_color_right(struct device *child, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d %d %d\n", (keyboard_colors.right >> 16), (keyboard_colors.right >> 8) & 0xFF, keyboard_colors.right & 0xFF);
}

static ssize_t set_region_color_right(struct device *child, struct device_attribute *attr, const char *buf, size_t size) {
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int i = 0;

    i = sscanf(buf, "%d %d %d", &red, &green, &blue);

    if ( i == 3 ) {
        if (keyboard_mode != KB_MODE_CUSTOM) reset_custom_colors();
        set_region_color(KB_REGION_RIGHT, (unsigned char)red, (unsigned char)green, (unsigned char)blue);
    }
    
    return size;        
}

DEVICE_ATTR(left, 0644, get_region_color_left, set_region_color_left);
DEVICE_ATTR(center, 0644, get_region_color_center, set_region_color_center);
DEVICE_ATTR(right, 0644, get_region_color_right, set_region_color_right);


/* Setting and getting the brightness of the keyboard 
 * ------------------------------------------------------------------------------------------ */
#define KB_BRIGHTNESS_MAX     255
#define KB_BRIGHTNESS_DEFAULT KB_BRIGHTNESS_MAX

unsigned char keyboard_brightness = KB_BRIGHTNESS_DEFAULT;

static void set_keyboard_brightness(unsigned char brightness) {
    /* Limit the value: 0-255 */
    brightness = clamp_t(unsigned int, brightness, 0, 255);

    /* Save the value */
    keyboard_brightness = brightness;

    /* Tell the keyboard */
    wmi_evaluate_wmbb_method(SET_KB_LED, 0xF4000000 | brightness, NULL);
}

static ssize_t get_brightness(struct device *child, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", keyboard_brightness);
}

static ssize_t set_brightness(struct device *child, struct device_attribute *attr, const char *buf, size_t size) {
    unsigned int brightness = KB_BRIGHTNESS_DEFAULT;
    unsigned int i = 0;

    i = sscanf(buf, "%d", &brightness);

    if ( i == 1 )
        set_keyboard_brightness((unsigned char)brightness);
    
    return size;        
}

DEVICE_ATTR(brightness, 0644, get_brightness, set_brightness);


/* Setting and getting the mode of the keyboard 
 * ------------------------------------------------------------------------------------------ */

static void set_keyboard_mode(unsigned char mode) {
    /* these are the actual commands for the modes */
    static u32 cmds[] = {
		[KB_MODE_BREATHE]      = 0x1002a000,
		[KB_MODE_CUSTOM]       = 0,
		[KB_MODE_CYCLE]        = 0x33010000,
		[KB_MODE_DANCE]        = 0x80000000,
		[KB_MODE_FLASH]        = 0xA0000000,
		[KB_MODE_RANDOM_COLOR] = 0x70000000,
		[KB_MODE_TEMPO]        = 0x90000000,
		[KB_MODE_WAVE]         = 0xB0000000,
	};

	/* Out of range? do nothing */
    if (mode >= NUMBER_OF_KEYBOARD_MODES)
        return;

    /* Reset the keyboard modes */
	wmi_evaluate_wmbb_method(SET_KB_LED, 0x10000000, NULL);

    /* Custom mode means to set color and brightness back to saved levels */
	if (mode == KB_MODE_CUSTOM) {    
        reset_custom_colors();
        set_keyboard_brightness(keyboard_brightness);
	    	
        /* Done */
		return;
	}

    /* Save state */
    keyboard_mode = mode;

    /* Send to keyboard */
	wmi_evaluate_wmbb_method(SET_KB_LED, cmds[mode], NULL);    
}

static ssize_t get_mode_integer(struct device *child, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%d\n", keyboard_mode);
}

static ssize_t set_mode_integer(struct device *child, struct device_attribute *attr, const char *buf, size_t size) {
    unsigned int mode = KB_MODE_CUSTOM;
    unsigned int i = 0;

    i = sscanf(buf, "%d", &mode);

    if ( i == 1 )
        set_keyboard_mode((unsigned char)mode);
    
    return size;        
}

static ssize_t get_mode_desc(struct device *child, struct device_attribute *attr, char *buf) {
    /* Out of range? return 0 */
    if (keyboard_mode >= NUMBER_OF_KEYBOARD_MODES)
        return 0;

    /* Return name of keyboard mode */
    return sprintf(buf, "%s\n", keyboard_mode_description[keyboard_mode]);
}

static ssize_t set_mode_desc(struct device *child, struct device_attribute *attr, const char *buf, size_t size) {
    unsigned int m = 0;

    /* at least one byte in the buffer, i.e. size > 1 */
    if (size < 2)
        return 0;

    /* look if the buf containes any of the words */
    for( m = 0; m < NUMBER_OF_KEYBOARD_MODES; m++ ) {        
        if (strncmp(keyboard_mode_description[m], buf, (size-1)) == 0) {
            set_keyboard_mode((unsigned char)m);
            break;
        }
    }
    
    return size;               
}

DEVICE_ATTR(mode, 0644, get_mode_integer, set_mode_integer);
DEVICE_ATTR(modedesc, 0644, get_mode_desc, set_mode_desc);


/* Init and exit function of this module 
 * ------------------------------------------------------------------------------------------ */

/* This is the entry function of the module; let's check if the wmi-GUID is available; it does 
 * not check if the machine or platform is the correct one - user responsibility atm */
static int __init mx5r2_wmi_init(void) {
    int noattributes = 0;

    /* Check for GUID */
    EUROCOM_INFO("Checking for GUID %s: ", EUROCOM_MX5R2_GUID);

    if (!wmi_has_guid(EUROCOM_MX5R2_GUID)) {
	    EUROCOM_ERROR("Not found.\n");
	    return -ENODEV;
	}

    EUROCOM_INFO("OK\n");

    /* Registering device */
    EUROCOM_INFO("Registering platform driver to /sys/devices/platform/%s...", EUROCOM_MX5R2_DRIVER_NAME);

    mx5r2_led_device = platform_create_bundle(&mx5r2_led_driver, NULL, NULL, 0, NULL, 0);
    if (unlikely(IS_ERR(mx5r2_led_device))) {
        EUROCOM_ERROR("Did not work. Error code: %ld.\n", PTR_ERR(mx5r2_led_device));
        return PTR_ERR(mx5r2_led_device);
    }

    EUROCOM_INFO("OK\n");   

    /* Registering attributes for the driver */
    EUROCOM_INFO("Registering attributes...");

    if (device_create_file(&mx5r2_led_device->dev,	&dev_attr_left) != 0) {
		EUROCOM_ERROR("Sysfs attribute creation failed for 'left'\n");    
    } else {
        noattributes++;
    }

    if (device_create_file(&mx5r2_led_device->dev,	&dev_attr_center) != 0) {
		EUROCOM_ERROR("Sysfs attribute creation failed for 'center'\n");    
    } else {
        noattributes++;
    }

    if (device_create_file(&mx5r2_led_device->dev,	&dev_attr_right) != 0) {
		EUROCOM_ERROR("Sysfs attribute creation failed for 'right'\n");    
    } else {
        noattributes++;
    }

    if (device_create_file(&mx5r2_led_device->dev,	&dev_attr_brightness) != 0) {
		EUROCOM_ERROR("Sysfs attribute creation failed for 'brightness'\n");    
    } else {
        noattributes++;
    }

    if (device_create_file(&mx5r2_led_device->dev,	&dev_attr_mode) != 0) {
		EUROCOM_ERROR("Sysfs attribute creation failed for 'mode'\n");    
    } else {
        noattributes++;
    }

    if (device_create_file(&mx5r2_led_device->dev,	&dev_attr_modedesc) != 0) {
		EUROCOM_ERROR("Sysfs attribute creation failed for 'modedesc'\n");    
    } else {
        noattributes++;
    }

    EUROCOM_INFO("Succesfully registered %d of 6 attributes.\n", noattributes);

    /* Set some standard colors */
    set_region_color(KB_REGION_LEFT,    255,   0,   0);
    set_region_color(KB_REGION_CENTER,    0, 255,   0);
    set_region_color(KB_REGION_RIGHT,     0,   0, 255);

    /* Set default brightness */
    set_keyboard_brightness(255);

    /* Success */
	return 0;
}

/* This is the exit function of the module; unload all sysfs components */
static void __exit mx5r2_wmi_exit(void) {
    /* Unload all device attributes */
    device_remove_file(&mx5r2_led_device->dev, &dev_attr_left);
    device_remove_file(&mx5r2_led_device->dev, &dev_attr_center);
    device_remove_file(&mx5r2_led_device->dev, &dev_attr_right);
    device_remove_file(&mx5r2_led_device->dev, &dev_attr_brightness);
    device_remove_file(&mx5r2_led_device->dev, &dev_attr_mode);
    device_remove_file(&mx5r2_led_device->dev, &dev_attr_modedesc);

    /* Unregister platform and device */
    platform_device_unregister(mx5r2_led_device);
	platform_driver_unregister(&mx5r2_led_driver);

    EUROCOM_INFO("Goodbye!\n");
    return;
}

/* Module settings */
module_init(mx5r2_wmi_init);
module_exit(mx5r2_wmi_exit);

/* Module info */
MODULE_AUTHOR("Sven Kochmann");
MODULE_DESCRIPTION("Eurocom MX5 R2 WMI driver for controlling the keyboard LEDs");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1.0");
MODULE_ALIAS("wmi:"EUROCOM_MX5R2_GUID);



