/* present_vulkan.c — Vulkan present backend (groundwork).
 *
 * Built only when CMake finds Vulkan (-DBENEFACTOR_HAVE_VULKAN). Two parts:
 *
 *   1. The Vulkan CORE + an OFFSCREEN render->readback self-test
 *      (present_vulkan_selftest). This proves, display-independently, that this
 *      machine can: create a device, upload the composed output surface to a
 *      sampled image, and run a fullscreen-quad shader pipeline that reproduces
 *      it. This is the exact scaffold a future per-character lighting pass needs.
 *      Invoke headless via `benefactor-pc --vk-selftest`.
 *
 *   2. The windowed swapchain present (present_backend_vulkan). TODO Phase 2b:
 *      needs a live display/surface to verify, so until then this returns NULL and
 *      present_backend_select() falls back to SDL.
 *
 * See ~/.claude/plans + docs/codebase-layout.md (render/ module). */
#include "render/present_backend.h"

#ifdef BENEFACTOR_HAVE_VULKAN
#include <vulkan/vulkan.h>
#include <SDL2/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SPIR-V embedded at build time (glslc -mfmt=num, see CMakeLists). */
static const uint32_t k_blit_vert_spv[] = {
#include "blit.vert.inc"
};
static const uint32_t k_blit_frag_spv[] = {
#include "blit.frag.inc"
};

