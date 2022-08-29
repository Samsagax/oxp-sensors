# Platform driver for OneXPlayer boards

This driver provides functinoality to control the fan in the OneXPlayer mini
AMD variant. Intel boards are not yet supported until I get the correct EC
registers to read/write to.

## Build
If you only want to build and test the module:

```
$ git clone https://gitlab.com/Samsagax/oxp-platform-dkms.git
$ cd oxp-platform-dkms
$ make
```

Then insert the module and check `sensors` and `dmesg` if appropriate.

## Install

You'll need appropriate headers for your kernel and `dkms` package from your
distribution.

```
$ git clone https://gitlab.com/Samsagax/oxp-platform-dkms.git
$ cd oxp-platform-dkms
$ make
$ sudo make dkms
```

## Usage

Insert the module with `insmod`.

`sensors` will show the fan RPM as read from the EC. To control it, look for
a `hwmon` device with name `oxpec`, i.e.:
`$ cat /sys/class/hwmon/hwmon?/name`

To enable manual control of the fan (assuming `hwmon5` is ours):

`# echo 1 > /sys/class/hwmon/hwmon5/pwm1_enable`

Then input values in the range `[0-100]` to the pwm:

`# echo 100 > /sys/class/hwmon/hwmon5/pwm1`

