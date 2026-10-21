PSP2SHELL
==========

**psp2shell is an utility/remote shell (plugin+client, wifi) to ease the development process on ps vita devices.** You should be able to print debug output over the shell (sceClibPrintf), send files, send/restart your application at anytime saving some precious time for the real stuff.

-----

**Building the plugins**
```
mkdir cmake-build && cd cmake-build
cmake -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release ..
make
```

**Building the client (linux/deb)**
```
sudo apt-get install libreadline-dev
cd psp2shell_cli && mkdir cmake-build && cd cmake-build
cmake ..
make
```

**Installation**
>- copy "psp2shell_k.skprx" and "psp2shell_m.suprx" to "ux0:/tai/"
>- add "ux0:/tai/psp2shell_k.skprx" to "*KERNEL" section of "ux0:/tai/config.txt"
>- add "ux0:/tai/psp2shell_m.suprx" to "*main" section of "ux0:/tai/config.txt"
>- reboot

Tipcal usage
--------

**Enable debug print in your application/module**

Use "**sceClibPrintf**" in your application/module to print debug messages to the shell.

**Get a shell, get messages**

Connect to your ps vita device from your computer with the client (replace "ip" with your ps vita ip address) :
```
./psp2shell_cli ip 3333
```
From there, you'll be able to see your sceClibPrintf messages coming in.

**Log stream (port 3335)**

Kernel and user debug output (`ksceDebugPrintf`, `sceClibPrintf`, stdout) is collected in a 64 KiB
ring buffer in the kernel module, from boot on, whether or not a client is connected. Printing never
waits for the network: when the buffer is full the oldest output is dropped and readers get a
`[psp2shell: N bytes of log dropped]` note. Every line is prefixed with `[seconds.micros pid]`
(time since boot, process id of the printer).

The buffer is streamed as plain text on the shell port + 2 (3335), to up to 4 readers at once,
independently of the shell session:
```
nc ip 3335
```
`psp2shell_cli` connects to it automatically; set `PSP2SHELL_NO_LOG=1` to leave it to another reader.
When no log reader is connected, logs are sent through the shell connection as before.

**Update, restart your application**

Re-build your application then, from the shell, you can send the newly complied version of your application, and restart it automatically :
```
reload /path/to/my/app/build/eboot.bin
```
Note that if your application is not running (or crashed), you'll have to specify the title id (9 chars) of your application (so we know where to put the new eboot.bin) :
```
load TITLEID01 /path/to/my/app/build/eboot.bin
```

**A little more**

There are a few more commands available, like "ls", "rm", "send", "modlist", "modinfo"... You can type "help" in the shell for a list of available commands.


Thanks
----------
consolex-bordeaux.fr, ScHlAuChi, noname120, motoharu, xyz, yifanlu, frangarcj, Rinnegatamante, davee and people i may forget for what they do for the ps vita.