#define VKLOG(...) fprintf(stderr, "[vulkan] " __VA_ARGS__)
#define VK_OK(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) { \
    VKLOG("%s failed: VkResult=%d (line %d)\n", #call, (int)_r, __LINE__); goto fail; } } while (0)

static uint32_t find_mem_type(VkPhysicalDevice pd, uint32_t type_bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* Offscreen: render `argb` (w x h, ARGB8888 = B8G8R8A8 in memory) through the
 * fullscreen-quad pipeline into an offscreen B8G8R8A8 image, read it back into
 * `out` (w*h u32). Returns 0 on success, -1 on any Vulkan failure. No surface,
 * no swapchain, no window — runs with the display off. */
static int vk_offscreen_render(const uint32_t *argb, int w, int h, uint32_t *out)
{
    int rc = -1;
    VkInstance inst = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkImage src_img = VK_NULL_HANDLE, dst_img = VK_NULL_HANDLE;
    VkDeviceMemory src_mem = VK_NULL_HANDLE, dst_mem = VK_NULL_HANDLE;
    VkImageView src_view = VK_NULL_HANDLE, dst_view = VK_NULL_HANDLE;
    VkSampler samp = VK_NULL_HANDLE;
    VkBuffer up_buf = VK_NULL_HANDLE, rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory up_mem = VK_NULL_HANDLE, rb_mem = VK_NULL_HANDLE;
    VkRenderPass rpass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkPipelineLayout pll = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const VkDeviceSize img_bytes = (VkDeviceSize)w * h * 4;
    const VkFormat FMT = VK_FORMAT_B8G8R8A8_UNORM;

    /* ── instance ─────────────────────────────────────────────────────────── */
    {
        VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "benefactor", .apiVersion = VK_API_VERSION_1_0 };
        VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &ai };
        VK_OK(vkCreateInstance(&ci, NULL, &inst));
    }

    /* ── physical device + graphics queue family ──────────────────────────── */
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t qfam = UINT32_MAX;
    {
        uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
        if (!n) { VKLOG("no Vulkan physical devices\n"); goto fail; }
        VkPhysicalDevice *devs = calloc(n, sizeof *devs);
        vkEnumeratePhysicalDevices(inst, &n, devs);
        for (uint32_t i = 0; i < n && pd == VK_NULL_HANDLE; i++) {
            uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
            VkQueueFamilyProperties *q = calloc(qn, sizeof *q);
            vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, q);
            for (uint32_t j = 0; j < qn; j++)
                if (q[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) { pd = devs[i]; qfam = j; break; }
            free(q);
        }
        free(devs);
        if (pd == VK_NULL_HANDLE) { VKLOG("no graphics-capable device\n"); goto fail; }
        VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
        VKLOG("device: %s\n", props.deviceName);
    }

    /* ── logical device + queue ───────────────────────────────────────────── */
    VkQueue queue;
    {
        float pri = 1.0f;
        VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &pri };
        VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
        VK_OK(vkCreateDevice(pd, &di, NULL, &dev));
        vkGetDeviceQueue(dev, qfam, 0, &queue);
    }

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfam };
    VK_OK(vkCreateCommandPool(dev, &pci, NULL, &cpool));

    /* ── create an image with backing memory + view ───────────────────────── */
    #define MK_IMAGE(img, mem, view, _usg) do { \
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, \
            .imageType = VK_IMAGE_TYPE_2D, .format = FMT, \
            .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1, .arrayLayers = 1, \
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, \
            .usage = (_usg), .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED }; \
        VK_OK(vkCreateImage(dev, &ici, NULL, &(img))); \
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, (img), &mr); \
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, \
            .allocationSize = mr.size, \
            .memoryTypeIndex = find_mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) }; \
        VK_OK(vkAllocateMemory(dev, &mai, NULL, &(mem))); \
        VK_OK(vkBindImageMemory(dev, (img), (mem), 0)); \
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, \
            .image = (img), .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = FMT, \
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }; \
        VK_OK(vkCreateImageView(dev, &vi, NULL, &(view))); \
    } while (0)

    MK_IMAGE(src_img, src_mem, src_view,
             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    MK_IMAGE(dst_img, dst_mem, dst_view,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    #undef MK_IMAGE

    /* sampler (nearest, clamp) */
    {
        VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        VK_OK(vkCreateSampler(dev, &si, NULL, &samp));
    }

    /* ── host buffers: upload (src) + readback (dst) ──────────────────────── */
    #define MK_BUFFER(buf, mem, _usg) do { \
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, \
            .size = img_bytes, .usage = (_usg), .sharingMode = VK_SHARING_MODE_EXCLUSIVE }; \
        VK_OK(vkCreateBuffer(dev, &bi, NULL, &(buf))); \
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, (buf), &mr); \
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, \
            .allocationSize = mr.size, .memoryTypeIndex = find_mem_type(pd, mr.memoryTypeBits, \
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) }; \
        VK_OK(vkAllocateMemory(dev, &mai, NULL, &(mem))); \
        VK_OK(vkBindBufferMemory(dev, (buf), (mem), 0)); \
    } while (0)

    MK_BUFFER(up_buf, up_mem, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    MK_BUFFER(rb_buf, rb_mem, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    #undef MK_BUFFER

    { void *p; VK_OK(vkMapMemory(dev, up_mem, 0, img_bytes, 0, &p));
      memcpy(p, argb, img_bytes); vkUnmapMemory(dev, up_mem); }

    /* ── render pass (one color attachment, final layout TRANSFER_SRC) ─────── */
    {
        VkAttachmentDescription at = { .format = FMT, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
        VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1, .pColorAttachments = &ar };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &at, .subpassCount = 1, .pSubpasses = &sp };
        VK_OK(vkCreateRenderPass(dev, &rci, NULL, &rpass));
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rpass, .attachmentCount = 1, .pAttachments = &dst_view,
            .width = (uint32_t)w, .height = (uint32_t)h, .layers = 1 };
        VK_OK(vkCreateFramebuffer(dev, &fci, NULL, &fb));
    }

    /* ── shaders, descriptor set, pipeline ────────────────────────────────── */
    {
        VkShaderModuleCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof k_blit_vert_spv, .pCode = k_blit_vert_spv };
        VK_OK(vkCreateShaderModule(dev, &vi, NULL, &vs));
        VkShaderModuleCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof k_blit_frag_spv, .pCode = k_blit_frag_spv };
        VK_OK(vkCreateShaderModule(dev, &fi, NULL, &fs));
    }
    {
        VkDescriptorSetLayoutBinding b = { .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
        VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &b };
        VK_OK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
        VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &dsl };
        VK_OK(vkCreatePipelineLayout(dev, &pli, NULL, &pll));
    }
    VkDescriptorSet dset;
    {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
        VK_OK(vkCreateDescriptorPool(dev, &ci, NULL, &dpool));
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
        VK_OK(vkAllocateDescriptorSets(dev, &ai, &dset));
        VkDescriptorImageInfo dii = { .sampler = samp, .imageView = src_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
        vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    }
    {
        VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
        VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
        VkRect2D sc = { {0,0}, {(uint32_t)w,(uint32_t)h} };
        VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba };
        VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = stages, .pVertexInputState = &vin,
            .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pll, .renderPass = rpass };
        VK_OK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
    }

    /* ── record + submit ──────────────────────────────────────────────────── */
    VkCommandBuffer cb;
    {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VK_OK(vkAllocateCommandBuffers(dev, &ai, &cb));
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VK_OK(vkBeginCommandBuffer(cb, &bi));

        /* src: UNDEFINED -> TRANSFER_DST, upload, -> SHADER_READ_ONLY */
        VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = src_img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 },
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &b1);
        VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
        vkCmdCopyBufferToImage(cb, up_buf, src_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        VkImageMemoryBarrier b2 = b1;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &b2);

        VkClearValue clr = { .color = { .float32 = {0,0,0,1} } };
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rpass, .framebuffer = fb,
            .renderArea = { {0,0}, {(uint32_t)w,(uint32_t)h} }, .clearValueCount = 1, .pClearValues = &clr };
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pll, 0, 1, &dset, 0, NULL);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);  /* dst now in TRANSFER_SRC_OPTIMAL (render pass finalLayout) */

        VkBufferImageCopy rbc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
        vkCmdCopyImageToBuffer(cb, dst_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &rbc);
        VK_OK(vkEndCommandBuffer(cb));

        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_OK(vkCreateFence(dev, &fci, NULL, &fence));
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb };
        VK_OK(vkQueueSubmit(queue, 1, &si, fence));
        VK_OK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    }

    { void *p; VK_OK(vkMapMemory(dev, rb_mem, 0, img_bytes, 0, &p));
      memcpy(out, p, img_bytes); vkUnmapMemory(dev, rb_mem); }

    rc = 0;
