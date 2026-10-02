#!/usr/bin/env python3

### zerowm: microscopic window manager for test harness
### makes all windows full-screen (expectation being - there's only one window)

from Xlib import X, display

d = display.Display()
root = d.screen().root

# tell X11 we are the window manager
root.change_attributes(event_mask=X.SubstructureRedirectMask)

w, h = d.screen().width_in_pixels, d.screen().height_in_pixels

while True:
    ev = d.next_event()
    if ev.type in (X.MapRequest, X.ConfigureRequest):
        win = ev.window
        # Force the window to be exactly the size of the screen
        win.configure(x=0, y=0, width=w, height=h, border_width=0)
        
        if ev.type == X.MapRequest:
            win.map()
            win.set_input_focus(X.RevertToParent, X.CurrentTime)
            
        d.sync()
