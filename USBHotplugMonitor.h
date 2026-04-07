/*---------------------------------------------------------*\
| USBHotplugMonitor.h                                       |
|                                                           |
|   Monitors USB hotplug events via libusb and triggers     |
|   targeted device detection/removal in ResourceManager    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <libusb.h>

class ResourceManager;

/*---------------------------------------------------------*\
| Hotplug event structure                                   |
\*---------------------------------------------------------*/
enum class HotplugEventType
{
    Arrived,
    Left
};

struct HotplugEvent
{
    HotplugEventType                        type;
    uint16_t                                vid;
    uint16_t                                pid;
    std::chrono::steady_clock::time_point   timestamp;
};

class USBHotplugMonitor
{
public:
    USBHotplugMonitor(ResourceManager* resource_manager);
    ~USBHotplugMonitor();

    void Start();
    void Stop();
    bool IsSupported() const;
    bool IsRunning() const;

private:
    void LibusbEventThreadFunction();
    void DispatchThreadFunction();

    static int LIBUSB_CALL OnHotplugEvent(
        libusb_context*         ctx,
        libusb_device*          dev,
        libusb_hotplug_event    event,
        void*                   user_data);

    ResourceManager*                    resource_manager;
    libusb_context*                     usb_context;
    libusb_hotplug_callback_handle      cb_handle;

    std::thread*                        libusb_event_thread;
    std::thread*                        dispatch_thread;
    std::atomic<bool>                   running;
    bool                                supported;

    std::mutex                          event_queue_mutex;
    std::condition_variable             event_queue_cv;
    std::deque<HotplugEvent>            event_queue;

    static constexpr int DEBOUNCE_MS = 500;
};
