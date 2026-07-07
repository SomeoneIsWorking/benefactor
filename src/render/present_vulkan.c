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
 *   2. BenRen VK — the windowed per-sprite renderer (present_backend_vulkan, the
 *      "Hardware" renderer). It draws the BenRen Scene draw list on the GPU: one
 *      textured quad per sprite/tile/banner (quad.vert/frag) sampling a per-frame
 *      atlas, then a blended fullscreen LIGHTING pass (fx.frag) driven by the
 *      renderer's FxFrame (player light + playfield rows + effect flags). The SDL
 *      path is never touched; effects are GPU-only.
 *
 * See instructions/rendering-overhaul-plan.md + docs/codebase-layout.md. */
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
/* BenRen VK per-sprite renderer shaders (one textured/flat quad per sprite). */
static const uint32_t k_quad_vert_spv[] = {
#include "quad.vert.inc"
};
static const uint32_t k_quad_frag_spv[] = {
#include "quad.frag.inc"
};
/* Lighting pass (fullscreen, blended over the scene): reuses blit.vert. */
static const uint32_t k_fx_frag_spv[] = {
#include "fx.frag.inc"
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
 * BenRen VK — the per-sprite "Hardware" renderer.
 *
 * The Vulkan twin of scene_sdl.c: instead of uploading a CPU-composited frame and
 * blitting it, this draws the BenRen Scene draw list itself — one GPU quad per
 * sprite/tile/banner. Each frame the quads are shelf-packed + baked (per-output-row
 * palette resolve) into a sprite ATLAS image, and a textured-quad pipeline issues
 * one draw per quad sampling the atlas (with a flat-colour mode for the per-row void
 * background). World quads are camera-projected (screen_x = x - view_left) and
 * scissor-clipped to the camera columns + playfield rows; screen quads (the banner)
 * draw on top with no camera. The base rows the scene does NOT own (top border +
 * HUD) come from the composed surface as a base texture.
 *
 * Because every sprite is its own draw with its own push constants, this is the
 * seam a per-object shader / lighting term hangs off next (the FxFrame from
 * set_effects is stored, ready for that pass). present() (no scene) is the overlay
 * fallback: one fullscreen base quad.
 *
 * DYNAMIC viewport/scissor → a window resize rebuilds only the swapchain +
 * framebuffers, never the pipeline. The offscreen selftest (above) is untouched.
 * ───────────────────────────────────────────────────────────────────────── */
#define VK_ATLAS_W 1024
#define VK_ATLAS_H 2048

/* Push constants — MUST match shaders/quad.vert + quad.frag (64 bytes). */
typedef struct {
    float   dst[4];     /* x, y, w, h in output pixels */
    float   src[4];     /* u0, v0, u1, v1 in atlas UV */
    float   color[4];   /* flat colour (mode 1) */
    float   screen[2];  /* output extent (pixel -> NDC) */
    int32_t mode;       /* 0 = sample atlas, 1 = flat colour */
    int32_t _pad;
} QuadPush;

/* Lighting-pass push constants — MUST match shaders/fx.frag (32 bytes). */
typedef struct {
    float   res[2];     /* content_w, content_h */
    float   light[2];   /* player centre, content px; <0 = none */
    float   pf[2];      /* pf_top, pf_bot, content px */
    int32_t flags;      /* FX_* bitmask */
    int32_t mode;       /* 0 = dim (multiply), 1 = glow (add) */
} FxPush;

/* A SAMPLED image + its persistently-mapped host upload buffer (atlas / base). */
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    VkBuffer       up_buf;
    VkDeviceMemory up_mem;
    void          *up_ptr;
    int            w, h;
    int            uploaded;     /* layout is SHADER_READ_ONLY (vs UNDEFINED) */
} VkTex;

