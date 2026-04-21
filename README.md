# sukawm – an infinite canvas x11 wm from scratch
my config file: 
``` //autorun
autorun='picom; kitty feh --bg-fill ~/Downloads/Peak/darkmode.png; ~/.config/polybar/launch.sh; /usr/lib/xdg-desktop-portal-gtk & ; dunst; /usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1 &; '
//binds
bind Alt+T, exec='kitty';
bind Alt+Return, exec='kitty';
bind Alt+LMB, exec='MoveWindow';
bind Alt+RMB, exec='ResizeWindow';
bind Alt+Control+LMB, exec='MoveCanvas';
bind Alt+Q, exec='KillWindow';
bind Alt+A, exec='rofi -show drun';
bind Alt+V, exec='JumpToWin';
//xf86 binds
bind XF86AudioMute, exec='pactl set-sink-mute @DEFAULT_SINK@ toggle';
bind XF86AudioLowerVolume, exec='pactl set-sink-volume @DEFAULT_SINK@ -5%';
bind XF86AudioRaiseVolume, exec='pactl set-sink-volume @DEFAULT_SINK@ +5%';
bind XF86MonBrightnessDown, exec='brightnessctl set 5%-';
bind XF86MonBrightnessUp, exec='brightnessctl set +5%';
```
recommended dependencies :
  *  dunst
  *  polybar
  *  feh
  *  xdg-desktop-portal-gtk
  *  kitty
  *  picom
Polybar requieres my spesific config, but it can be edited:
``` [bar/example]
width = 100%
height = 32

override-redirect = true
wm-restack = generic
fixed-center = true
enable-ipc = true

background = #222222
foreground = #ffffff

font-0 = "JetBrainsMono Nerd Font:size=10;2"

modules-left   = pulseaudio
modules-center = date 
modules-right  = cpu 


; -------------------------
;       MODULES
; -------------------------

[module/title]
type = internal/xwindow
label = %title%
label-maxlen = 60
label-empty = "Desktop"

[module/pulseaudio]
type = internal/pulseaudio
interval = 2
format = <label>
label= %percentage%

[module/date]
type = internal/date
interval = 1
date = %H:%M:%S
label = %date%

[module/cpu]
type = internal/cpu
interval = 2
format = <label>
label = CPU %percentage%%

[module/memory]
type = internal/memory
interval = 2
format = <label>
label = RAM %percentage_used%%
```
picom and rofi can be styled how you like.