fail:
    if (dev) vkDeviceWaitIdle(dev);
    if (fence) vkDestroyFence(dev, fence, NULL);
    if (pipe) vkDestroyPipeline(dev, pipe, NULL);
    if (pll) vkDestroyPipelineLayout(dev, pll, NULL);
    if (dpool) vkDestroyDescriptorPool(dev, dpool, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    if (vs) vkDestroyShaderModule(dev, vs, NULL);
    if (fs) vkDestroyShaderModule(dev, fs, NULL);
    if (fb) vkDestroyFramebuffer(dev, fb, NULL);
    if (rpass) vkDestroyRenderPass(dev, rpass, NULL);
    if (rb_buf) vkDestroyBuffer(dev, rb_buf, NULL);
    if (rb_mem) vkFreeMemory(dev, rb_mem, NULL);
    if (up_buf) vkDestroyBuffer(dev, up_buf, NULL);
    if (up_mem) vkFreeMemory(dev, up_mem, NULL);
    if (samp) vkDestroySampler(dev, samp, NULL);
    if (src_view) vkDestroyImageView(dev, src_view, NULL);
    if (dst_view) vkDestroyImageView(dev, dst_view, NULL);
    if (src_img) vkDestroyImage(dev, src_img, NULL);
    if (dst_img) vkDestroyImage(dev, dst_img, NULL);
    if (src_mem) vkFreeMemory(dev, src_mem, NULL);
    if (dst_mem) vkFreeMemory(dev, dst_mem, NULL);
    if (cpool) vkDestroyCommandPool(dev, cpool, NULL);
    if (dev) vkDestroyDevice(dev, NULL);
    if (inst) vkDestroyInstance(inst, NULL);
    return rc;
}

/* Public self-test: render argb through Vulkan offscreen, read back, compare.
 * Returns the max per-channel abs difference (0 = exact), or -1 on Vulkan error. */