typedef struct {
    SDL_Window      *win;
    VkInstance       inst;
    VkPhysicalDevice pd;
    uint32_t         qfam;
    VkDevice         dev;
    VkQueue          queue;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swap;
    VkFormat         sc_format;       /* swapchain image format (for the render pass) */
    VkExtent2D       extent;
    float            vp[4];           /* content viewport (x,y,w,h) in the swapchain — letterboxed
                                       * to the content aspect each frame; NEVER stretch to fill */
    uint32_t         n_images;
    VkImage         *images;          /* owned by the swapchain */
    VkImageView     *views;           /* one per swapchain image */
    VkFramebuffer   *fbs;             /* one per swapchain image */
    VkCommandPool    cpool;
    VkCommandBuffer  cmd;
    VkRenderPass     rpass;
    VkSampler        samp;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet  dset_atlas;      /* binds the sprite atlas */
    VkDescriptorSet  dset_base;       /* binds the base (composed) texture */
    VkPipelineLayout pll;
    VkPipeline       pipe;             /* opaque sprite/base/void quads */
    VkPipeline       pipe_quad_shadow; /* same quads, ALPHA blend — the per-character drop shadow */
    VkShaderModule   vs, fs;
    /* Lighting pass (blended fullscreen, no descriptor set): blit.vert + fx.frag. */
    VkShaderModule   fx_vs, fx_fs;
    VkPipelineLayout pll_fx;
    VkPipeline       pipe_fx_dim;     /* multiply blend (ambient darkness + torch glow) */
    VkTex            atlas;           /* VK_ATLAS_W x VK_ATLAS_H sprite atlas */
    VkTex            base;            /* content-sized base / fallback texture */
    VkSemaphore      sem_acquire;     /* single (1 frame in flight; freed by the fence wait) */
    VkSemaphore     *sem_done;        /* per swapchain image — present may still hold the prev one */
    VkFence          fence;
    FxFrame          fx;              /* latest effect params — for the future lighting pass */
    int              ok;
} Swap;
static Swap g_sw;

static void sw_destroy_targets(Swap *s)
{
    if (s->fbs) {
        for (uint32_t i = 0; i < s->n_images; i++)
            if (s->fbs[i]) vkDestroyFramebuffer(s->dev, s->fbs[i], NULL);
        free(s->fbs); s->fbs = NULL;
    }
    if (s->views) {
        for (uint32_t i = 0; i < s->n_images; i++)
            if (s->views[i]) vkDestroyImageView(s->dev, s->views[i], NULL);
        free(s->views); s->views = NULL;
    }
    if (s->sem_done) {
        for (uint32_t i = 0; i < s->n_images; i++)
            if (s->sem_done[i]) vkDestroySemaphore(s->dev, s->sem_done[i], NULL);
        free(s->sem_done); s->sem_done = NULL;
    }
}

static int sw_make_swapchain(Swap *s, uint32_t w, uint32_t h)
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->pd, s->surface, &caps) != VK_SUCCESS)
        return -1;
    s->extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent
              : (VkExtent2D){ w, h };
    if (s->extent.width == 0 || s->extent.height == 0) s->extent = (VkExtent2D){ w, h };

    /* Prefer B8G8R8A8_UNORM (matches the source surface: no colour conversion). */
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(s->pd, s->surface, &nf, NULL);
    VkSurfaceFormatKHR *fmts = calloc(nf, sizeof *fmts);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->pd, s->surface, &nf, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; break; }
    free(fmts);
    s->sc_format = pick.format;

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s->surface, .minImageCount = want, .imageFormat = pick.format,
        .imageColorSpace = pick.colorSpace, .imageExtent = s->extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,   /* we render into it */
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

/* Image views + framebuffers for the current swapchain images (needs s->rpass). */
static int sw_make_targets(Swap *s)
{
    s->views = calloc(s->n_images, sizeof *s->views);
    s->fbs   = calloc(s->n_images, sizeof *s->fbs);
    for (uint32_t i = 0; i < s->n_images; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = s->images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = s->sc_format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (vkCreateImageView(s->dev, &vi, NULL, &s->views[i]) != VK_SUCCESS) return -1;
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = s->rpass, .attachmentCount = 1, .pAttachments = &s->views[i],
            .width = s->extent.width, .height = s->extent.height, .layers = 1 };
        if (vkCreateFramebuffer(s->dev, &fi, NULL, &s->fbs[i]) != VK_SUCCESS) return -1;
    }
    /* One "render done" semaphore per image: present may still be consuming the
     * previous frame's, so it can't be reused until that image is reacquired. */
    s->sem_done = calloc(s->n_images, sizeof *s->sem_done);
    for (uint32_t i = 0; i < s->n_images; i++) {
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(s->dev, &sci, NULL, &s->sem_done[i]) != VK_SUCCESS) return -1;
    }
    return 0;
}

/* Rebuild swapchain + targets at the window's CURRENT drawable size, so the render
 * resolution tracks the window live (incl. Wayland, where the surface currentExtent
 * is "undefined" and we must supply the size). */
static int sw_rebuild(Swap *s)
{
    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(s->win, &dw, &dh);
    if (dw <= 0) dw = (int)s->extent.width;
    if (dh <= 0) dh = (int)s->extent.height;
    vkDeviceWaitIdle(s->dev);
    sw_destroy_targets(s);
    if (s->swap) vkDestroySwapchainKHR(s->dev, s->swap, NULL);
    free(s->images); s->images = NULL;
    if (sw_make_swapchain(s, (uint32_t)dw, (uint32_t)dh) != 0) return -1;
    return sw_make_targets(s);
}

