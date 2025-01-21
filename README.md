# soft_pwm

This is a generic software-only Linux kernel driver for generating PWM (Pulse Width Modulation) signals using high-resolution timers and the GPIO interface. It was originally written by [Antonio Galea](https://github.com/antoniogalea).


## Overview
The **soft_pwm** module uses high-resolution timers within the Linux kernel to toggle GPIO pins at precise intervals. This provides a software-implemented PWM without needing dedicated PWM hardware. Each GPIO pin can be treated as a separate PWM output after being exported to this driver.

## Features
- **Sysfs interface** for setting:
  - `pulse` (the duration of the "high" part of the cycle, in microseconds),
  - `period` (the total duration of one cycle, in microseconds),
  - `pulses` (the number of cycles to output; `-1` means continue indefinitely),
  - `counter` (a read-only attribute counting toggles).
- **High-resolution timers** for accurate toggling.
- **Multiple GPIOs** can be exported simultaneously, each functioning independently.

### Steps to Compile
1. Ensure you have kernel headers installed (e.g., `sudo apt-get install linux-headers-$(uname -r)` on Debian/Ubuntu).
2. In the directory containing `soft_pwm.c` and the `Makefile`, run:
   
       make

   This will produce a file called `soft_pwm.ko`, which is the compiled kernel module.

### Steps to Install
Insert the module into the running kernel:

    sudo insmod soft_pwm.ko

### Verify Installation
Check the system log for confirmation:

    dmesg | tail

You should see messages similar to:

    SoftPWM v0.3 initializing.
    SoftPWM initialized.

---

## Usage

### Exporting a GPIO for PWM
After loading the module, a new directory `/sys/class/soft_pwm/` is created. To export a GPIO pin (e.g., GPIO 18), run:

    echo 18 > /sys/class/soft_pwm/export

This creates `/sys/class/soft_pwm/pwm18/` with the following attributes:
- `pulse`   (RW)
- `period`  (RW)
- `pulses`  (RW)
- `counter` (RO)

### Setting PWM Attributes
1. **Set the total period** (in microseconds):

       echo 20000 > /sys/class/soft_pwm/pwm18/period

   This configures a 20,000µs (20ms) total cycle time.

2. **Set the pulse width** (in microseconds):

       echo 1500 > /sys/class/soft_pwm/pwm18/pulse

   This keeps the GPIO pin high for 1,500µs in each cycle, then low for the remainder (18,500µs).

3. **Set the number of pulses** (optional):

       echo 100 > /sys/class/soft_pwm/pwm18/pulses

   The pin will toggle for 100 cycles, then stop.  
   If you set `-1`, it will continue toggling indefinitely:

       echo -1 > /sys/class/soft_pwm/pwm18/pulses

4. **Read the toggle counter**:

       cat /sys/class/soft_pwm/pwm18/counter

   Shows how many toggles (rising or falling edges) have occurred so far.

### Stopping the PWM
To stop PWM on GPIO 18, you can:
- Set `pulses` to `0`:

      echo 0 > /sys/class/soft_pwm/pwm18/pulses

  or
- Unexport the GPIO:

      echo 18 > /sys/class/soft_pwm/unexport

Either method will stop PWM on that pin. Unexporting also frees the GPIO and removes `/sys/class/soft_pwm/pwm18/`.

<br>

Old documentation [on old FoxG20 site](https://web.archive.org/web/20111222171059/http://foxg20old.acmesystems.it/doku.php?id=contributes:antoniogalea:soft_pwm).

More example usage here: https://www.acmesystems.it/soft_pwm