int present_vulkan_selftest(const uint32_t *argb, int w, int h)
{
    uint32_t *out = malloc((size_t)w * h * 4);
    if (!out) return -1;
    int rc = vk_offscreen_render(argb, w, h, out);
    if (rc != 0) { free(out); return -1; }
    int maxdiff = 0;
    const uint8_t *a = (const uint8_t *)argb, *b = (const uint8_t *)out;
    for (size_t i = 0; i < (size_t)w * h * 4; i++) {
        int d = (int)a[i] - (int)b[i]; if (d < 0) d = -d;
        if (d > maxdiff) maxdiff = d;
    }
    free(out);
    VKLOG("selftest %dx%d: max channel diff = %d\n", w, h, maxdiff);
    return maxdiff;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Windowed swapchain present (blit-based).
 *
 * Each frame: upload `argb` into a device-local source image, acquire a
 * swapchain image, vkCmdBlitImage (NEAREST, scales content->window) the source
 * into it, present. No graphics pipeline needed for present — the verified
 * fullscreen-quad shader pipeline (above) is where the Phase 3 per-character
 * lighting pass will render INTO the swapchain instead of a plain blit.
 * ───────────────────────────────────────────────────────────────────────── */
typedef struct {
    SDL_Window      *win;
    VkInstance       inst;
    VkPhysicalDevice pd;
    uint32_t         qfam;
    VkDevice         dev;
    VkQueue          queue;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swap;
    VkExtent2D       extent;
    uint32_t         n_images;
    VkImage         *images;          /* owned by the swapchain */
    VkCommandPool    cpool;
    VkCommandBuffer  cmd;
    int              cw, ch;          /* content size */
    VkImage          src_img;
    VkDeviceMemory   src_mem;
    VkBuffer         up_buf;
    VkDeviceMemory   up_mem;
    void            *up_ptr;          /* persistently mapped */
    VkSemaphore      sem_acquire, sem_done;
    VkFence          fence;
    int             ok;
} Swap;
static Swap g_sw;

static int sw_make_swapchain(Swap *s, uint32_t w, uint32_t h)
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->pd, s->surface, &caps) != VK_SUCCESS)
        return -1;
    s->extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent
              : (VkExtent2D){ w, h };

    /* Prefer B8G8R8A8_UNORM (matches the source: no blit colour conversion). */
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(s->pd, s->surface, &nf, NULL);
    VkSurfaceFormatKHR *fmts = calloc(nf, sizeof *fmts);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->pd, s->surface, &nf, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; break; }
    free(fmts);

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s->surface, .minImageCount = want, .imageFormat = pick.format,
        .imageColorSpace = pick.colorSpace, .imageExtent = s->extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,   /* we blit into it */
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,   /* always supported */
        .clipped = VK_TRUE };
    if (vkCreateSwapchainKHR(s->dev, &ci, NULL, &s->swap) != VK_SUCCESS) return -1;
    vkGetSwapchainImagesKHR(s->dev, s->swap, &s->n_images, NULL);
    s->images = calloc(s->n_images, sizeof *s->images);
    vkGetSwapchainImagesKHR(s->dev, s->swap, &s->n_images, s->images);
    return 0;
}

