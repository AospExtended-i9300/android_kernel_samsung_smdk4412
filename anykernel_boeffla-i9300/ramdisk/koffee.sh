#!/system/xbin/bash
# Koffee's startup script
# running immediatelly after mounting /system
# do not edit!

# 1.
/sbin/busybox mount -o remount,rw /

# 2. Pyramid
/sbin/busybox echo "pyramid" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 3. SELinux contexts
/system/bin/restorecon -FRD /data/data

# 4. zRam
# Enable total 400 MB zRam on 1 device as default
/sbin/busybox echo "1" > /sys/block/zram0/reset
/sbin/busybox echo "209715200" > /sys/block/zram0/disksize
/sbin/busybox mkswap /dev/block/zram0
/sbin/busybox swapon /dev/block/zram0

/sbin/busybox echo "1" > /sys/block/zram1/reset
/sbin/busybox echo "lzo" > /sys/block/zram1/comp_algorithm
/sbin/busybox echo "419430400" > /sys/block/zram1/disksize
/sbin/busybox mkswap /dev/block/zram1
/sbin/busybox swapon /dev/block/zram1
/sbin/busybox echo "100" > /proc/sys/vm/swappiness

# 5. BFQ and deadline
/sbin/busybox echo "bfq" > /sys/block/mmcblk0/queue/scheduler
/sbin/busybox echo "deadline" > /sys/block/mmcblk1/queue/scheduler

# 6. Switch to fq_codel on mobile data and wlan
/system/bin/tc qdisc add dev rmnet0 root fq_codel
/system/bin/tc qdisc add dev wlan0 root fq_codel

# 7. Enable network security enhacements from O
/sbin/busybox echo 1 > /proc/sys/net/ipv4/conf/all/drop_unicast_in_l2_multicast
/sbin/busybox echo 1 > /proc/sys/net/ipv6/conf/all/drop_unicast_in_l2_multicast
/sbin/busybox echo 1 > /proc/sys/net/ipv4/conf/all/drop_gratuitous_arp
/sbin/busybox echo 1 > /proc/sys/net/ipv6/conf/all/drop_unsolicited_na

# 8. Tweak scheduler
/sbin/busybox echo 1 > /proc/sys/kernel/sched_child_runs_first

# 9. Fix Doze helper permissions
/sbin/busybox chmod 0755 /res/koffee/supolicy
/res/koffee/supolicy --live "allow kernel system_file file { execute_no_trans }"

# 10. Enlarge nr_requests for emmc
/sbin/busybox echo 2048 > /sys/block/mmcblk0/queue/nr_requests

# 11. Sdcard buffer tweaks
/sbin/busybox echo 2048 > /sys/block/mmcblk0/bdi/read_ahead_kb
/sbin/busybox 1024 > /sys/block/mmcblk1/bdi/read_ahead_kb

# 12. Strict request affinity for internal storage
/sbin/busybox echo 2 > /sys/block/mmcblk0/queue/rq_affinity

# Clean up and fire up SELinux
/sbin/busybox rm /koffee.sh
/sbin/busybox rm /res/koffee/supolicy
/sbin/busybox mount -o remount,ro /libs
/sbin/busybox mount -o remount,ro /
/system/bin/toybox setenforce 1
