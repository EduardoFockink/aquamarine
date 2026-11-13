#!/usr/bin/env python3
"""A fake X server that accepts connections and never answers, like Hyprland's lazy Xwayland socket while
Hyprland's main loop is blocked. Listens on the abstract socket @/tmp/.X11-unix/X99 for N seconds."""
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind("\0/tmp/.X11-unix/X99"); s.listen(16)
conns = []
fim = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 30)
s.settimeout(0.5)
while time.time() < fim:
    try:
        c, _ = s.accept(); conns.append(c)   # accept and keep silent
        print("x_mudo: connection accepted, not answering", flush=True)
    except socket.timeout:
        pass