/* If the window changed size since last frame, rebuild to match — the render
 * resolution follows the window dynamically, not only on OUT_OF_DATE. */
static void vk_check_resize(Swap *s)
{
    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(s->win, &dw, &dh);
    if (dw > 0 && dh > 0 &&
        ((uint32_t)dw != s->extent.width || (uint32_t)dh != s->extent.height))
        sw_rebuild(s);
}

/* ── SAMPLED texture (atlas / base) + persistently-mapped upload buffer ─────── */
static void vk_tex_free(Swap *s, VkTex *t)
{
    if (t->view)   vkDestroyImageView(s->dev, t->view, NULL);
    if (t->img)    vkDestroyImage(s->dev, t->img, NULL);
    if (t->mem)    vkFreeMemory(s->dev, t->mem, NULL);
    if (t->up_ptr) vkUnmapMemory(s->dev, t->up_mem);
    if (t->up_buf) vkDestroyBuffer(s->dev, t->up_buf, NULL);
    if (t->up_mem) vkFreeMemory(s->dev, t->up_mem, NULL);
    memset(t, 0, sizeof *t);
}

static int vk_tex_make(Swap *s, VkTex *t, int w, int h)
{
    t->w = w; t->h = h; t->uploaded = 0;
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(s->dev, &ii, NULL, &t->img) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(s->dev, t->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_mem_type(s->pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (vkAllocateMemory(s->dev, &mai, NULL, &t->mem) != VK_SUCCESS) return -1;
    vkBindImageMemory(s->dev, t->img, t->mem, 0);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(s->dev, &vi, NULL, &t->view) != VK_SUCCESS) return -1;

    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    if (vkCreateBuffer(s->dev, &bi, NULL, &t->up_buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(s->dev, t->up_buf, &br);
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = br.size, .memoryTypeIndex = find_mem_type(s->pd, br.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    if (vkAllocateMemory(s->dev, &bmai, NULL, &t->up_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(s->dev, t->up_buf, t->up_mem, 0);
    vkMapMemory(s->dev, t->up_mem, 0, bytes, 0, &t->up_ptr);
    return 0;
}

static void vk_point_desc(Swap *s, VkDescriptorSet set, VkImageView view)
{
    VkDescriptorImageInfo dii = { .sampler = s->samp, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
    vkUpdateDescriptorSets(s->dev, 1, &wr, 0, NULL);
}

/* Ensure the base texture matches the content size (rebuild + re-point descriptor
 * when the aspect toggles at runtime). */
static int vk_ensure_base(Swap *s, int w, int h)
{
    if (s->base.img && s->base.w == w && s->base.h == h) return 0;
    vkDeviceWaitIdle(s->dev);
    vk_tex_free(s, &s->base);
    if (vk_tex_make(s, &s->base, w, h) != 0) return -1;
    vk_point_desc(s, s->dset_base, s->base.view);
    return 0;
}

static int vk_make_render_pass(Swap *s)
{
    VkAttachmentDescription at = { .format = s->sc_format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &at, .subpassCount = 1, .pSubpasses = &sp,
        .dependencyCount = 1, .pDependencies = &dep };
    return vkCreateRenderPass(s->dev, &rci, NULL, &s->rpass) == VK_SUCCESS ? 0 : -1;
}

/* One textured/flat-colour quad pipeline (dynamic viewport+scissor); two
 * descriptor sets (atlas + base). */
static int vk_make_pipeline(Swap *s)
{
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    if (vkCreateSampler(s->dev, &si, NULL, &s->samp) != VK_SUCCESS) return -1;

    VkDescriptorSetLayoutBinding b = { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b };
    if (vkCreateDescriptorSetLayout(s->dev, &li, NULL, &s->dsl) != VK_SUCCESS) return -1;

    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = sizeof(QuadPush) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &s->dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (vkCreatePipelineLayout(s->dev, &pli, NULL, &s->pll) != VK_SUCCESS) return -1;

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (vkCreateDescriptorPool(s->dev, &dci, NULL, &s->dpool) != VK_SUCCESS) return -1;
    VkDescriptorSetLayout layouts[2] = { s->dsl, s->dsl };
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s->dpool, .descriptorSetCount = 2, .pSetLayouts = layouts };
    if (vkAllocateDescriptorSets(s->dev, &dai, sets) != VK_SUCCESS) return -1;
    s->dset_atlas = sets[0]; s->dset_base = sets[1];

    VkShaderModuleCreateInfo vmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_quad_vert_spv, .pCode = k_quad_vert_spv };
    if (vkCreateShaderModule(s->dev, &vmi, NULL, &s->vs) != VK_SUCCESS) return -1;
    VkShaderModuleCreateInfo fmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_quad_frag_spv, .pCode = k_quad_frag_spv };
    if (vkCreateShaderModule(s->dev, &fmi, NULL, &s->fs) != VK_SUCCESS) return -1;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s->fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dys = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &dys, .layout = s->pll, .renderPass = s->rpass };
    if (vkCreateGraphicsPipelines(s->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s->pipe) != VK_SUCCESS) return -1;

    /* Same pipeline but ALPHA blend — the per-character drop shadow (quad mode 2):
     * result = black*srcA + dst*(1-srcA) → darkens the background by the silhouette. */
    VkPipelineColorBlendAttachmentState cba_a = { .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb_a = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba_a };
    gp.pColorBlendState = &cb_a;
    if (vkCreateGraphicsPipelines(s->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s->pipe_quad_shadow) != VK_SUCCESS) return -1;
    return 0;
}

/* Two fullscreen lighting pipelines (blit.vert + fx.frag), blended over the scene:
 * a MULTIPLY pipeline (dst *= brightness — ambient + torch) and an ADDITIVE one
 * (dst += glow — character glow). No descriptor set; FxPush push constants only. */
static int vk_make_fx_pipelines(Swap *s)
{
    VkShaderModuleCreateInfo vmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_blit_vert_spv, .pCode = k_blit_vert_spv };
    if (vkCreateShaderModule(s->dev, &vmi, NULL, &s->fx_vs) != VK_SUCCESS) return -1;
    VkShaderModuleCreateInfo fmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_fx_frag_spv, .pCode = k_fx_frag_spv };
    if (vkCreateShaderModule(s->dev, &fmi, NULL, &s->fx_fs) != VK_SUCCESS) return -1;

    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = sizeof(FxPush) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (vkCreatePipelineLayout(s->dev, &pli, NULL, &s->pll_fx) != VK_SUCCESS) return -1;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s->fx_vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s->fx_fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dys = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };

    /* MULTIPLY blend (dst *= src): the dim pass (ambient darkness + torch glow).
     * The character/sprite glow is a per-sprite additive pass, not here. */
    VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO, .dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &dys, .layout = s->pll_fx, .renderPass = s->rpass };
    if (vkCreateGraphicsPipelines(s->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s->pipe_fx_dim) != VK_SUCCESS) return -1;
    return 0;
}

