/* Copyright (C) 2010 Antonio Galea
 *
 * May be copied or modified under the terms of the GNU General Public
 * License. See linux/COPYING for more information.
 *
 * Generic software-only driver for generating PWM signals via high
 * resolution timers and GPIO lib interface.
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/gpio.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antonio Galea");
MODULE_DESCRIPTION("Driver for kernel-generated PWM signals");

#ifndef ARCH_NR_GPIOS
#define ARCH_NR_GPIOS 565
#endif

static struct hrtimer hr_timer;

/* pwm_desc
 *
 * This structure maintains the information regarding a
 * single PWM signal: its wave period and pulse length
 * are user-definable via sysfs. The counter is also
 * shown in sysfs as a debug helper.
*/
struct pwm_desc {
  unsigned int pulse;    // pulse width, in microseconds
  unsigned int period;   // wave period, in microseconds 
  unsigned int pulses;   // number of pwm pulses before stopping; -1 never stops, 0 stops immediately
  unsigned long counter; // "interrupt" counter - counts each value toggle
  int value;             // current GPIO pin value (0 or 1 only)
  ktime_t next_tick;     // timer tick at which next toggling should happen
  unsigned long flags;   // only FLAG_SOFTPWM is used, for synchronizing inside module
#define FLAG_SOFTPWM 0
};

/* pwm_table
 *
 * The table will hold a description for any GPIO pin available
 * on the system. It's wasteful to preallocate the entire table,
 * but avoiding race conditions is so much easier this way ;-)
*/
static struct pwm_desc pwm_table[ARCH_NR_GPIOS];

/* lock protects against pwm_unexport() being called while
 * sysfs files are active.
 */
static DEFINE_MUTEX(sysfs_lock);

int pwm_export(unsigned gpio);   // forward definition
int pwm_unexport(unsigned gpio); // forward definition

/* Show attribute values for PWMs */
static ssize_t pwm_show(struct device *dev, struct device_attribute *attr, char *buf){
  const struct pwm_desc *desc = dev_get_drvdata(dev);
  ssize_t status;
  mutex_lock(&sysfs_lock);
  if(!test_bit(FLAG_SOFTPWM, &desc->flags)){
    status = -EIO;
  }else{
    if(strcmp(attr->attr.name, "pulse")==0){
      status = sprintf(buf, "%d usec\n", desc->pulse);
    }else if(strcmp(attr->attr.name, "period")==0){
      status = sprintf(buf, "%d usec\n", desc->period);
    }else if(strcmp(attr->attr.name, "pulses")==0){
      status = sprintf(buf, "%d usec\n", desc->pulses);
    }else if(strcmp(attr->attr.name, "counter")==0){
      status = sprintf(buf, "%lu\n", desc->counter);
    }else{
      status = -EIO;
    }
  }
  mutex_unlock(&sysfs_lock);
  return status;
}

ktime_t tick( void ) {
  unsigned gpio;
  struct pwm_desc *desc;
  ktime_t now = ktime_get();
  ktime_t next_tick = ktime_set(0,0);

  for(gpio=0;gpio<ARCH_NR_GPIOS;gpio++){
    desc = &pwm_table[gpio];
    if(
      test_bit(FLAG_SOFTPWM,&desc->flags) &&
      (desc->period>0) &&
      (desc->pulse>0) &&
      (desc->pulse<=desc->period) &&
      (desc->pulses!=0)
    ){
      if(desc->next_tick<=now){
        desc->value = 1-desc->value;
        gpio_set_value(gpio,desc->value);
        desc->counter++;
        if(desc->pulses>0){ desc->pulses--; }
        if((desc->pulse==0)||(desc->pulse==desc->period)||(desc->pulses==0)){
          desc->next_tick = KTIME_MAX;
        }else{
          desc->next_tick=ktime_add_ns(
            desc->next_tick,
            (desc->value? desc->pulse : desc->period-desc->pulse)*1000
          );
        }
      }

      if((next_tick==0)||(desc->next_tick<next_tick && desc->next_tick < KTIME_MAX)){
        next_tick = desc->next_tick;
      }
    }
  }

  return next_tick;
}

