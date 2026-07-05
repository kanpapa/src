# DEC AXPvme 230 NetBSD/alpha Cheat Sheet

【注意】AXPvme230は発熱が多いため、電源投入中は必ずエアフローを当てること。

## Ubuntuでの操作

### start dnsmasq

```
sudo systemctl start dnsmasq
```

### start tcpdump

USB-C LAN Adapter
```
sudo tcpdump -i enx6084bd485c85 -n host 192.168.99.10
```

i210 LAN Card
```
sudo tcpdump -i enp5s0 -n host 192.168.99.10
```

### deploy kernel

```
sudo cp ~/obj/sys/arch/alpha/compile/AXPVME/netbsd /export/client/root/.
```

## AXPvme230での操作

### boot kernel

```
>>>boot ewa0
```

### ping

```
ping 192.168.99.10
```

### ssh

```
ssh root@192.168.99.10
```