static int vulkan_init(const char *title, int cw, int ch)
{
    Swap *s = &g_sw;
    memset(s, 0, sizeof *s);
    s->cw = cw; s->ch = ch;

    s->win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cw * 2, ch * 2, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!s->win) { VKLOG("SDL_CreateWindow(VULKAN): %s\n", SDL_GetError()); return -1; }

    unsigned next = 0;
    SDL_Vulkan_GetInstanceExtensions(s->win, &next, NULL);
    const char **exts = calloc(next, sizeof *exts);
    SDL_Vulkan_GetInstanceExtensions(s->win, &next, exts);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "benefactor", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai, .enabledExtensionCount = next, .ppEnabledExtensionNames = exts };
    VkResult r = vkCreateInstance(&ici, NULL, &s->inst);
    free(exts);
    if (r != VK_SUCCESS) { VKLOG("vkCreateInstance: %d\n", r); goto fail; }

    if (!SDL_Vulkan_CreateSurface(s->win, s->inst, &s->surface)) {
        VKLOG("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError()); goto fail; }

    /* pick a physical device with a graphics+present queue family */
    uint32_t nd = 0; vkEnumeratePhysicalDevices(s->inst, &nd, NULL);
    VkPhysicalDevice *devs = calloc(nd, sizeof *devs);
    vkEnumeratePhysicalDevices(s->inst, &nd, devs);
    for (uint32_t i = 0; i < nd && s->pd == VK_NULL_HANDLE; i++) {
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq, sizeof *q);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, q);
        for (uint32_t j = 0; j < nq; j++) {
            VkBool32 present = 0;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], j, s->surface, &present);
            if ((q[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                s->pd = devs[i]; s->qfam = j; break;
            }
        }
        free(q);
    }
    free(devs);
    if (s->pd == VK_NULL_HANDLE) { VKLOG("no graphics+present device\n"); goto fail; }
    { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(s->pd, &p);
      VKLOG("windowed device: %s\n", p.deviceName); }

    { float pri = 1.0f;
      const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
      VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
          .queueFamilyIndex = s->qfam, .queueCount = 1, .pQueuePriorities = &pri };
      VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
          .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
          .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
      if (vkCreateDevice(s->pd, &di, NULL, &s->dev) != VK_SUCCESS) { VKLOG("vkCreateDevice\n"); goto fail; }
      vkGetDeviceQueue(s->dev, s->qfam, 0, &s->queue); }

    if (sw_make_swapchain(s, (uint32_t)cw, (uint32_t)ch) != 0) { VKLOG("swapchain\n"); goto fail; }

    { VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s->qfam };
      vkCreateCommandPool(s->dev, &pci, NULL, &s->cpool);
      VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s->cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
      vkAllocateCommandBuffers(s->dev, &cai, &s->cmd); }

    /* source content image (B8G8R8A8) + persistently-mapped upload buffer */
    { VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { (uint32_t)cw, (uint32_t)ch, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
      if (vkCreateImage(s->dev, &ii, NULL, &s->src_img) != VK_SUCCESS) goto fail;
      VkMemoryRequirements mr; vkGetImageMemoryRequirements(s->dev, s->src_img, &mr);
      VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_mem_type(s->pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
      if (vkAllocateMemory(s->dev, &mai, NULL, &s->src_mem) != VK_SUCCESS) goto fail;
      vkBindImageMemory(s->dev, s->src_img, s->src_mem, 0); }
    { VkDeviceSize bytes = (VkDeviceSize)cw * ch * 4;
      VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
      if (vkCreateBuffer(s->dev, &bi, NULL, &s->up_buf) != VK_SUCCESS) goto fail;
      VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s->dev, s->up_buf, &mr);
      VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = find_mem_type(s->pd, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
      if (vkAllocateMemory(s->dev, &mai, NULL, &s->up_mem) != VK_SUCCESS) goto fail;
      vkBindBufferMemory(s->dev, s->up_buf, s->up_mem, 0);
      vkMapMemory(s->dev, s->up_mem, 0, bytes, 0, &s->up_ptr); }

    { VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      vkCreateSemaphore(s->dev, &si, NULL, &s->sem_acquire);
      vkCreateSemaphore(s->dev, &si, NULL, &s->sem_done);
      VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT };
      vkCreateFence(s->dev, &fi, NULL, &s->fence); }

    s->ok = 1;
    return 0;
fail:
    return -1;
}

