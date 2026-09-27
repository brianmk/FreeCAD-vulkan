#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

static PFN_vkGetInstanceProcAddr g_next_gipa;
static PFN_vkGetDeviceProcAddr g_next_gdpa;
static VkInstance g_instance;

typedef struct {
    VkDevice dev;
    PFN_vkCreateSwapchainKHR next;
} DevSlot;
static DevSlot g_devs[64];
static int g_ndev;

static PFN_vkCreateSwapchainKHR next_swapchain(VkDevice dev)
{
    for (int i = 0; i < g_ndev; ++i)
        if (g_devs[i].dev == dev)
            return g_devs[i].next;
    return NULL;
}

static VkLayerInstanceCreateInfo *instance_link(const VkInstanceCreateInfo *ci)
{
    VkLayerInstanceCreateInfo *p = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            p->function == VK_LAYER_LINK_INFO)
            return p;
        p = (VkLayerInstanceCreateInfo *)p->pNext;
    }
    return NULL;
}

static VkLayerDeviceCreateInfo *device_link(const VkDeviceCreateInfo *ci)
{
    VkLayerDeviceCreateInfo *p = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            p->function == VK_LAYER_LINK_INFO)
            return p;
        p = (VkLayerDeviceCreateInfo *)p->pNext;
    }
    return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_create_instance(const VkInstanceCreateInfo *ci,
                   const VkAllocationCallbacks *alloc, VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = instance_link(ci);
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    g_next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance next =
        (PFN_vkCreateInstance)g_next_gipa(NULL, "vkCreateInstance");
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult r = next(ci, alloc, out);
    if (r == VK_SUCCESS)
        g_instance = *out;
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                 const VkAllocationCallbacks *alloc, VkDevice *out)
{
    VkLayerDeviceCreateInfo *link = device_link(ci);
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateDevice next =
        (PFN_vkCreateDevice)gipa(g_instance, "vkCreateDevice");
    VkResult r = next(phys, ci, alloc, out);
    if (r == VK_SUCCESS && g_ndev < 64) {
        g_next_gdpa = gdpa;
        g_devs[g_ndev].dev = *out;
        g_devs[g_ndev].next =
            (PFN_vkCreateSwapchainKHR)gdpa(*out, "vkCreateSwapchainKHR");
        ++g_ndev;
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
                    const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    PFN_vkCreateSwapchainKHR next = next_swapchain(dev);
    if (!next)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkSwapchainCreateInfoKHR mod = *ci;
    VkPresentModeKHR orig = mod.presentMode;
    if (!getenv("FC_NOVSYNC_FORCE")) {
        fprintf(stderr, "[novsync] app requested presentMode %d (log-only)\n",
                (int)orig);
        return next(dev, &mod, alloc, out);
    }
    VkPresentModeKHR want = getenv("FC_NOVSYNC_MAILBOX")
                                ? VK_PRESENT_MODE_MAILBOX_KHR
                                : VK_PRESENT_MODE_IMMEDIATE_KHR;

    mod.presentMode = want;
    VkResult r = next(dev, &mod, alloc, out);
    if (r != VK_SUCCESS) {
        mod.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        r = next(dev, &mod, alloc, out);
    }
    if (r != VK_SUCCESS) {
        mod.presentMode = orig;
        r = next(dev, &mod, alloc, out);
    }
    fprintf(stderr, "[novsync] presentMode %d -> %d (result %d)\n",
            (int)orig, (int)mod.presentMode, (int)r);
    return r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_gipa(VkInstance inst, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)layer_gipa;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)g_next_gdpa;
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)nv_create_instance;
    if (!strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)nv_create_device;
    return g_next_gipa ? g_next_gipa(inst, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_gdpa(VkDevice dev, const char *name)
{
    if (!strcmp(name, "vkCreateSwapchainKHR"))
        return (PFN_vkVoidFunction)nv_create_swapchain;
    return g_next_gdpa ? g_next_gdpa(dev, name) : NULL;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance inst, const char *name)
{
    return layer_gipa(inst, name);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice dev, const char *name)
{
    return layer_gdpa(dev, name);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *p)
{
    if (p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (p->loaderLayerInterfaceVersion > 2)
        p->loaderLayerInterfaceVersion = 2;
    p->pfnGetInstanceProcAddr = layer_gipa;
    p->pfnGetDeviceProcAddr = layer_gdpa;
    p->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