static int vulkan_init(const char *title, int cw, int ch)
{
    Swap *s = &g_sw;
    memset(s, 0, sizeof *s);

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
    if (vk_make_render_pass(s) != 0) { VKLOG("render pass\n"); goto fail; }
    if (sw_make_targets(s) != 0)     { VKLOG("framebuffers\n"); goto fail; }

    { VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s->qfam };
      vkCreateCommandPool(s->dev, &pci, NULL, &s->cpool);
      VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s->cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
      vkAllocateCommandBuffers(s->dev, &cai, &s->cmd); }

    if (vk_make_pipeline(s) != 0)     { VKLOG("pipeline\n"); goto fail; }
    if (vk_make_fx_pipelines(s) != 0) { VKLOG("fx pipelines\n"); goto fail; }

    /* The sprite atlas is persistent; point its descriptor now. The base texture
     * follows the content size, so it is created lazily at first present
     * (vk_ensure_base) and re-created when the aspect toggles. */
    if (vk_tex_make(s, &s->atlas, VK_ATLAS_W, VK_ATLAS_H) != 0) { VKLOG("atlas\n"); goto fail; }
    vk_point_desc(s, s->dset_atlas, s->atlas.view);

    /* sem_done is per-image, created in sw_make_targets. */
    { VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      vkCreateSemaphore(s->dev, &si, NULL, &s->sem_acquire);
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

/* Record: upload a VkTex's host buffer into its image, leave it SHADER_READ. */
static void vk_tex_upload_cmd(Swap *s, VkTex *t)
{
    VkImageLayout from = t->uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags sa = t->uploaded ? VK_ACCESS_SHADER_READ_BIT : 0;
    VkPipelineStageFlags ss = t->uploaded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    sw_barrier(s->cmd, t->img, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               sa, VK_ACCESS_TRANSFER_WRITE_BIT, ss, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t)t->w, (uint32_t)t->h, 1 } };
    vkCmdCopyBufferToImage(s->cmd, t->up_buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    sw_barrier(s->cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    t->uploaded = 1;
}

/* The renderer composes in CONTENT pixels; the pipeline maps content -> the letterboxed
 * content viewport (s->vp, computed in vk_begin_pass — preserves aspect, never stretches).
 * Scissor rects (given in content px) map into that same viewport rect here. */
static void vk_scissor_content(Swap *s, int cw, int ch, int x, int y, int w, int h)
{
    float sx = s->vp[2] / (float)cw, sy = s->vp[3] / (float)ch;
    int rx = (int)(s->vp[0] + x * sx),       ry = (int)(s->vp[1] + y * sy);
    int rw = (int)(s->vp[0] + (x + w) * sx) - rx;
    int rh = (int)(s->vp[1] + (y + h) * sy) - ry;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > (int)s->extent.width)  rw = (int)s->extent.width  - rx;
    if (ry + rh > (int)s->extent.height) rh = (int)s->extent.height - ry;
    if (rw < 0) rw = 0;
    if (rh < 0) rh = 0;
    VkRect2D sc = { { rx, ry }, { (uint32_t)rw, (uint32_t)rh } };
    vkCmdSetScissor(s->cmd, 0, 1, &sc);
}

