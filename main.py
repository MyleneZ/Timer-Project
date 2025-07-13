import board

# 4 timers maximum
timers = []

# ===== Timer Functions =====

def set_timer(name, duration):
    if len(timers) >= 4:
        # TODO: display that we can't have more timers
        return
    
    timer = board.Timer(name, duration)
    timers.append(timer)
    timer.start()

# == <= >= !=

def cancel_timer(name):
    global timers
    for timer in timers:
        if timer.name == name:
            timer.cancel()
            timers.remove(timer)
            return
            

def add_time(name, add):
    global timers
    for timer in timers:
        if timer.name == name:
            timer.add_time(add)
            return
        
def subtract_time(name, sub):
    global timers
    for timer in timers:
        if timer.name == name:
            timer.subtract_time(sub)
            return

# ===== Sound Functions =====

def stop_ringtone():
    # get the currently playing tone

    # Stop the current tone
    pass

def play_sound(name):
    pass

# ===== Display Functions =====