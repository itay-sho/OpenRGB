/*---------------------------------------------------------*\
| USBHotplugMonitor.cpp                                     |
|                                                           |
|   Monitors USB hotplug events via libusb and triggers     |
|   targeted device detection/removal in ResourceManager    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "USBHotplugMonitor.h"
#include "ResourceManager.h"
#include "LogManager.h"
#include <algorithm>
#include <unordered_map>

USBHotplugMonitor::USBHotplugMonitor(ResourceManager* resource_manager)
    : resource_manager(resource_manager)
    , usb_context(nullptr)
    , cb_handle(0)
    , libusb_event_thread(nullptr)
    , dispatch_thread(nullptr)
    , running(false)
    , supported(false)
{
    /*-----------------------------------------------------*\
    | Initialize a dedicated libusb context for hotplug      |
    \*-----------------------------------------------------*/
    int rc = libusb_init(&usb_context);
    if(rc != LIBUSB_SUCCESS)
    {
        LOG_WARNING("[USBHotplugMonitor] Failed to initialize libusb context: %s", libusb_strerror((enum libusb_error)rc));
        usb_context = nullptr;
        return;
    }

    supported = libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) != 0;
}

USBHotplugMonitor::~USBHotplugMonitor()
{
    Stop();

    if(usb_context)
    {
        libusb_exit(usb_context);
        usb_context = nullptr;
    }
}

bool USBHotplugMonitor::IsSupported() const
{
    return supported;
}

bool USBHotplugMonitor::IsRunning() const
{
    return running.load();
}