static void vk_draw_quad(Swap *s, VkDescriptorSet set, const QuadPush *qp)
{
    vkCmdBindDescriptorSets(s->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pll, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(s->cmd, s->pll, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof *qp, qp);
    vkCmdDraw(s->cmd, 4, 1, 0, 0);
}

/* Shelf-pack every drawable quad into the atlas upload buffer, baking its texels
 * (per-output-row palette resolve). pos[i].w==0 means "not packed". Returns -1 if
 * the atlas overflows (caller falls back to the composed blit). Mirrors
 * scene_sdl.c's atlas_pack_bake. */
typedef struct { int x, y, w, h; } AtlasPos;
static int vk_atlas_pack_bake(Swap *s, const Scene *sc, AtlasPos *pos)
{
    int cur_x = 0, cur_y = 0, shelf_h = 0;
    uint32_t *atlas = (uint32_t *)s->atlas.up_ptr;
    for (int i = 0; i < sc->nquads; i++) {
        const SceneQuad *q = &sc->quads[i];
        pos[i].w = 0;
        if (q->w <= 0 || q->h <= 0) continue;
        if (q->w > VK_ATLAS_W) return -1;
        if (cur_x + q->w > VK_ATLAS_W) { cur_x = 0; cur_y += shelf_h; shelf_h = 0; }
        if (cur_y + q->h > VK_ATLAS_H) return -1;
        pos[i] = (AtlasPos){ cur_x, cur_y, q->w, q->h };
        cur_x += q->w;
        if (q->h > shelf_h) shelf_h = q->h;
        for (int rr = 0; rr < q->h; rr++) {
            int dy = q->y + rr;
            const uint32_t *pal = (dy >= 0 && dy < SCENE_MAX_ROWS) ? sc->pal_rows[dy] : sc->pal_rows[0];
            const uint8_t *srcrow = q->idx + (size_t)rr * q->stride;
            uint32_t *dstrow = atlas + (size_t)(pos[i].y + rr) * VK_ATLAS_W + pos[i].x;
            for (int c = 0; c < q->w; c++) {
                uint8_t v = srcrow[c];
                dstrow[c] = (v == SCENE_TRANSPARENT)
                          ? 0u
                          : (0xFF000000u | (pal[v & (SCENE_PAL - 1)] & 0x00FFFFFFu));
            }
        }
    }
    return 0;
}

/* Begin frame: wait, acquire (rebuild on resize), begin command buffer + render
 * pass on the acquired image. Returns the image index, or -1 to skip the frame. */
static int vk_begin_frame(Swap *s, uint32_t *out_idx)
{
    vk_check_resize(s);   /* render resolution tracks the window size live */
    vkWaitForFences(s->dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(s->dev, s->swap, UINT64_MAX, s->sem_acquire, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { sw_rebuild(s); return -1; }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return -1;
    vkResetFences(s->dev, 1, &s->fence);
    vkResetCommandBuffer(s->cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(s->cmd, &bi);
    *out_idx = idx;
    return 0;
}

/* End frame: end render pass + command buffer, submit, present (rebuild on resize). */
static void vk_end_frame(Swap *s, uint32_t idx)
{
    vkCmdEndRenderPass(s->cmd);
    vkEndCommandBuffer(s->cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo subm = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s->sem_acquire, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &s->cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s->sem_done[idx] };
    vkQueueSubmit(s->queue, 1, &subm, s->fence);
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s->sem_done[idx],
        .swapchainCount = 1, .pSwapchains = &s->swap, .pImageIndices = &idx };
    VkResult pr = vkQueuePresentKHR(s->queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
        sw_rebuild(s);
}

static void vk_begin_pass(Swap *s, uint32_t idx, int cw, int ch)
{
    VkClearValue clr = { .color = { .float32 = { 0, 0, 0, 1 } } };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = s->rpass, .framebuffer = s->fbs[idx],
        .renderArea = { {0,0}, s->extent }, .clearValueCount = 1, .pClearValues = &clr };
    vkCmdBeginRenderPass(s->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    /* Letterbox/pillarbox the content (cw x ch) into the swapchain preserving aspect —
     * NEVER stretch. The render pass clears to black, so the bars are black. */
    float ew = (float)s->extent.width, eh = (float)s->extent.height;
    float ca = (float)cw / (float)ch;
    float vw, vh;
    if (ew / eh > ca) { vh = eh; vw = eh * ca; }   /* window wider → pillarbox (L/R bars) */
    else              { vw = ew; vh = ew / ca; }   /* window taller → letterbox (T/B bars) */
    s->vp[0] = (ew - vw) * 0.5f; s->vp[1] = (eh - vh) * 0.5f;
    s->vp[2] = vw;               s->vp[3] = vh;
    VkViewport vp = { s->vp[0], s->vp[1], vw, vh, 0, 1 };
    vkCmdSetViewport(s->cmd, 0, 1, &vp);
    vk_scissor_content(s, cw, ch, 0, 0, cw, ch);
    vkCmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
}

/* Overlay / fallback path: present the composed surface as one fullscreen quad. */
static void vulkan_present(const uint32_t *argb, int w, int h)
{
    Swap *s = &g_sw;
    if (!s->ok) return;
    if (vk_ensure_base(s, w, h) != 0) return;
    memcpy(s->base.up_ptr, argb, (size_t)w * h * 4);

    uint32_t idx;
    if (vk_begin_frame(s, &idx) != 0) return;
    vk_tex_upload_cmd(s, &s->base);
    vk_begin_pass(s, idx, w, h);

    QuadPush q; memset(&q, 0, sizeof q);
    q.screen[0] = (float)w; q.screen[1] = (float)h; q.mode = 0;
    q.dst[2] = (float)w; q.dst[3] = (float)h;
    q.src[2] = 1.0f; q.src[3] = 1.0f;
    vk_draw_quad(s, s->dset_base, &q);

    vk_end_frame(s, idx);
}

/* BenRen VK — the per-sprite path: draw the Scene draw list on the GPU. */
static void vulkan_present_scene(const Scene *sc, int y_lo, int y_hi,
                                 const uint32_t *base, int w, int h,
                                 const PresentRect *rects, int nrects)
{
    Swap *s = &g_sw;
    if (!s->ok) return;
    if (vk_ensure_base(s, w, h) != 0) return;
    memcpy(s->base.up_ptr, base, (size_t)w * h * 4);

    AtlasPos *pos = malloc(sizeof(AtlasPos) * (size_t)(sc->nquads ? sc->nquads : 1));
    if (!pos) return;
    if (vk_atlas_pack_bake(s, sc, pos) != 0) {   /* atlas overflow: composed fallback */
        free(pos);
        vulkan_present(base, w, h);
        return;
    }

    uint32_t idx;
    if (vk_begin_frame(s, &idx) != 0) { free(pos); return; }
    vk_tex_upload_cmd(s, &s->base);
    vk_tex_upload_cmd(s, &s->atlas);
    vk_begin_pass(s, idx, w, h);

    const float scr0 = (float)w, scr1 = (float)h;

    /* Base layer: the rows the scene does NOT own (top border + HUD). */
    if (y_lo > 0) {
        QuadPush q; memset(&q, 0, sizeof q);
        q.screen[0] = scr0; q.screen[1] = scr1;
        q.dst[2] = (float)w; q.dst[3] = (float)y_lo;
        q.src[2] = 1.0f; q.src[3] = (float)y_lo / (float)h;
        vk_draw_quad(s, s->dset_base, &q);
    }
    if (y_hi < h) {
        QuadPush q; memset(&q, 0, sizeof q);
        q.screen[0] = scr0; q.screen[1] = scr1;
        q.dst[1] = (float)y_hi; q.dst[2] = (float)w; q.dst[3] = (float)(h - y_hi);
        q.src[1] = (float)y_hi / (float)h; q.src[2] = 1.0f; q.src[3] = 1.0f;
        vk_draw_quad(s, s->dset_base, &q);
    }

    /* Per-row void background across the scene rows (COLOR00 run-length bands). */
    for (int y = y_lo; y < y_hi; ) {
        uint32_t c0 = (y >= 0 && y < SCENE_MAX_ROWS) ? sc->pal_rows[y][0] : 0;
        int y1 = y + 1;
        while (y1 < y_hi) {
            uint32_t c1 = (y1 >= 0 && y1 < SCENE_MAX_ROWS) ? sc->pal_rows[y1][0] : 0;
            if ((c1 & 0xFFFFFF) != (c0 & 0xFFFFFF)) break;
            y1++;
        }
        QuadPush q; memset(&q, 0, sizeof q);
        q.screen[0] = scr0; q.screen[1] = scr1; q.mode = 1;
        q.dst[2] = (float)w; q.dst[1] = (float)y; q.dst[3] = (float)(y1 - y);
        q.color[0] = ((c0 >> 16) & 0xFF) / 255.0f;
        q.color[1] = ((c0 >>  8) & 0xFF) / 255.0f;
        q.color[2] = ( c0        & 0xFF) / 255.0f;
        q.color[3] = 1.0f;
        vk_draw_quad(s, s->dset_atlas, &q);   /* mode 1 ignores the bound texture */
        y = y1;
    }

    /* World quads: per-sprite, camera-projected, clipped to camera cols + rows.
     * The faint back-glow (an expanded additive copy of each foreground sprite) is
     * drawn FIRST, so the sprites paint over their own glow and only the soft fringe
     * shows behind them. */
    {
        int cx0 = sc->wclip_x0 - sc->view_left, cx1 = sc->wclip_x1 - sc->view_left;
        if (cx0 < 0)  cx0 = 0;
        if (cx1 > w)  cx1 = w;
        if (cx1 > cx0) {
            vk_scissor_content(s, w, h, cx0, y_lo, cx1 - cx0, y_hi - y_lo);

            int shadow_on = (s->fx.valid && (s->fx.flags & FX_SHADOW));
            const float SH_DX = 3.0f, SH_DY = 3.0f, SH_OPACITY = 0.45f;  /* offset px + darkness */
            for (int i = 0; i < sc->nquads; i++) {
                const SceneQuad *q = &sc->quads[i];
                if (q->space != SCENE_SPACE_WORLD || pos[i].w == 0) continue;
                float u0 = (float)pos[i].x / VK_ATLAS_W, v0 = (float)pos[i].y / VK_ATLAS_H;
                float u1 = (float)(pos[i].x + q->w) / VK_ATLAS_W, v1 = (float)(pos[i].y + q->h) / VK_ATLAS_H;

                /* Drop shadow FIRST, per character — drawn AFTER the tiles/earlier quads
                 * (over the terrain) and BEFORE this sprite, OFFSET down-right so a dark
                 * silhouette sits behind/below the character. */
                if (shadow_on && q->shadow) {
                    QuadPush qs; memset(&qs, 0, sizeof qs);
                    qs.screen[0] = scr0; qs.screen[1] = scr1; qs.mode = 2;
                    qs.color[3] = SH_OPACITY;
                    qs.dst[0] = (float)(q->x - sc->view_left) + SH_DX; qs.dst[1] = (float)q->y + SH_DY;
                    qs.dst[2] = (float)q->w; qs.dst[3] = (float)q->h;
                    qs.src[0] = u0; qs.src[1] = v0; qs.src[2] = u1; qs.src[3] = v1;
                    vkCmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe_quad_shadow);
                    vk_draw_quad(s, s->dset_atlas, &qs);
                    vkCmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
                }

                QuadPush qp; memset(&qp, 0, sizeof qp);
                qp.screen[0] = scr0; qp.screen[1] = scr1;
                qp.dst[0] = (float)(q->x - sc->view_left); qp.dst[1] = (float)q->y;
                qp.dst[2] = (float)q->w; qp.dst[3] = (float)q->h;
                qp.src[0] = u0; qp.src[1] = v0; qp.src[2] = u1; qp.src[3] = v1;
                vk_draw_quad(s, s->dset_atlas, &qp);
            }
        }
    }

    /* Lighting pass (Hardware-only): ambient darkness, blended fullscreen over the
     * world, BEFORE the banner so screen-fixed UI stays bright. */
    {
        int fl = s->fx.valid ? s->fx.flags : 0;
        if (fl & FX_AMBIENT) {
            vk_scissor_content(s, w, h, 0, 0, w, h);
            FxPush fp;
            fp.res[0] = scr0; fp.res[1] = scr1;
            fp.light[0] = (float)s->fx.light_sx; fp.light[1] = (float)s->fx.light_sy;
            fp.pf[0] = (float)s->fx.pf_top;      fp.pf[1] = (float)s->fx.pf_bot;
            fp.flags = fl; fp.mode = 0;
            vkCmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe_fx_dim);
            vkCmdPushConstants(s->cmd, s->pll_fx, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof fp, &fp);
            vkCmdDraw(s->cmd, 3, 1, 0, 0);
        }
    }

    /* Screen-fixed quads (the banner): no camera, full frame, on top. */
    vkCmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);   /* restore the quad pipeline */
    vk_scissor_content(s, w, h, 0, 0, w, h);
    for (int i = 0; i < sc->nquads; i++) {
        const SceneQuad *q = &sc->quads[i];
        if (q->space != SCENE_SPACE_SCREEN || pos[i].w == 0) continue;
        QuadPush qp; memset(&qp, 0, sizeof qp);
        qp.screen[0] = scr0; qp.screen[1] = scr1;
        qp.dst[0] = (float)q->x; qp.dst[1] = (float)q->y;
        qp.dst[2] = (float)q->w; qp.dst[3] = (float)q->h;
        qp.src[0] = (float)pos[i].x / VK_ATLAS_W;          qp.src[1] = (float)pos[i].y / VK_ATLAS_H;
        qp.src[2] = (float)(pos[i].x + q->w) / VK_ATLAS_W; qp.src[3] = (float)(pos[i].y + q->h) / VK_ATLAS_H;
        vk_draw_quad(s, s->dset_atlas, &qp);
    }

    /* PC overlay rects (HUD icons / F3 profiler): sample the composed base
     * texture (which already carries those overlays — hw.c drew them into the
     * composed surface) for just those regions, on top of everything. Drawn
     * after the lighting pass so overlay UI stays bright, like the banner. */
    for (int i = 0; i < nrects; i++) {
        int x0 = rects[i].x, y0 = rects[i].y, rw = rects[i].w, rh = rects[i].h;
        if (x0 < 0) { rw += x0; x0 = 0; }
        if (y0 < 0) { rh += y0; y0 = 0; }
        if (x0 + rw > w) rw = w - x0;
        if (y0 + rh > h) rh = h - y0;
        if (rw <= 0 || rh <= 0) continue;
        QuadPush qp; memset(&qp, 0, sizeof qp);
        qp.screen[0] = scr0; qp.screen[1] = scr1;
        qp.dst[0] = (float)x0; qp.dst[1] = (float)y0;
        qp.dst[2] = (float)rw; qp.dst[3] = (float)rh;
        qp.src[0] = (float)x0 / (float)w; qp.src[1] = (float)y0 / (float)h;
        qp.src[2] = (float)(x0 + rw) / (float)w; qp.src[3] = (float)(y0 + rh) / (float)h;
        vk_draw_quad(s, s->dset_base, &qp);
    }

    free(pos);
    vk_end_frame(s, idx);
}

static void vulkan_set_effects(const FxFrame *fx) { if (fx) g_sw.fx = *fx; }

static SDL_Window *vulkan_window(void) { return g_sw.win; }

static void vulkan_shutdown(void)
{
    Swap *s = &g_sw;
    if (s->dev) vkDeviceWaitIdle(s->dev);
    if (s->fence) vkDestroyFence(s->dev, s->fence, NULL);
    if (s->sem_acquire) vkDestroySemaphore(s->dev, s->sem_acquire, NULL);
    /* sem_done[] is freed by sw_destroy_targets below. */
    if (s->pipe_fx_dim) vkDestroyPipeline(s->dev, s->pipe_fx_dim, NULL);
    if (s->pll_fx) vkDestroyPipelineLayout(s->dev, s->pll_fx, NULL);
    if (s->fx_vs) vkDestroyShaderModule(s->dev, s->fx_vs, NULL);
    if (s->fx_fs) vkDestroyShaderModule(s->dev, s->fx_fs, NULL);
    if (s->pipe_quad_shadow) vkDestroyPipeline(s->dev, s->pipe_quad_shadow, NULL);
    if (s->pipe) vkDestroyPipeline(s->dev, s->pipe, NULL);
    if (s->vs) vkDestroyShaderModule(s->dev, s->vs, NULL);
    if (s->fs) vkDestroyShaderModule(s->dev, s->fs, NULL);
    if (s->pll) vkDestroyPipelineLayout(s->dev, s->pll, NULL);
    if (s->dpool) vkDestroyDescriptorPool(s->dev, s->dpool, NULL);
    if (s->dsl) vkDestroyDescriptorSetLayout(s->dev, s->dsl, NULL);
    if (s->samp) vkDestroySampler(s->dev, s->samp, NULL);
    vk_tex_free(s, &s->atlas);
    vk_tex_free(s, &s->base);
    sw_destroy_targets(s);
    if (s->rpass) vkDestroyRenderPass(s->dev, s->rpass, NULL);
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
    "vulkan", vulkan_init, vulkan_present, vulkan_present_scene,
    vulkan_set_effects,
    vulkan_window, vulkan_shutdown
};

/* Returns the Vulkan backend; present_backend_select() handles the case where
 * vulkan_init() later fails (it returns -1 and hw_init aborts that backend). */
const PresentBackend *present_backend_vulkan(void) { return &VULKAN_BACKEND; }

#endif /* BENEFACTOR_HAVE_VULKAN */
