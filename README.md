# eurocom-mx5r2-keyleds

## Description
Kernel module for keyboard backlighting of Eurocom MX5-R2 notebooks. Might work with other models, too. In
contrast to previous implementations (clevo-xsm-wmi and tuxedo-wmi) it does not implement anything else. Do 
one thing, and only one thing.

The kernel module does not check for the right platform atm, so please make sure that your system is compatible. 

## Compatible models
The following model(s) have been tested.
 * Eurocom SKY MX5 R2 (Bios: 1.05.05)

## Attributes
When loaded the interface is available at /sys/devices/platform/mx5kbleds with various attributes, which all provide
read and write access.

To set the colour of a region, just forward a RGB code to *left*, *center*, or *right*. Please note that setting the
colour will also set the *mode* to *custom* (see below):

```bash
# echo "255 0 0" > center
```

To set the brightness of the LEDs, set *brightness* (range: 0-255; 0 turns LEDs off):

```bash
# echo 125 > brightness
```

The keyboard LEDs supports 8 modes:
0. random
1. custom
2. breathe
3. cycle
4. wave
5. dance
6. tempo
7. flash

All modes except *custom* will automatically and continuously change the brightness and colour of the LEDs. Please note that
the above attributes only reflect the *brightness* and *colours* for the *custom*-mode. Therefore, setting the mode to 
*custom* will restore the *brightness* and *colours* set before.

To set a mode, either provide the number from the list to *mode* or the name of the mode to *modedesc*. The following two lines
result both in setting the mode to *wave*:

```bash
# echo 4 > mode
# echo wave > modedesc
```

## Building
The module can be simply compiled with *make*. There is also an *install*-script, which will copy the module to the respective kernel
directory:

```bash
# cd module
# make
# ./install
```

I advice to test the module first by manually loading it with *modprobe*. If everything is working and there are no negative side
effects you can autoload the module with *insmod*.

```bash
# modprobe eurocom-mx5r2-keyleds
# cd /sys/devices/platform/mx5kbleds/
```