void USBHotplugMonitor::Start()
{
    if(running.load() || !supported || !usb_context)
    {
        return;
    }

    /*-----------------------------------------------------*\
    | Register a single callback for both arrive and leave   |
    \*-----------------------------------------------------*/
    int rc = libusb_hotplug_register_callback(
        usb_context,
        (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_NO_FLAGS,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        OnHotplugEvent,
        this,
        &cb_handle
    );

    if(rc != LIBUSB_SUCCESS)
    {
        LOG_WARNING("[USBHotplugMonitor] Failed to register hotplug callback: %s", libusb_strerror((enum libusb_error)rc));
        return;
    }

    running = true;

    libusb_event_thread = new std::thread(&USBHotplugMonitor::LibusbEventThreadFunction, this);
    dispatch_thread     = new std::thread(&USBHotplugMonitor::DispatchThreadFunction, this);

    LOG_INFO("[USBHotplugMonitor] Started monitoring USB hotplug events");
}

void USBHotplugMonitor::Stop()
{
    if(!running.load())
    {
        return;
    }

    running = false;

    /*-----------------------------------------------------*\
    | Deregister the hotplug callback                        |
    \*-----------------------------------------------------*/
    if(usb_context)
    {
        libusb_hotplug_deregister_callback(usb_context, cb_handle);
    }

    /*-----------------------------------------------------*\
    | Wake up the dispatch thread so it can exit             |
    \*-----------------------------------------------------*/
    event_queue_cv.notify_one();

    /*-----------------------------------------------------*\
    | Interrupt the libusb event loop                        |
    \*-----------------------------------------------------*/
    if(usb_context)
    {
        libusb_interrupt_event_handler(usb_context);
    }

    if(libusb_event_thread)
    {
        libusb_event_thread->join();
        delete libusb_event_thread;
        libusb_event_thread = nullptr;
    }

    if(dispatch_thread)
    {
        dispatch_thread->join();
        delete dispatch_thread;
        dispatch_thread = nullptr;
    }

    LOG_INFO("[USBHotplugMonitor] Stopped monitoring USB hotplug events");
}

int LIBUSB_CALL USBHotplugMonitor::OnHotplugEvent(
    libusb_context*         ctx,
    libusb_device*          dev,
    libusb_hotplug_event    event,
    void*                   user_data)
{
    (void)ctx;

    USBHotplugMonitor* monitor = static_cast<USBHotplugMonitor*>(user_data);

    struct libusb_device_descriptor desc;
    int rc = libusb_get_device_descriptor(dev, &desc);
    if(rc != LIBUSB_SUCCESS)
    {
        LOG_WARNING("[USBHotplugMonitor] Failed to get device descriptor: %s", libusb_strerror((enum libusb_error)rc));
        return 0;
    }

    HotplugEvent hp_event;
    hp_event.vid       = desc.idVendor;
    hp_event.pid       = desc.idProduct;
    hp_event.timestamp = std::chrono::steady_clock::now();

    if(event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
    {
        hp_event.type = HotplugEventType::Arrived;
        LOG_INFO("[USBHotplugMonitor] Device arrived: %04X:%04X", hp_event.vid, hp_event.pid);
    }
    else if(event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
    {
        hp_event.type = HotplugEventType::Left;
        LOG_INFO("[USBHotplugMonitor] Device left: %04X:%04X", hp_event.vid, hp_event.pid);
    }
    else
    {
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(monitor->event_queue_mutex);
        monitor->event_queue.push_back(hp_event);
    }
    monitor->event_queue_cv.notify_one();

    return 0;
}

void USBHotplugMonitor::LibusbEventThreadFunction()
{
    LOG_DEBUG("[USBHotplugMonitor] libusb event thread started");

    while(running.load())
    {
        /*-------------------------------------------------*\
        | Use a timeout so we periodically check the        |
        | running flag                                      |
        \*-------------------------------------------------*/
        struct timeval tv = { 1, 0 };
        int rc = libusb_handle_events_timeout(usb_context, &tv);

        if(rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_INTERRUPTED)
        {
            LOG_WARNING("[USBHotplugMonitor] libusb_handle_events error: %s", libusb_strerror((enum libusb_error)rc));
        }
    }

    LOG_DEBUG("[USBHotplugMonitor] libusb event thread stopped");
}

void USBHotplugMonitor::DispatchThreadFunction()
{
    LOG_DEBUG("[USBHotplugMonitor] Dispatch thread started");

    while(running.load())
    {
        std::unique_lock<std::mutex> lock(event_queue_mutex);

        /*-------------------------------------------------*\
        | Wait for events or shutdown                        |
        \*-------------------------------------------------*/
        event_queue_cv.wait(lock, [this]()
        {
            return !event_queue.empty() || !running.load();
        });

        if(!running.load())
        {
            break;
        }

        /*-------------------------------------------------*\
        | Debounce: wait for events to settle                |
        \*-------------------------------------------------*/
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(DEBOUNCE_MS));
        lock.lock();

        if(event_queue.empty())
        {
            continue;
        }

        /*-------------------------------------------------*\
        | Drain and coalesce events per VID/PID              |
        | Key: (vid << 16 | pid)                             |
        | Value: net effect (positive = arrived, negative    |
        |        = left, zero = no-op)                       |
        \*-------------------------------------------------*/
        std::unordered_map<uint32_t, int> coalesced;

        while(!event_queue.empty())
        {
            HotplugEvent ev = event_queue.front();
            event_queue.pop_front();

            uint32_t key = ((uint32_t)ev.vid << 16) | ev.pid;

            if(ev.type == HotplugEventType::Arrived)
            {
                coalesced[key]++;
            }
            else
            {
                coalesced[key]--;
            }
        }

        lock.unlock();

        /*-------------------------------------------------*\
        | Process coalesced events                           |
        \*-------------------------------------------------*/
        for(auto& [key, net_effect] : coalesced)
        {
            uint16_t vid = (uint16_t)(key >> 16);
            uint16_t pid = (uint16_t)(key & 0xFFFF);

            if(net_effect < 0)
            {
                /*-----------------------------------------*\
                | Net disconnect: remove devices             |
                \*-----------------------------------------*/
                LOG_INFO("[USBHotplugMonitor] Dispatching removal for %04X:%04X", vid, pid);
                resource_manager->RunInBackgroundThread([this, vid, pid]()
                {
                    resource_manager->RemoveDevicesByVidPid(vid, pid);
                });
            }
            else if(net_effect > 0)
            {
                /*-----------------------------------------*\
                | Net connect: detect new devices            |
                \*-----------------------------------------*/
                LOG_INFO("[USBHotplugMonitor] Dispatching targeted detection for %04X:%04X", vid, pid);
                resource_manager->RunInBackgroundThread([this, vid, pid]()
                {
                    resource_manager->TargetedDetectDevices(vid, pid);
                });
            }
            else
            {
                /*-----------------------------------------*\
                | Net zero (plug+unplug or vice versa):      |
                | remove then re-detect to handle reconnect  |
                \*-----------------------------------------*/
                LOG_INFO("[USBHotplugMonitor] Dispatching reconnect for %04X:%04X", vid, pid);
                resource_manager->RunInBackgroundThread([this, vid, pid]()
                {
                    resource_manager->RemoveDevicesByVidPid(vid, pid);
                    resource_manager->TargetedDetectDevices(vid, pid);
                });
            }
        }
    }

    LOG_DEBUG("[USBHotplugMonitor] Dispatch thread stopped");
}