static void sw_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = from, .newLayout = to, .srcAccessMask = sa, .dstAccessMask = da,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static void vulkan_present(const uint32_t *argb, int w, int h)
{
    Swap *s = &g_sw;
    if (!s->ok) return;
    (void)w; (void)h;   /* uploads the content at s->cw x s->ch */
    memcpy(s->up_ptr, argb, (size_t)s->cw * s->ch * 4);

    vkWaitForFences(s->dev, 1, &s->fence, VK_TRUE, UINT64_MAX);

    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(s->dev, s->swap, UINT64_MAX, s->sem_acquire, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {   /* window resized: rebuild swapchain, skip frame */
        vkDeviceWaitIdle(s->dev);
        vkDestroySwapchainKHR(s->dev, s->swap, NULL); free(s->images); s->images = NULL;
        sw_make_swapchain(s, s->extent.width, s->extent.height);
        return;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return;
    vkResetFences(s->dev, 1, &s->fence);

    vkResetCommandBuffer(s->cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(s->cmd, &bi);

    /* upload -> src image, leave it TRANSFER_SRC */
    sw_barrier(s->cmd, s->src_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t)s->cw, (uint32_t)s->ch, 1 } };
    vkCmdCopyBufferToImage(s->cmd, s->up_buf, s->src_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    sw_barrier(s->cmd, s->src_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* swapchain image: UNDEFINED -> TRANSFER_DST, blit (scaled), -> PRESENT_SRC */
    sw_barrier(s->cmd, s->images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { {0,0,0}, { s->cw, s->ch, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { {0,0,0}, { (int)s->extent.width, (int)s->extent.height, 1 } } };
    vkCmdBlitImage(s->cmd, s->src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    sw_barrier(s->cmd, s->images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(s->cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s->sem_acquire, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &s->cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s->sem_done };
    vkQueueSubmit(s->queue, 1, &si, s->fence);

    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s->sem_done,
        .swapchainCount = 1, .pSwapchains = &s->swap, .pImageIndices = &idx };
    VkResult pr = vkQueuePresentKHR(s->queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(s->dev);
        vkDestroySwapchainKHR(s->dev, s->swap, NULL); free(s->images); s->images = NULL;
        sw_make_swapchain(s, s->extent.width, s->extent.height);
    }
}

static void vulkan_toggle_fullscreen(void)
{
    uint32_t f = SDL_GetWindowFlags(g_sw.win);
    SDL_SetWindowFullscreen(g_sw.win, (f & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

static SDL_Window *vulkan_window(void) { return g_sw.win; }

static void vulkan_shutdown(void)
{
    Swap *s = &g_sw;
    if (s->dev) vkDeviceWaitIdle(s->dev);
    if (s->fence) vkDestroyFence(s->dev, s->fence, NULL);
    if (s->sem_acquire) vkDestroySemaphore(s->dev, s->sem_acquire, NULL);
    if (s->sem_done) vkDestroySemaphore(s->dev, s->sem_done, NULL);
    if (s->up_buf) vkDestroyBuffer(s->dev, s->up_buf, NULL);
    if (s->up_mem) vkFreeMemory(s->dev, s->up_mem, NULL);
    if (s->src_img) vkDestroyImage(s->dev, s->src_img, NULL);
    if (s->src_mem) vkFreeMemory(s->dev, s->src_mem, NULL);
    if (s->cpool) vkDestroyCommandPool(s->dev, s->cpool, NULL);
    if (s->swap) vkDestroySwapchainKHR(s->dev, s->swap, NULL);
    free(s->images);
    if (s->dev) vkDestroyDevice(s->dev, NULL);
    if (s->surface) vkDestroySurfaceKHR(s->inst, s->surface, NULL);
    if (s->inst) vkDestroyInstance(s->inst, NULL);
    if (s->win) SDL_DestroyWindow(s->win);
    memset(s, 0, sizeof *s);
}

static const PresentBackend VULKAN_BACKEND = {
    "vulkan", vulkan_init, vulkan_present, NULL /* per-sprite present: SDL only (P3 shelved) */,
    vulkan_toggle_fullscreen, vulkan_window, vulkan_shutdown
};

/* Returns the Vulkan backend; present_backend_select() handles the case where
 * vulkan_init() later fails (it returns -1 and hw_init aborts that backend). */
const PresentBackend *present_backend_vulkan(void) { return &VULKAN_BACKEND; }

#endif /* BENEFACTOR_HAVE_VULKAN */