/* Store attribute values for PWMs */
static ssize_t pwm_store(
  struct device *dev, struct device_attribute *attr, const char *buf, size_t size
){
  struct pwm_desc *desc = dev_get_drvdata(dev);
  ssize_t status;
  ktime_t now;

  mutex_lock(&sysfs_lock);
  if(!test_bit(FLAG_SOFTPWM, &desc->flags)){
    status = -EIO;
  }else{
    unsigned long value;
    status = kstrtoul(buf, 0, &value);

    hrtimer_cancel(&hr_timer);

    if(status==0){
      if(strcmp(attr->attr.name, "pulse")==0){
        if(value<=desc->period){ desc->pulse = (unsigned int)value; }
      }else if(strcmp(attr->attr.name, "period")==0){
        desc->period = (unsigned int)value;
      }else if(strcmp(attr->attr.name, "pulses")==0){
        desc->pulses = (unsigned int)value;
      }

      now = ktime_get();

      if (desc->next_tick < now || desc->next_tick == KTIME_MAX) {
        desc->next_tick = now;
      }

      hrtimer_start(&hr_timer, ktime_set(0,1), HRTIMER_MODE_REL);
    }
  }
  mutex_unlock(&sysfs_lock);
  return status ? : size;
}

/* Sysfs attributes definition for PWMs */
static DEVICE_ATTR(pulse,   0644, pwm_show, pwm_store);
static DEVICE_ATTR(period,  0644, pwm_show, pwm_store);
static DEVICE_ATTR(pulses,  0644, pwm_show, pwm_store);
static DEVICE_ATTR(counter, 0444, pwm_show, NULL);
static const struct attribute *soft_pwm_dev_attrs[] = {
  &dev_attr_pulse.attr,
  &dev_attr_period.attr,
  &dev_attr_pulses.attr,
  &dev_attr_counter.attr,
  NULL,
};
static const struct attribute_group soft_pwm_dev_attr_group = {
  .attrs = (struct attribute **) soft_pwm_dev_attrs,
};

/* Export a GPIO pin to sysfs, and claim it for PWM usage.
 * See the equivalent function in drivers/gpio/gpiolib.c
 */
static ssize_t export_store(const struct class *class,
                            const struct class_attribute *attr,
                            const char *buf,
                            size_t len) {
  long gpio;
  int status;

  status = kstrtoul(buf, 0, &gpio);
  if(status<0){ goto done; }

  status = gpio_request(gpio, "soft_pwm");
  if(status<0){ goto done; }

  status = gpio_direction_output(gpio,0);
  if(status<0){ goto done; }
  
  status = pwm_export(gpio);
  if(status<0){ goto done; }

  set_bit(FLAG_SOFTPWM, &pwm_table[gpio].flags);

done:
  if(status){
    gpio_free(gpio);
    pr_debug("%s: status %d\n", __func__, status);
  }
  return status ? : len;
}

/* Unexport a PWM GPIO pin from sysfs, and unreclaim it.
 * See the equivalent function in drivers/gpio/gpiolib.c
 */
static ssize_t unexport_store(const struct class *class,
                              const struct class_attribute *attr,
                              const char *buf,
                              size_t len){
  long gpio;
  int  status;

  status = kstrtoul(buf, 0, &gpio);
  if(status<0){ goto done; }

  status = -EINVAL;
  if(!gpio_is_valid(gpio)){ goto done; }

  if(test_and_clear_bit(FLAG_SOFTPWM, &pwm_table[gpio].flags)){
    status = pwm_unexport(gpio);
    if(status==0){ gpio_free(gpio); }
  }
done:
  if(status){ pr_debug("%s: status %d\n", __func__, status); }
  return status ? : len;
}

/* Sysfs definitions for soft_pwm class */
static CLASS_ATTR_WO(export);
static CLASS_ATTR_WO(unexport);

static struct attribute *soft_pwm_class_attrs[] = {
	&class_attr_export.attr,
	&class_attr_unexport.attr,
	NULL,
};

ATTRIBUTE_GROUPS(soft_pwm_class);

static struct class soft_pwm_class = {
  .name =        "soft_pwm",
  .class_groups = soft_pwm_class_groups,
};

/* Setup the sysfs directory for a claimed PWM device */
int pwm_export(unsigned gpio){
  struct pwm_desc *desc;
  struct device   *dev;
  int             status;

  mutex_lock(&sysfs_lock);

  desc = &pwm_table[gpio];
  desc->value  = 0;
  desc->pulses = -1;
  desc->next_tick = 0;
  dev = device_create(&soft_pwm_class, NULL, MKDEV(0, 0), desc, "pwm%d", gpio);
  if(dev){
    status = sysfs_create_group(&dev->kobj, &soft_pwm_dev_attr_group);
    if(status==0){
      printk(KERN_INFO "Registered device pwm%d\n", gpio);
    }else{
      device_unregister(dev);
    }
  }else{
    status = -ENODEV;
  }

  mutex_unlock(&sysfs_lock);

  if(status){ pr_debug("%s: pwm%d status %d\n", __func__, gpio, status); }
  return status;
}

/* Used by pwm_unexport below to find the device which should be freed */
static int match_export(struct device *dev, const void *data){
  return dev_get_drvdata(dev) == data;
}

/* Free a claimed PWM device and unregister the sysfs directory */
int pwm_unexport(unsigned gpio){
  struct pwm_desc *desc;
  struct device   *dev;
  int             status;
      
  mutex_lock(&sysfs_lock);

  desc = &pwm_table[gpio];
  dev  = class_find_device(&soft_pwm_class, NULL, desc, match_export);
  if(dev){
    put_device(dev);
    device_unregister(dev);
    printk(KERN_INFO "Unregistered device pwm%d\n", gpio);
    status = 0;
  }else{
    status = -ENODEV;
  }

  mutex_unlock(&sysfs_lock);

  if(status){ pr_debug("%s: pwm%d status %d\n", __func__, gpio, status); }
  return status;
}

/* The timer callback is called only when needed (which is to
 * say, at the earliest PWM signal toggling time) in order to
 * maintain the pressure on system latency as low as possible
 */
enum hrtimer_restart soft_pwm_hrtimer_callback(struct hrtimer *timer) {
  ktime_t next_tick = tick();
  
  if(next_tick > 0){
    hrtimer_start(&hr_timer, next_tick, HRTIMER_MODE_ABS);
  }else{
    printk(KERN_INFO "Stopping timer.\n");
  }
  return HRTIMER_NORESTART;
}

/* module initialization: init the hr-timer and register a driver class */
static int __init soft_pwm_init(void){
  int status;
  printk(KERN_INFO "SoftPWM v0.3 initializing.\n");

  hrtimer_init(&hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  hr_timer.function = &soft_pwm_hrtimer_callback;

  status = class_register(&soft_pwm_class);
  if(status<0){ goto fail_no_class; }

  printk(KERN_INFO "SoftPWM initialized.\n");
  return 0;

fail_no_class:
  return status;
}

/* module finalization: cancel the hr-timer, switch off any PWM
 * signal and give back to GPIO the pin, then deregister our class
 */
static void __exit soft_pwm_exit(void){
  unsigned gpio;
  int status;

  hrtimer_cancel(&hr_timer);
  for(gpio=0;gpio<ARCH_NR_GPIOS;gpio++){
    struct pwm_desc *desc;
    desc = &pwm_table[gpio];
    if(test_bit(FLAG_SOFTPWM,&desc->flags)){
      gpio_set_value(gpio,0);
      status = pwm_unexport(gpio);
      if(status==0){ gpio_free(gpio); }
    }
  }
  class_unregister(&soft_pwm_class);
  printk(KERN_INFO "SoftPWM disabled.\n");
}

module_init(soft_pwm_init);
module_exit(soft_pwm_exit);
